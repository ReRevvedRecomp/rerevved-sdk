#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/shared_memory.h>

namespace rex::graphics {

// CPU input versions own only watch metadata. All borrowed views must be
// consumed before Reset; no guest bytes or GPU resources are retained here.
class FrameInputBufferVersions {
 public:
  static constexpr size_t kMaxRanges = 4096;

  FrameInputBufferVersions(memory::Memory& memory, SharedMemory& shared_memory)
      : memory_(memory), shared_memory_(shared_memory) {}
  ~FrameInputBufferVersions() { Reset(); }
  FrameInputBufferVersions(const FrameInputBufferVersions&) = delete;
  FrameInputBufferVersions& operator=(const FrameInputBufferVersions&) = delete;

  diagnostic::DrawCaptureGuestMemoryVersion Get(uint32_t address, uint32_t size) {
    auto lock = global_critical_region_.Acquire();
    if (!Readable(address, size)) {
      return {};
    }
    const uint64_t key = (uint64_t(address) << 32) | size;
    auto found = ranges_.find(key);
    if (found == ranges_.end()) {
      // Never evict while assembling a view: another range in that same
      // callback may already borrow an entry. Saturation uses byte reads.
      if (ranges_.size() >= kMaxRanges || next_identity_ == UINT64_MAX) {
        return {};
      }
      auto entry = std::make_unique<Entry>(*this, next_identity_++, address, size);
      found = ranges_.emplace(key, std::move(entry)).first;
    }
    auto& entry = *found->second;
    if (!entry.watch) {
      if (entry.generation == UINT64_MAX) {
        return {};
      }
      ++entry.generation;
      entry.watch =
          shared_memory_.WatchMemoryRange(address, size, &Invalidated, nullptr, &entry, 0);
      // Range watches alone do not protect guest pages. Arm every physical
      // alias before releasing the same lock used by write callbacks.
      memory_.EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
    }
    return {entry.identity, entry.generation, address, size, 0, 0, &entry, &Check};
  }

  void Reset() {
    auto lock = global_critical_region_.Acquire();
    for (auto& [key, entry] : ranges_) {
      if (entry->watch) {
        shared_memory_.UnwatchMemoryRange(entry->watch);
      }
    }
    ranges_.clear();
  }

 private:
  struct Entry {
    Entry(FrameInputBufferVersions& owner, uint64_t identity, uint32_t address, uint32_t size)
        : owner(owner), identity(identity), address(address), size(size) {}
    FrameInputBufferVersions& owner;
    uint64_t identity;
    uint64_t generation = 0;
    uint32_t address;
    uint32_t size;
    SharedMemory::WatchHandle watch = nullptr;
  };

  bool Readable(uint32_t address, uint32_t size) {
    if (!size || address >= SharedMemory::kBufferSize ||
        uint64_t(size) > uint64_t(SharedMemory::kBufferSize) - address) {
      return false;
    }
    auto* heap = memory_.GetPhysicalHeap();
    return heap && memory_.TranslatePhysical(address) &&
           heap->QueryRangeAccess(address, address + size - 1) != memory::PageAccess::kNoAccess &&
           !shared_memory_.IsRangeWrittenByGpu(address, size);
  }

  static bool Check(const void* context, uint64_t generation) noexcept {
    const auto& entry = *static_cast<const Entry*>(context);
    auto lock = entry.owner.global_critical_region_.Acquire();
    // Making pages unreadable does not fire a write watch. Recheck access as
    // well as generation, even when the physical address hasn't changed.
    return entry.watch && entry.generation == generation &&
           entry.owner.Readable(entry.address, entry.size);
  }

  static void Invalidated(const std::unique_lock<std::recursive_mutex>&, void*, void* data,
                          uint64_t, bool) {
    // SharedMemory cancels the watch after this callback; never unwatch here.
    static_cast<Entry*>(data)->watch = nullptr;
  }

  memory::Memory& memory_;
  SharedMemory& shared_memory_;
  thread::global_critical_region global_critical_region_;
  std::unordered_map<uint64_t, std::unique_ptr<Entry>> ranges_;
  // Serialized by the global critical region; resetting a frame cannot reuse
  // an old scalar identity. Exhaustion permanently falls back to byte reads.
  inline static uint64_t next_identity_ = 1;
};

}  // namespace rex::graphics
