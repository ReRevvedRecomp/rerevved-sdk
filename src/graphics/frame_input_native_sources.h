#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/registers.h>
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

  struct Composition {
    uint32_t base = 0;
    uint32_t width = 0;
    uint32_t pitch = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t endian = 0;
    uint32_t x_begin = 0;
    uint32_t x_end = 0;

    bool SameDestination(const Composition& other) const {
      return base == other.base && width == other.width && pitch == other.pitch &&
             height == other.height && format == other.format && endian == other.endian;
    }
  };

  static bool GetPreparedResolvePlan(const diagnostic::DrawCaptureFrameInputCopyView& copy,
                                     Composition& composition) {
    composition = {};
    if (copy.copy_mode != "resolve" || !copy.cpu_data_complete || !copy.planned_address ||
        !copy.planned_length || copy.registers.size() <= XE_GPU_REG_RB_COPY_DEST_INFO) {
      return false;
    }
    reg::RB_COPY_CONTROL control{};
    control.value = copy.registers[XE_GPU_REG_RB_COPY_CONTROL];
    reg::RB_COPY_DEST_INFO destination{};
    destination.value = copy.registers[XE_GPU_REG_RB_COPY_DEST_INFO];
    if (control.copy_src_select != 0 ||
        (control.copy_command != xenos::CopyCommand::kRaw &&
         control.copy_command != xenos::CopyCommand::kConvert) ||
        destination.copy_dest_array ||
        (destination.copy_dest_format != xenos::ColorFormat::k_8_8_8_8 &&
         destination.copy_dest_format != xenos::ColorFormat::k_16_16_FLOAT)) {
      return false;
    }
    reg::RB_COPY_DEST_PITCH pitch{};
    pitch.value = copy.registers[XE_GPU_REG_RB_COPY_DEST_PITCH];
    const bool half_shape = copy.source_rect_present && copy.source_width == 640 &&
                            copy.source_height == 720 && pitch.copy_dest_pitch == 1280 &&
                            pitch.copy_dest_height == 720 &&
                            destination.copy_dest_format == xenos::ColorFormat::k_8_8_8_8;
    if (half_shape) {
      (void)GetHalfComposition(copy, control, destination, composition);
    }
    // CPU-complete planning admits only the exact selected clear-enabled left
    // tuple. Other clear plans must never become standalone native owners. A
    // non-clear plan that fails the half gate is an ordinary standalone plan.
    return (!control.color_clear_enable && !control.depth_clear_enable) || composition.width;
  }

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

  // Production callers supply the copy event ID. Zero is reserved for untagged
  // synchronous callers and cannot participate in a composed destination.
  bool Arm(uint32_t address, uint32_t size, uint64_t generation = 0,
           const Composition* composition = nullptr) {
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
    Entry next;
    next.address = address;
    next.size = size;
    next.members[0] = {address, size, generation, false};
    if (composition) {
      next.composition = *composition;
      next.composed = true;
    }
    Entry* first = nullptr;
    if (composition) {
      // Draws and unrelated resolves may intervene. Overlapping plans already
      // supersede the left member, and its watches reject intervening writes.
      for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
        if (CanCompose(*it, next)) {
          first = &*it;
          break;
        }
      }
    }
    if (first) {
      next.members[1] = next.members[0];
      next.members[0] = first->members[0];
      next.member_count = 2;
      next.address = first->address;
      next.size = uint32_t(uint64_t(address) + size - next.address);
      if (first->state == EntryState::kRejected) {
        next.state = EntryState::kRejected;
      }
    }
    for (const auto& entry : ranges_) {
      if (&entry != first && entry.current &&
          Overlaps(next.address, next.size, entry.address, entry.size) &&
          next.address != entry.address && !Contains(next, entry)) {
        next.state = EntryState::kRejected;
      }
    }
    // Erase contained history before testing capacity. Uncovered rejection
    // tails survive, but exact repeats and alias cycles cannot accumulate.
    for (auto it = ranges_.begin(); it != ranges_.end();) {
      auto& entry = *it;
      if (Overlaps(next.address, next.size, entry.address, entry.size)) {
        entry.current = false;
        Unwatch(entry);
      }
      if (Contains(next, entry)) {
        Unwatch(entry);
        it = ranges_.erase(it);
      } else {
        ++it;
      }
    }
    if (ranges_.size() >= kMaxRanges) {
      FailClosed();
      RecordRejected(next.address, next.size);
      return false;
    }
    try {
      ranges_.push_back(next);
    } catch (...) {
      FailClosed();
      RecordRejected(address, size);
      return false;
    }
    if (next.state == EntryState::kRejected) {
      RecordRejected(next.address, next.size);
      return false;
    }
    // Register before the lock is released, matching the existing CPU input
    // version owner. The callback itself clamps SharedMemory's widening to
    // the incoming system-page interval while any entry remains live.
    try {
      memory_.EnablePhysicalMemoryAccessCallbacks(next.address, next.size, true, false);
      return true;
    } catch (...) {
      auto lock = global_critical_region_.Acquire();
      FailClosed();
      RecordRejected(address, size);
      return false;
    }
  }

  // Keep only successful original resolves; arm SharedMemory afterward so the
  // planned GPU write is not treated as a later overwrite. Nonzero generation
  // guards delayed completion/address reuse; zero means current plan.
  bool Complete(uint32_t address, uint32_t size, bool original_succeeded, uint64_t generation = 0) {
    auto lock = global_critical_region_.Acquire();
    if (failed_) {
      return false;
    }
    auto* entry = FindCurrent(address, size, generation);
    if (!entry) {
      return false;
    }
    if (!original_succeeded) {
      RejectEntry(*entry);
      return false;
    }
    if (entry->state == EntryState::kRejected) {
      return false;
    }
    for (auto& member : entry->members) {
      if (Matches(member, address, size, generation)) {
        member.completed = true;
      }
    }
    if (entry->member_count == 2 &&
        (!entry->members[0].completed || !entry->members[1].completed)) {
      return true;
    }
    if (entry->state == EntryState::kArmed) {
      entry->state = EntryState::kNative;
      try {
        if (!ArmSharedWatch(*entry)) {
          FailClosed();
          return false;
        }
      } catch (...) {
        FailClosed();
        return false;
      }
      if (rejections_ && (!entry->composed || entry->member_count == 2)) {
        rejections_->ClearExact(entry->address, entry->size);
      }
    }
    return entry->state == EntryState::kNative;
  }

  // Retires a plan whose original command never reached a submitted owner.
  // Keep the rejected marker until the observer is replaced or stopped so a
  // later CPU fallback cannot consume stale bytes from the failed plan.
  void Reject(uint32_t address, uint32_t size, uint64_t generation = 0) {
    auto lock = global_critical_region_.Acquire();
    if (auto* entry = FindCurrent(address, size, generation)) {
      RejectEntry(*entry);
    }
  }

  // Exact physical ranges; no overlap, labels, or format/completion proof.
  // Bound draw identities are immutable. Pairs map right ID to generation and
  // left ID to left_generation; otherwise left_generation = 0.
  State Classify(uint32_t address, uint32_t size, uint64_t* generation = nullptr,
                 uint64_t* left_generation = nullptr) {
    auto lock = global_critical_region_.Acquire();
    if (generation) {
      *generation = 0;
    }
    if (left_generation) {
      *left_generation = 0;
    }
    if (failed_) {
      return State::kRejected;
    }
    for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
      auto& entry = *it;
      if (!Overlaps(address, size, entry.address, entry.size)) {
        continue;
      }
      if (entry.current && entry.address == address && entry.size == size &&
          entry.state == EntryState::kNative && (!entry.composed || entry.member_count == 2)) {
        if (!Readable(entry.address, entry.size)) {
          RejectEntry(entry);
          return State::kRejected;
        }
        if (generation) {
          *generation = entry.members[entry.member_count - 1].generation;
        }
        if (left_generation && entry.member_count == 2) {
          *left_generation = entry.members[0].generation;
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

  // Fill only views admitted by the prepared builder. Recheck ownership at the
  // binding boundary so a rejected range never receives a producer identity.
  bool BindTextureView(diagnostic::DrawCaptureFrameInputTextureView& view) {
    view.native_source_generation = view.native_source_left_generation = 0;
    if (view.available ||
        view.unavailable_reason != diagnostic::DrawCaptureReason::kTextureUnresolved) {
      return true;
    }
    if (Classify(view.base_address, view.base_size, &view.native_source_generation,
                 &view.native_source_left_generation) != State::kUnresolved ||
        !view.native_source_generation) {
      view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
      return false;
    }
    return true;
  }

  bool failed() const {
    auto lock = global_critical_region_.Acquire();
    return failed_;
  }

  size_t range_count() const {
    auto lock = global_critical_region_.Acquire();
    return ranges_.size();
  }

  void Reset() {
    auto lock = global_critical_region_.Acquire();
    for (auto& entry : ranges_) {
      if (entry.watch) {
        shared_memory_.UnwatchMemoryRange(entry.watch);
        entry.watch = nullptr;
      }
    }
    ranges_.clear();
    failed_ = callback_handle_ == nullptr;
  }

 private:
  // Strict 640x720 half of the 1280x720 Rgba8 scene destination. Leaves
  // composition empty when any check fails, so the caller can fall back to
  // standalone admission.
  static bool GetHalfComposition(const diagnostic::DrawCaptureFrameInputCopyView& copy,
                                 const reg::RB_COPY_CONTROL& control,
                                 const reg::RB_COPY_DEST_INFO& destination,
                                 Composition& composition) {
    composition = {};
    // Identify the logical half from the guarded guest rectangle, before
    // attachment-local normalization. Window/scissor setup may vary while
    // producing the same planned rectangle and physical range.
    if (copy.registers.size() <= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1) {
      return false;
    }
    xenos::xe_gpu_vertex_fetch_t fetch{};
    std::memcpy(&fetch, &copy.registers[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(fetch));
    std::array<float, 6> vertices;
    if (!copy.source_geometry.memory.Copy(
            copy.source_geometry.physical_address,
            {reinterpret_cast<uint8_t*>(vertices.data()), sizeof(vertices)})) {
      return false;
    }
    for (auto& vertex : vertices) {
      vertex = xenos::GpuSwap(vertex, fetch.endian);
    }
    const bool right = vertices[0] == 639.5F;
    const float x = right ? 640.0F : 0.0F;
    const std::array<float, 6> rectangle{x - 0.5F, -0.5F, x + 639.5F, -0.5F, x + 639.5F, 719.5F};
    reg::RB_SURFACE_INFO surface{};
    surface.value = copy.registers[XE_GPU_REG_RB_SURFACE_INFO];
    const auto base = copy.registers[XE_GPU_REG_RB_COPY_DEST_BASE];
    // source_x/y are sub-tile EDRAM offsets and planned_address is a 32x32 tile
    // lower bound; neither gives texel placement. Require planner destination
    // origin zero or the union overclaims rows/columns this half did not write.
    if (vertices != rectangle || copy.source_x || copy.source_y || copy.copy_dest_offset_x ||
        copy.copy_dest_offset_y ||
        (control.value != 0x00100040U && (right || control.value != 0x00100340U)) ||
        destination.value != 0x01000300U || surface.msaa_samples != xenos::MsaaSamples::k2X ||
        copy.planned_length != 0x384000 || copy.planned_address != base ||
        (right && base < 0x14000)) {
      return false;
    }
    composition = {base - (right ? 0x14000U : 0U),
                   1280,
                   1280,
                   720,
                   uint32_t(destination.copy_dest_format),
                   uint32_t(destination.copy_dest_endian),
                   right ? 640U : 0U,
                   right ? 1280U : 640U};
    return true;
  }

  enum class EntryState : uint8_t {
    kArmed,
    kNative,
    kRejected,
  };

  struct Member {
    uint32_t address = 0;
    uint32_t size = 0;
    uint64_t generation = 0;
    bool completed = false;
  };

  struct Entry {
    uint32_t address = 0;
    uint32_t size = 0;
    EntryState state = EntryState::kArmed;
    SharedMemory::WatchHandle watch = nullptr;
    bool current = true;
    bool composed = false;
    Composition composition;
    std::array<Member, 2> members;
    size_t member_count = 1;
  };

  static bool Contains(const Entry& outer, const Entry& inner) {
    return outer.address <= inner.address &&
           uint64_t(outer.address) + outer.size >= uint64_t(inner.address) + inner.size;
  }

  static bool CanCompose(const Entry& left, const Entry& right) {
    const auto& a = left.composition;
    const auto& b = right.composition;
    const uint64_t left_id = left.members[0].generation;
    return left.current && left.composed && left.member_count == 1 && left_id &&
           right.members[0].generation > left_id && a.SameDestination(b) && a.width &&
           a.width <= a.pitch && a.height && a.x_begin == 0 && a.x_end > 0 &&
           a.x_end == b.x_begin && b.x_begin < b.x_end && b.x_end == b.width &&
           left.address == a.base && right.address > left.address &&
           Overlaps(left.address, left.size, right.address, right.size) &&
           uint64_t(right.address) + right.size > uint64_t(left.address) + left.size;
  }

  static bool Matches(const Member& member, uint32_t address, uint32_t size, uint64_t generation) {
    return member.address == address && member.size == size &&
           (!generation || member.generation == generation);
  }

  Entry* FindCurrent(uint32_t address, uint32_t size, uint64_t generation) {
    for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
      auto& entry = *it;
      if (entry.current) {
        for (size_t index = 0; index < entry.member_count; ++index) {
          if (Matches(entry.members[index], address, size, generation)) {
            return &entry;
          }
        }
      }
    }
    return nullptr;
  }

  void Unwatch(Entry& entry) {
    if (entry.watch) {
      shared_memory_.UnwatchMemoryRange(entry.watch);
      entry.watch = nullptr;
    }
  }

  void RejectEntry(Entry& entry) {
    entry.state = EntryState::kRejected;
    RecordRejected(entry.address, entry.size);
  }

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
    for (auto& entry : ranges_) {
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
      for (const auto& entry : ranges_) {
        (void)rejections_->Add(entry.address, entry.size);
      }
    }
    for (auto& entry : ranges_) {
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
      auto& entry = *it;
      if (Overlaps(physical_address_start, length, entry.address, entry.size)) {
        // exact_range=false is emitted before the faulting CPU store, so it
        // cannot prove that the whole range was replaced. Retain a rejected
        // marker for both writes and protection/lifetime invalidations.
        self->RejectEntry(entry);
      } else if (entry.current && !entry.watch && entry.state == EntryState::kNative) {
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

  static void SharedMemoryInvalidated(const std::unique_lock<std::recursive_mutex>&, void* context,
                                      void* data, uint64_t, bool invalidated_by_gpu) {
    auto& entry = *static_cast<Entry*>(data);
    // SharedMemory cancels the watch after this callback. CPU/protection
    // invalidations are classified by the physical callback using the
    // incoming interval; a GPU event has no CPU payload and rejects now.
    entry.watch = nullptr;
    if (invalidated_by_gpu) {
      static_cast<FrameInputNativeSources*>(context)->RejectEntry(entry);
    }
  }

  memory::Memory& memory_;
  SharedMemory& shared_memory_;
  FrameInputNativeSourceRejections* rejections_ = nullptr;
  void* callback_handle_ = nullptr;
  mutable thread::global_critical_region global_critical_region_;
  // Submission order is independent of the caller's identity. List nodes keep
  // watch callback data stable while contained history is erased.
  std::list<Entry> ranges_;
  bool failed_ = false;
};

}  // namespace rex::graphics
