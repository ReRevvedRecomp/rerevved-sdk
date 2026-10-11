#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/system/xmemory.h>

namespace rex::graphics {

// Rejected native plans outlive one FrameInputNativeSources generation. This
// is diagnostic metadata only: it never owns guest bytes or GPU resources.
class FrameInputNativeSourceRejections final {
 public:
  static constexpr size_t kMaxRanges = 4096;

  bool Add(uint32_t address, uint32_t size) noexcept {
    std::lock_guard lock(mutex_);
    if (failed_) {
      return false;
    }
    if (!ValidRange(address, size)) {
      failed_ = true;
      return false;
    }
    for (const auto& range : ranges_) {
      if (address == range.first && size == range.second) {
        return true;
      }
    }
    if (ranges_.size() >= kMaxRanges) {
      failed_ = true;
      return false;
    }
    try {
      ranges_.emplace_back(address, size);
      return true;
    } catch (...) {
      failed_ = true;
      return false;
    }
  }

  bool Overlaps(uint32_t address, uint32_t size) const noexcept {
    std::lock_guard lock(mutex_);
    if (failed_ || !ValidRange(address, size)) {
      return true;
    }
    for (const auto& range : ranges_) {
      if (Overlaps(address, size, range.first, range.second)) {
        return true;
      }
    }
    return false;
  }

  // A successful new native generation may retire an older exact rejection.
  // The active source remains the authority; its destructor re-adds the range
  // before the borrowed owner can be replaced.
  void ClearExact(uint32_t address, uint32_t size) noexcept {
    std::lock_guard lock(mutex_);
    if (failed_) {
      return;
    }
    for (auto it = ranges_.begin(); it != ranges_.end(); ++it) {
      if (it->first == address && it->second == size) {
        ranges_.erase(it);
        return;
      }
    }
  }

  void FailClosed() noexcept {
    std::lock_guard lock(mutex_);
    failed_ = true;
  }

 private:
  static bool ValidRange(uint32_t address, uint32_t size) noexcept {
    return size && address < SharedMemory::kBufferSize &&
           uint64_t(size) <= uint64_t(SharedMemory::kBufferSize) - address;
  }

  static bool Overlaps(uint32_t first_address, uint32_t first_size, uint32_t second_address,
                       uint32_t second_size) noexcept {
    const uint64_t first_end = uint64_t(first_address) + first_size;
    const uint64_t second_end = uint64_t(second_address) + second_size;
    return uint64_t(first_address) < second_end && uint64_t(second_address) < first_end;
  }

  mutable std::mutex mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> ranges_;
  bool failed_ = false;
};

// Tracks only the physical ranges of prepared resolves. It never owns guest
// bytes or a host resource. A range is eligible for the next prepared draw
// only while its physical aliases and SharedMemory ownership remain valid.
class FrameInputNativeSources {
 public:
  static constexpr size_t kMaxRanges = FrameInputNativeSourceRejections::kMaxRanges;

  enum class State : uint8_t {
    kNone,
    kUnresolved,
    kRejected,
  };

  FrameInputNativeSources(memory::Memory& memory, SharedMemory& shared_memory,
                          FrameInputNativeSourceRejections* rejections = nullptr)
      : memory_(memory), shared_memory_(shared_memory), rejections_(rejections) {
    callback_handle_ =
        memory_.RegisterPhysicalMemoryInvalidationCallback(&PhysicalMemoryInvalidated, this);
    failed_ = callback_handle_ == nullptr;
  }

  ~FrameInputNativeSources() {
    RetireGeneration();
    if (callback_handle_) {
      memory_.UnregisterPhysicalMemoryInvalidationCallback(callback_handle_);
      callback_handle_ = nullptr;
    }
  }

  FrameInputNativeSources(const FrameInputNativeSources&) = delete;
  FrameInputNativeSources& operator=(const FrameInputNativeSources&) = delete;

  // Arms physical alias observation for a planned destination range. A
  // second overlapping plan fails closed and retires both native candidates.
  bool Arm(uint32_t address, uint32_t size) {
    if (!ValidRange(address, size)) {
      auto lock = global_critical_region_.Acquire();
      FailClosed();
      RecordRejected(address, size);
      return false;
    }
    auto lock = global_critical_region_.Acquire();
    if (failed_) {
      return false;
    }
    bool overlaps_existing = false;
    for (auto& [key, entry] : ranges_) {
      if (Overlaps(address, size, entry.address, entry.size)) {
        entry.state = EntryState::kRejected;
        overlaps_existing = true;
      }
    }
    if (overlaps_existing) {
      FailClosed();
      RecordRejected(address, size);
      return false;
    }
    if (ranges_.size() >= kMaxRanges) {
      FailClosed();
      RecordRejected(address, size);
      return false;
    }
    try {
      auto [found, inserted] =
          ranges_.emplace(address, Entry{address, size, EntryState::kArmed, nullptr});
      if (!inserted) {
        found->second.state = EntryState::kRejected;
        FailClosed();
        RecordRejected(address, size);
        return false;
      }
    } catch (...) {
      FailClosed();
      RecordRejected(address, size);
      return false;
    }
    // Register before the lock is released, matching the existing CPU input
    // version owner. The callback itself clamps SharedMemory's widening to
    // the incoming system-page interval while any entry remains live.
    try {
      memory_.EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
      return true;
    } catch (...) {
      auto lock = global_critical_region_.Acquire();
      if (auto found = ranges_.find(address);
          found != ranges_.end() && found->second.size == size) {
        found->second.state = EntryState::kRejected;
      }
      FailClosed();
      RecordRejected(address, size);
      return false;
    }
  }

  // Records the result of the actual original resolve. Only a successful
  // outcome retains the exact planned range as a prospective native source;
  // a failed original is rejected. Arm the SharedMemory watch only now so
  // the planned resolve's own GPU write is not mistaken for a later overwrite.
  bool Complete(uint32_t address, uint32_t size, bool original_succeeded) {
    auto lock = global_critical_region_.Acquire();
    if (failed_) {
      return false;
    }
    auto found = ranges_.find(address);
    if (found == ranges_.end() || found->second.size != size) {
      return false;
    }
    if (!original_succeeded) {
      found->second.state = EntryState::kRejected;
      RecordRejected(address, size);
      return false;
    }
    if (found->second.state == EntryState::kArmed) {
      found->second.state = EntryState::kNative;
      try {
        if (!ArmSharedWatch(found->second)) {
          FailClosed();
          return false;
        }
      } catch (...) {
        FailClosed();
        return false;
      }
      if (original_succeeded && rejections_) {
        rejections_->ClearExact(address, size);
      }
    }
    return found->second.state == EntryState::kNative;
  }

  // Retires a plan whose original command never reached a submitted owner.
  // Keep the rejected marker until the observer is replaced or stopped so a
  // later CPU fallback cannot consume stale bytes from the failed plan.
  void Reject(uint32_t address, uint32_t size) {
    auto lock = global_critical_region_.Acquire();
    auto found = ranges_.find(address);
    if (found != ranges_.end() && found->second.size == size) {
      found->second.state = EntryState::kRejected;
      RecordRejected(address, size);
    }
  }

  // Requires an exact physical range match. This deliberately does not
  // infer a native source from an overlapping page or a label alone.
  State Classify(uint32_t address, uint32_t size) {
    auto lock = global_critical_region_.Acquire();
    if (failed_) {
      return State::kRejected;
    }
    for (auto& [key, entry] : ranges_) {
      if (!Overlaps(address, size, entry.address, entry.size)) {
        continue;
      }
      if (entry.address == address && entry.size == size && entry.state == EntryState::kNative) {
        if (!Readable(entry.address, entry.size)) {
          entry.state = EntryState::kRejected;
          return State::kRejected;
        }
        return State::kUnresolved;
      }
      return State::kRejected;
    }
    if (rejections_ && rejections_->Overlaps(address, size)) {
      return State::kRejected;
    }
    if (!ValidRange(address, size)) {
      return State::kNone;
    }
    return State::kNone;
  }

  void Reset() {
    auto lock = global_critical_region_.Acquire();
    for (auto& [key, entry] : ranges_) {
      if (entry.watch) {
        shared_memory_.UnwatchMemoryRange(entry.watch);
        entry.watch = nullptr;
      }
    }
    ranges_.clear();
    failed_ = callback_handle_ == nullptr;
  }

 private:
  enum class EntryState : uint8_t {
    kArmed,
    kNative,
    kRejected,
  };

  struct Entry {
    uint32_t address;
    uint32_t size;
    EntryState state;
    SharedMemory::WatchHandle watch;
  };

  bool ArmSharedWatch(Entry& entry) {
    entry.watch = shared_memory_.WatchMemoryRange(entry.address, entry.size,
                                                  &SharedMemoryInvalidated, this, &entry, 0);
    return entry.watch != nullptr;
  }

  static bool ValidRange(uint32_t address, uint32_t size) {
    return size && address < SharedMemory::kBufferSize &&
           uint64_t(size) <= uint64_t(SharedMemory::kBufferSize) - address;
  }

  static bool Overlaps(uint32_t first_address, uint32_t first_size, uint32_t second_address,
                       uint32_t second_size) {
    const uint64_t first_end = uint64_t(first_address) + first_size;
    const uint64_t second_end = uint64_t(second_address) + second_size;
    return uint64_t(first_address) < second_end && uint64_t(second_address) < first_end;
  }

  bool Readable(uint32_t address, uint32_t size) const {
    if (!ValidRange(address, size)) {
      return false;
    }
    auto* heap = memory_.GetPhysicalHeap();
    return heap && memory_.TranslatePhysical(address) &&
           heap->QueryRangeAccess(address, address + size - 1) != memory::PageAccess::kNoAccess;
  }

  void FailClosed() noexcept {
    failed_ = true;
    for (auto& [key, entry] : ranges_) {
      entry.state = EntryState::kRejected;
    }
    if (rejections_) {
      rejections_->FailClosed();
    }
  }

  void RecordRejected(uint32_t address, uint32_t size) noexcept {
    if (rejections_) {
      (void)rejections_->Add(address, size);
    }
  }

  void RetireGeneration() {
    auto lock = global_critical_region_.Acquire();
    if (rejections_) {
      if (failed_) {
        rejections_->FailClosed();
      }
      for (const auto& [key, entry] : ranges_) {
        (void)rejections_->Add(entry.address, entry.size);
      }
    }
    for (auto& [key, entry] : ranges_) {
      if (entry.watch) {
        shared_memory_.UnwatchMemoryRange(entry.watch);
        entry.watch = nullptr;
      }
    }
    ranges_.clear();
    failed_ = callback_handle_ == nullptr;
  }

  static std::pair<uint32_t, uint32_t> PhysicalMemoryInvalidated(void* context,
                                                                 uint32_t physical_address_start,
                                                                 uint32_t length,
                                                                 bool exact_range) {
    (void)exact_range;
    auto* self = static_cast<FrameInputNativeSources*>(context);
    auto lock = self->global_critical_region_.Acquire();
    for (auto it = self->ranges_.begin(); it != self->ranges_.end();) {
      auto& entry = it->second;
      if (Overlaps(physical_address_start, length, entry.address, entry.size)) {
        // exact_range=false is emitted before the faulting CPU store, so it
        // cannot prove that the whole range was replaced. Retain a rejected
        // marker for both writes and protection/lifetime invalidations.
        entry.state = EntryState::kRejected;
      } else if (!entry.watch && entry.state == EntryState::kNative) {
        try {
          if (!self->ArmSharedWatch(entry)) {
            self->failed_ = true;
          }
        } catch (...) {
          self->failed_ = true;
        }
      }
      ++it;
    }
    if (self->failed_) {
      self->FailClosed();
    }
    // A live pending range must not be lost when SharedMemory widens the
    // incoming interval to neighbouring GPU pages. Returning the incoming
    // interval keeps those aliases watched for future writes.
    if (!self->ranges_.empty()) {
      return {physical_address_start, length};
    }
    return {0, std::numeric_limits<uint32_t>::max()};
  }

  static void SharedMemoryInvalidated(const std::unique_lock<std::recursive_mutex>&, void*,
                                      void* data, uint64_t, bool invalidated_by_gpu) {
    auto& entry = *static_cast<Entry*>(data);
    // SharedMemory cancels the watch after this callback. CPU/protection
    // invalidations are classified by the physical callback using the
    // incoming interval; a GPU event has no CPU payload and rejects now.
    entry.watch = nullptr;
    if (invalidated_by_gpu) {
      entry.state = EntryState::kRejected;
    }
  }

  memory::Memory& memory_;
  SharedMemory& shared_memory_;
  FrameInputNativeSourceRejections* rejections_ = nullptr;
  void* callback_handle_ = nullptr;
  mutable thread::global_critical_region global_critical_region_;
  std::map<uint32_t, Entry> ranges_;
  bool failed_ = false;
};

}  // namespace rex::graphics
