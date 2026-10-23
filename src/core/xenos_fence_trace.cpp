#include <rex/graphics/xenos_fence_trace.h>

#include <rex/thread.h>

#include <algorithm>
#include <fstream>
#include <string_view>
#include <vector>

namespace rex::graphics::diagnostic {

namespace {

thread_local const XenosFenceTrace* active_callback_trace = nullptr;

std::string_view PointName(XenosFenceTracePoint point) noexcept {
  switch (point) {
    case XenosFenceTracePoint::kTraceStarted:
      return "trace_started";
    case XenosFenceTracePoint::kTraceReset:
      return "trace_reset";
    case XenosFenceTracePoint::kRingInitialized:
      return "ring_initialized";
    case XenosFenceTracePoint::kRingMappingObserved:
      return "ring_mapping_observed";
    case XenosFenceTracePoint::kReadPointerWritebackConfigured:
      return "read_pointer_writeback_configured";
    case XenosFenceTracePoint::kWatchRegistered:
      return "watch_registered";
    case XenosFenceTracePoint::kWritePointerPublished:
      return "write_pointer_published";
    case XenosFenceTracePoint::kConsumerRangeBegin:
      return "consumer_range_begin";
    case XenosFenceTracePoint::kSwapDecoded:
      return "swap_decoded";
    case XenosFenceTracePoint::kD3D12Submission:
      return "d3d12_submission";
    case XenosFenceTracePoint::kD3D12FenceCompleted:
      return "d3d12_fence_completed";
    case XenosFenceTracePoint::kConsumerRangeEnd:
      return "consumer_range_end";
    case XenosFenceTracePoint::kReadPointerWriteback:
      return "read_pointer_writeback";
    case XenosFenceTracePoint::kWatchNotDecoded:
      return "watch_not_decoded";
    case XenosFenceTracePoint::kWatchTimedOut:
      return "watch_timed_out";
    case XenosFenceTracePoint::kTraceFinished:
      return "trace_finished";
  }
  return "unknown";
}

uint32_t SaturatingIncrement(std::atomic<uint32_t>& value) noexcept {
  uint32_t current = value.load(std::memory_order_relaxed);
  while (current != UINT32_MAX &&
         !value.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {}
  return current;
}

}  // namespace

uint64_t XenosFenceTrace::MonotonicNanoseconds() noexcept {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool XenosFenceTrace::RangeContainsIndex(uint32_t start, uint32_t end, uint32_t index,
                                         uint32_t capacity_dwords) noexcept {
  if (!capacity_dwords) {
    return false;
  }
  start %= capacity_dwords;
  end %= capacity_dwords;
  index %= capacity_dwords;
  uint32_t distance = (end + capacity_dwords - start) % capacity_dwords;
  uint32_t position = (index + capacity_dwords - start) % capacity_dwords;
  return position < distance;
}

void XenosFenceTrace::UpdateMaximum(std::atomic<uint64_t>& target, uint64_t value) noexcept {
  uint64_t current = target.load(std::memory_order_relaxed);
  while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {}
}

bool XenosFenceTrace::TryAdmitCallback(uint64_t& monotonic_nanoseconds,
                                       uint32_t& thread_id) noexcept {
  uint64_t admission = admission_state_.load(std::memory_order_acquire);
  if (!(admission & kAdmissionOpen)) {
    return false;
  }
  monotonic_nanoseconds = MonotonicNanoseconds();
  thread_id = rex::thread::current_thread_id();
  if (active_callback_trace == this) {
    SaturatingIncrement(reentry_failures_);
    return false;
  }
  while (admission & kAdmissionOpen) {
    const uint64_t active_count = admission & kAdmissionCountMask;
    if (active_count == kAdmissionCountMask) {
      SaturatingIncrement(dropped_callbacks_);
      return false;
    }
    if (admission_state_.compare_exchange_weak(admission, admission + 1, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

XenosFenceTrace::ReadPointerWritebackAdmission::ReadPointerWritebackAdmission(
    XenosFenceTrace& trace) noexcept {
  if (trace.TryAdmitCallback(monotonic_nanoseconds_, thread_id_)) {
    trace_ = &trace;
  }
}

XenosFenceTrace::ReadPointerWritebackAdmission::~ReadPointerWritebackAdmission() noexcept {
  if (trace_) {
    SaturatingIncrement(trace_->dropped_callbacks_);
    trace_->FinishCallback();
  }
}

XenosFenceTrace::ReadPointerWritebackAdmission::ReadPointerWritebackAdmission(
    ReadPointerWritebackAdmission&& other) noexcept
    : trace_(other.trace_),
      monotonic_nanoseconds_(other.monotonic_nanoseconds_),
      thread_id_(other.thread_id_) {
  other.trace_ = nullptr;
}

XenosFenceTrace::ReadPointerWritebackAdmission&
XenosFenceTrace::ReadPointerWritebackAdmission::operator=(
    ReadPointerWritebackAdmission&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (trace_) {
    SaturatingIncrement(trace_->dropped_callbacks_);
    trace_->FinishCallback();
  }
  trace_ = other.trace_;
  monotonic_nanoseconds_ = other.monotonic_nanoseconds_;
  thread_id_ = other.thread_id_;
  other.trace_ = nullptr;
  return *this;
}

XenosFenceTrace::CallbackScope::CallbackScope(XenosFenceTrace& trace) noexcept : trace_(trace) {
  admitted_ = trace_.TryAdmitCallback(monotonic_nanoseconds_, thread_id_);
  if (!admitted_) {
    return;
  }

  previous_callback_trace_ = active_callback_trace;
  active_callback_trace = &trace_;
  state_lock_ = std::unique_lock<std::mutex>(trace_.state_mutex_, std::defer_lock);
  if (!state_lock_.try_lock()) {
    SaturatingIncrement(trace_.lock_waits_);
    const uint64_t wait_started = MonotonicNanoseconds();
    state_lock_.lock();
    UpdateMaximum(trace_.maximum_lock_wait_nanoseconds_, MonotonicNanoseconds() - wait_started);
  }
}

XenosFenceTrace::CallbackScope::CallbackScope(XenosFenceTrace& trace,
                                              ReadPointerWritebackAdmission& admission) noexcept
    : trace_(trace) {
  if (admission.trace_ != &trace_) {
    return;
  }
  admitted_ = true;
  monotonic_nanoseconds_ = admission.monotonic_nanoseconds_;
  thread_id_ = admission.thread_id_;
  admission.trace_ = nullptr;

  if (active_callback_trace == &trace_) {
    SaturatingIncrement(trace_.reentry_failures_);
    trace_.FinishCallback();
    admitted_ = false;
    return;
  }
  previous_callback_trace_ = active_callback_trace;
  active_callback_trace = &trace_;
  state_lock_ = std::unique_lock<std::mutex>(trace_.state_mutex_, std::defer_lock);
  if (!state_lock_.try_lock()) {
    SaturatingIncrement(trace_.lock_waits_);
    const uint64_t wait_started = MonotonicNanoseconds();
    state_lock_.lock();
    UpdateMaximum(trace_.maximum_lock_wait_nanoseconds_, MonotonicNanoseconds() - wait_started);
  }
}

XenosFenceTrace::CallbackScope::~CallbackScope() {
  if (!admitted_) {
    return;
  }
  state_lock_.unlock();
  active_callback_trace = previous_callback_trace_;
  trace_.FinishCallback();
}

void XenosFenceTrace::FinishCallback() noexcept {
  const uint64_t previous = admission_state_.fetch_sub(1, std::memory_order_acq_rel);
  if ((previous & kAdmissionCountMask) == 1) {
    std::lock_guard admission_lock(admission_mutex_);
    admission_drained_.notify_all();
  }
}

bool XenosFenceTrace::Start(const std::filesystem::path& output_path) {
  if (active_callback_trace == this) {
    SaturatingIncrement(reentry_failures_);
    return false;
  }
  {
    std::lock_guard admission_lock(admission_mutex_);
    if (enabled() || start_in_progress_ || flush_in_progress_ || output_path.empty()) {
      return false;
    }
    start_in_progress_ = true;
  }

  std::error_code output_error;
  if (std::filesystem::exists(output_path, output_error) || output_error) {
    std::lock_guard admission_lock(admission_mutex_);
    start_in_progress_ = false;
    return false;
  }
  std::filesystem::path temporary_path = output_path;
  temporary_path += ".tmp";
  const auto temporary_status = std::filesystem::symlink_status(temporary_path, output_error);
  if ((output_error && output_error != std::errc::no_such_file_or_directory) ||
      (!output_error && temporary_status.type() != std::filesystem::file_type::not_found)) {
    std::lock_guard admission_lock(admission_mutex_);
    start_in_progress_ = false;
    return false;
  }

  bool started = false;
  {
    std::lock_guard state_lock(state_mutex_);
    output_path_ = output_path;
    for (Slot& slot : slots_) {
      slot.committed.store(false, std::memory_order_relaxed);
      slot.event = {};
    }
    watches_ = {};
    next_slot_.store(0, std::memory_order_relaxed);
    overflow_.store(0, std::memory_order_relaxed);
    lock_waits_.store(0, std::memory_order_relaxed);
    maximum_lock_wait_nanoseconds_.store(0, std::memory_order_relaxed);
    dropped_callbacks_.store(0, std::memory_order_relaxed);
    reentry_failures_.store(0, std::memory_order_relaxed);
    next_sequence_.store(0, std::memory_order_relaxed);
    epoch_ = 1;
    ring_generation_ = 0;
    ring_guest_virtual_base_ = 0;
    ring_physical_base_ = 0;
    ring_capacity_bytes_ = 0;
    ring_capacity_dwords_ = 0;
    ring_size_log2_ = 0;
    read_pointer_writeback_address_ = 0;
    last_read_index_ = 0;
    last_write_index_ = 0;
    watched_count_ = 0;
    next_token_ = 0;
    next_range_identity_ = 0;
    ClearRange();
    finished_range_valid_ = false;

    XenosFenceTraceEvent event{};
    event.point = XenosFenceTracePoint::kTraceStarted;
    FillCommon(event, MonotonicNanoseconds(), rex::thread::current_thread_id());
    started = StoreEvent(event);
  }
  {
    std::lock_guard admission_lock(admission_mutex_);
    start_in_progress_ = false;
    admission_state_.store(started ? kAdmissionOpen : 0, std::memory_order_release);
  }
  return started;
}

bool XenosFenceTrace::FinishAndFlush() {
  if (active_callback_trace == this) {
    SaturatingIncrement(reentry_failures_);
    return false;
  }
  std::vector<XenosFenceTraceEvent> events;
  XenosFenceTraceEvent finished_event{};
  {
    std::unique_lock admission_lock(admission_mutex_);
    const uint64_t admission =
        admission_state_.fetch_and(kAdmissionCountMask, std::memory_order_acq_rel);
    if (!(admission & kAdmissionOpen)) {
      return false;
    }
    flush_in_progress_ = true;
    admission_drained_.wait(admission_lock, [this] {
      return (admission_state_.load(std::memory_order_acquire) & kAdmissionCountMask) == 0;
    });
  }
  {
    std::lock_guard state_lock(state_mutex_);
    const uint64_t now = MonotonicNanoseconds();
    ExpireWatches(now, rex::thread::current_thread_id());
    finished_event.point = XenosFenceTracePoint::kTraceFinished;
    finished_event.stored_count = std::min<uint32_t>(next_slot_.load(std::memory_order_relaxed),
                                                     uint32_t(kXenosFenceTraceCapacity));
    finished_event.overflow_count = overflow_.load(std::memory_order_relaxed);
    finished_event.lock_wait_count = lock_waits_.load(std::memory_order_relaxed);
    finished_event.maximum_lock_wait_nanoseconds =
        maximum_lock_wait_nanoseconds_.load(std::memory_order_relaxed);
    finished_event.dropped_callback_count = dropped_callbacks_.load(std::memory_order_relaxed);
    finished_event.reentry_failure_count = reentry_failures_.load(std::memory_order_relaxed);
    finished_event.watched_count = watched_count_;
    finished_event.in_flight_count = CountInFlight();
    finished_event.unresolved_count = CountUnresolved();
    FillCommon(finished_event, now, rex::thread::current_thread_id());
    finished_event.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;

    events.reserve(kXenosFenceTraceCapacity + 1);
    for (size_t i = 0; i < kXenosFenceTraceCapacity; ++i) {
      if (slots_[i].committed.load(std::memory_order_acquire)) {
        events.push_back(slots_[i].event);
      }
    }
    events.push_back(finished_event);
  }
  std::sort(events.begin(), events.end(),
            [](const XenosFenceTraceEvent& left, const XenosFenceTraceEvent& right) {
              return left.sequence < right.sequence;
            });
  const bool serialized = Serialize(events.data(), events.size());
  {
    std::lock_guard admission_lock(admission_mutex_);
    flush_in_progress_ = false;
    if (!serialized) {
      next_sequence_.store(finished_event.sequence - 1, std::memory_order_relaxed);
      admission_state_.store(kAdmissionOpen, std::memory_order_release);
    }
  }
  return serialized;
}

bool XenosFenceTrace::enabled() const noexcept {
  return (admission_state_.load(std::memory_order_acquire) & kAdmissionOpen) != 0;
}

bool XenosFenceTrace::IsTerminal(WatchStage stage) noexcept {
  return stage == WatchStage::kNotDecoded || stage == WatchStage::kReset ||
         stage == WatchStage::kTimedOut;
}

void XenosFenceTrace::FillCommon(XenosFenceTraceEvent& event, uint64_t monotonic_nanoseconds,
                                 uint32_t thread_id) noexcept {
  event.monotonic_nanoseconds = monotonic_nanoseconds;
  event.thread_id = thread_id;
  event.epoch = epoch_;
}

void XenosFenceTrace::FillRing(XenosFenceTraceEvent& event) const noexcept {
  event.ring_generation = ring_generation_;
  event.ring_guest_virtual_base = ring_guest_virtual_base_;
  event.ring_physical_base = ring_physical_base_;
  event.ring_capacity_bytes = ring_capacity_bytes_;
  event.ring_capacity_dwords = ring_capacity_dwords_;
  event.ring_size_log2 = ring_size_log2_;
  event.ring_wrap_mask = ring_capacity_dwords_ ? ring_capacity_dwords_ - 1 : 0;
  event.read_pointer_writeback_address = read_pointer_writeback_address_;
  if (ring_guest_virtual_base_) {
    event.flags |= kXenosFenceTraceGuestVirtualBaseValid;
  }
}

bool XenosFenceTrace::StoreEvent(XenosFenceTraceEvent event) noexcept {
  uint32_t slot = next_slot_.load(std::memory_order_relaxed);
  if (slot >= kXenosFenceTraceCapacity) {
    SaturatingIncrement(overflow_);
    SaturatingIncrement(dropped_callbacks_);
    return false;
  }
  event.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  slots_[slot].event = event;
  slots_[slot].committed.store(true, std::memory_order_release);
  next_slot_.store(slot + 1, std::memory_order_relaxed);
  return true;
}

void XenosFenceTrace::ResetObservationEpoch() noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return;
  }
  const uint64_t now = callback.monotonic_nanoseconds();
  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kTraceReset;
  event.unresolved_count = CountUnresolved();
  FillRing(event);
  FillCommon(event, now, callback.thread_id());
  StoreEvent(event);
  for (Watch& watch : watches_) {
    if (watch.stage != WatchStage::kUnused && !IsTerminal(watch.stage) &&
        !(watch.completion_observed && watch.readback_observed)) {
      watch.stage = WatchStage::kReset;
    }
  }
  ++epoch_;
  ring_guest_virtual_base_ = 0;
  ring_physical_base_ = 0;
  ring_capacity_bytes_ = 0;
  ring_capacity_dwords_ = 0;
  ring_size_log2_ = 0;
  read_pointer_writeback_address_ = 0;
  last_read_index_ = 0;
  last_write_index_ = 0;
  ClearRange();
  finished_range_valid_ = false;
}

void XenosFenceTrace::RingInitialized(uint32_t physical_base, uint32_t size_log2,
                                      uint32_t initial_read_index,
                                      uint32_t initial_write_index) noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return;
  }
  const uint64_t now = callback.monotonic_nanoseconds();
  if (ring_capacity_bytes_) {
    XenosFenceTraceEvent reset{};
    reset.point = XenosFenceTracePoint::kTraceReset;
    reset.unresolved_count = CountUnresolved();
    FillRing(reset);
    FillCommon(reset, now, callback.thread_id());
    StoreEvent(reset);
    for (Watch& watch : watches_) {
      if (watch.stage != WatchStage::kUnused && !IsTerminal(watch.stage) &&
          !(watch.completion_observed && watch.readback_observed)) {
        watch.stage = WatchStage::kReset;
      }
    }
    ++epoch_;
  }
  ++ring_generation_;
  ring_guest_virtual_base_ = 0;
  ring_physical_base_ = physical_base;
  ring_size_log2_ = size_log2;
  ring_capacity_bytes_ = size_log2 <= 28 ? uint32_t(1) << (size_log2 + 3) : 0;
  ring_capacity_dwords_ = ring_capacity_bytes_ / sizeof(uint32_t);
  last_read_index_ = initial_read_index;
  last_write_index_ = initial_write_index;
  ClearRange();
  finished_range_valid_ = false;

  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kRingInitialized;
  event.read_index = initial_read_index;
  event.write_index = initial_write_index;
  FillRing(event);
  FillCommon(event, now, callback.thread_id());
  StoreEvent(event);
}

void XenosFenceTrace::ReadPointerWritebackConfigured(uint32_t address) noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return;
  }
  read_pointer_writeback_address_ = address;
  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kReadPointerWritebackConfigured;
  FillRing(event);
  FillCommon(event, callback.monotonic_nanoseconds(), callback.thread_id());
  StoreEvent(event);
}

uint64_t XenosFenceTrace::WatchSwapReservation(uint32_t guest_virtual_address,
                                               uint32_t physical_address) noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return 0;
  }
  const uint64_t now = callback.monotonic_nanoseconds();
  ExpireWatches(now, callback.thread_id());
  if (!ring_capacity_dwords_ || watched_count_ >= kXenosFenceTraceWatchLimit ||
      (physical_address & 3) || physical_address < ring_physical_base_) {
    return 0;
  }
  uint32_t byte_offset = physical_address - ring_physical_base_;
  if (byte_offset >= ring_capacity_bytes_ || (byte_offset & 3)) {
    return 0;
  }
  Watch& watch = watches_[watched_count_];
  watch = {};
  watch.stage = WatchStage::kRegistered;
  watch.epoch = epoch_;
  watch.ring_generation = ring_generation_;
  watch.guest_reservation_address = guest_virtual_address;
  watch.physical_reservation_address = physical_address;
  watch.reservation_index = byte_offset / sizeof(uint32_t);
  watch.token = (uint64_t(epoch_) << 32) | ++next_token_;
  watch.registered_nanoseconds = now;
  ++watched_count_;

  if (!ring_guest_virtual_base_ && guest_virtual_address >= byte_offset) {
    ring_guest_virtual_base_ = guest_virtual_address - byte_offset;
    XenosFenceTraceEvent mapping{};
    mapping.point = XenosFenceTracePoint::kRingMappingObserved;
    mapping.correlation_token = watch.token;
    mapping.guest_reservation_address = guest_virtual_address;
    mapping.physical_reservation_address = physical_address;
    mapping.reservation_index = watch.reservation_index;
    FillRing(mapping);
    FillCommon(mapping, now, callback.thread_id());
    StoreEvent(mapping);
  }

  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kWatchRegistered;
  event.correlation_token = watch.token;
  event.guest_reservation_address = guest_virtual_address;
  event.physical_reservation_address = physical_address;
  event.reservation_index = watch.reservation_index;
  event.watched_count = watched_count_;
  FillRing(event);
  FillCommon(event, now, callback.thread_id());
  StoreEvent(event);
  return watch.token;
}

void XenosFenceTrace::WritePointerPublished(uint32_t previous_value, uint32_t value) noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return;
  }
  ExpireWatches(callback.monotonic_nanoseconds(), callback.thread_id());
  last_write_index_ = value;
  if (!watched_count_ || !CountInFlight()) {
    return;
  }
  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kWritePointerPublished;
  event.read_index = last_read_index_;
  event.previous_write_index = previous_value;
  event.write_index = value;
  FillRing(event);
  FillCommon(event, callback.monotonic_nanoseconds(), callback.thread_id());
  StoreEvent(event);
}

uint64_t XenosFenceTrace::ConsumerRangeBegin(uint32_t start_index, uint32_t target_index) noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return 0;
  }
  const uint64_t now = callback.monotonic_nanoseconds();
  ExpireWatches(now, callback.thread_id());
  if (!CountInFlight()) {
    ClearRange();
    return 0;
  }
  range_active_ = true;
  range_start_index_ = start_index;
  range_target_index_ = target_index;
  range_actual_end_index_ = start_index;
  range_identity_ = ++next_range_identity_;
  range_correlation_mask_ = TokensInRange(start_index, target_index);
  range_contains_unrelated_pm4_ = false;
  range_failed_ = false;

  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kConsumerRangeBegin;
  event.range_identity = range_identity_;
  event.range_start_index = start_index;
  event.range_target_index = target_index;
  event.correlation_mask = range_correlation_mask_;
  if (range_correlation_mask_) {
    event.flags |= kXenosFenceTraceContainsWatchedSwap;
  }
  FillRing(event);
  FillCommon(event, now, callback.thread_id());
  StoreEvent(event);
  return range_identity_;
}

void XenosFenceTrace::PrimaryPacketObserved(uint32_t start_index, uint32_t end_index,
                                            bool succeeded) noexcept {
  CallbackScope callback(*this);
  if (!callback || !range_active_) {
    return;
  }
  range_actual_end_index_ = end_index;
  if (!PacketCoveredByWatch(start_index, end_index)) {
    range_contains_unrelated_pm4_ = true;
  }
  if (!succeeded) {
    range_failed_ = true;
  }
}

uint64_t XenosFenceTrace::SwapDecoded(uint32_t packet_start_index, uint32_t packet_end_index,
                                      uint32_t fetch_source, uint32_t texture_format,
                                      uint32_t width, uint32_t height) noexcept {
  CallbackScope callback(*this);
  if (!callback || !range_active_) {
    return 0;
  }
  Watch* watch = FindWatchByPacket(packet_start_index);
  if (!watch || watch->stage != WatchStage::kRegistered) {
    return 0;
  }
  watch->stage = WatchStage::kDecoded;
  watch->packet_start_index = packet_start_index;
  watch->packet_end_index = packet_end_index;
  watch->range_identity = range_identity_;

  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kSwapDecoded;
  event.correlation_token = watch->token;
  event.range_identity = range_identity_;
  event.packet_start_index = packet_start_index;
  event.packet_end_index = packet_end_index;
  event.reservation_index = watch->reservation_index;
  event.physical_reservation_address = watch->physical_reservation_address;
  event.fetch_source = fetch_source;
  event.texture_format = texture_format;
  event.width = width;
  event.height = height;
  FillRing(event);
  FillCommon(event, callback.monotonic_nanoseconds(), callback.thread_id());
  StoreEvent(event);
  return watch->token;
}

void XenosFenceTrace::ConsumerRangeEnd(uint32_t actual_end_index, bool succeeded) noexcept {
  CallbackScope callback(*this);
  if (!callback || !range_active_) {
    return;
  }
  range_actual_end_index_ = actual_end_index;
  range_failed_ = range_failed_ || !succeeded;
  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kConsumerRangeEnd;
  event.range_identity = range_identity_;
  event.range_start_index = range_start_index_;
  event.range_end_index = actual_end_index;
  event.range_target_index = range_target_index_;
  event.correlation_mask = range_correlation_mask_;
  if (range_correlation_mask_) {
    event.flags |= kXenosFenceTraceContainsWatchedSwap;
  }
  if (range_contains_unrelated_pm4_) {
    event.flags |= kXenosFenceTraceContainsUnrelatedPm4;
  }
  if (range_failed_) {
    event.flags |= kXenosFenceTraceRangeFailed;
  }
  FillRing(event);
  FillCommon(event, callback.monotonic_nanoseconds(), callback.thread_id());
  StoreEvent(event);

  for (size_t i = 0; i < watches_.size(); ++i) {
    if (!(range_correlation_mask_ & (UINT64_C(1) << i))) {
      continue;
    }
    Watch& watch = watches_[i];
    if (watch.stage != WatchStage::kRegistered) {
      continue;
    }
    watch.stage = WatchStage::kNotDecoded;
    XenosFenceTraceEvent missed{};
    missed.point = XenosFenceTracePoint::kWatchNotDecoded;
    missed.flags = kXenosFenceTraceRangeFailed;
    missed.correlation_token = watch.token;
    missed.range_identity = range_identity_;
    missed.reservation_index = watch.reservation_index;
    missed.guest_reservation_address = watch.guest_reservation_address;
    missed.physical_reservation_address = watch.physical_reservation_address;
    FillRing(missed);
    FillCommon(missed, callback.monotonic_nanoseconds(), callback.thread_id());
    StoreEvent(missed);
  }

  finished_range_valid_ = true;
  finished_range_start_index_ = range_start_index_;
  finished_range_end_index_ = actual_end_index;
  finished_range_target_index_ = range_target_index_;
  finished_range_identity_ = range_identity_;
  finished_range_correlation_mask_ = range_correlation_mask_;
  last_read_index_ = range_target_index_;
  ClearRange();
}

XenosFenceTrace::ReadPointerWritebackAdmission
XenosFenceTrace::AdmitReadPointerWriteback() noexcept {
  return ReadPointerWritebackAdmission(*this);
}

void XenosFenceTrace::ReadPointerWriteback(ReadPointerWritebackAdmission&& admission,
                                           uint32_t address, uint32_t old_value,
                                           uint32_t new_value) noexcept {
  CallbackScope callback(*this, admission);
  if (callback) {
    RecordReadPointerWriteback(callback, address, old_value, new_value);
  }
}

void XenosFenceTrace::RecordReadPointerWriteback(const CallbackScope& callback, uint32_t address,
                                                 uint32_t old_value, uint32_t new_value) noexcept {
  if (!finished_range_valid_ || address != read_pointer_writeback_address_ ||
      new_value != finished_range_target_index_) {
    return;
  }
  uint32_t flags = 0;
  for (size_t i = 0; i < watches_.size(); ++i) {
    if (!(finished_range_correlation_mask_ & (UINT64_C(1) << i))) {
      continue;
    }
    Watch& watch = watches_[i];
    watch.readback_observed = true;
    if (watch.completion_observed) {
      flags |= kXenosFenceTraceCompletionBeforeReadback;
    } else {
      flags |= kXenosFenceTraceReadbackBeforeCompletion;
    }
  }

  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kReadPointerWriteback;
  event.flags = flags;
  event.range_identity = finished_range_identity_;
  event.range_start_index = finished_range_start_index_;
  event.range_end_index = finished_range_end_index_;
  event.range_target_index = finished_range_target_index_;
  event.correlation_mask = finished_range_correlation_mask_;
  event.read_pointer_writeback_address = address;
  event.read_pointer_writeback_old = old_value;
  event.read_pointer_writeback_new = new_value;
  FillRing(event);
  FillCommon(event, callback.monotonic_nanoseconds(), callback.thread_id());
  StoreEvent(event);
  finished_range_valid_ = false;
}

void XenosFenceTrace::D3D12SwapRecording(uint64_t correlation_token, uint64_t submission_identity,
                                         bool submission_had_prior_guest_work) noexcept {
  if (!enabled() || !correlation_token) {
    return;
  }
  CallbackScope callback(*this);
  if (!callback) {
    return;
  }
  Watch* watch = FindWatchByToken(correlation_token);
  if (!watch || watch->stage != WatchStage::kDecoded) {
    return;
  }
  watch->stage = WatchStage::kRecording;
  watch->submission_identity = submission_identity;
  watch->submission_had_prior_guest_work = submission_had_prior_guest_work;
}

void XenosFenceTrace::D3D12Submission(uint64_t submission_identity, uint64_t command_list_identity,
                                      uint64_t fence_identity, uint64_t fence_value) noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return;
  }
  uint64_t mask = 0;
  bool shared = false;
  uint32_t token_count = 0;
  for (size_t i = 0; i < watches_.size(); ++i) {
    Watch& watch = watches_[i];
    if (watch.stage != WatchStage::kRecording || watch.submission_identity != submission_identity) {
      continue;
    }
    watch.stage = WatchStage::kSubmitted;
    watch.fence_identity = fence_identity;
    watch.fence_value = fence_value;
    mask |= UINT64_C(1) << i;
    shared = shared || watch.submission_had_prior_guest_work;
    ++token_count;
  }
  if (!mask) {
    return;
  }
  shared = shared || token_count > 1;
  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kD3D12Submission;
  event.correlation_mask = mask;
  event.command_list_identity = command_list_identity;
  event.fence_identity = fence_identity;
  event.fence_value = fence_value;
  if (shared) {
    event.flags |= kXenosFenceTraceSharedSubmission;
  }
  FillRing(event);
  FillCommon(event, callback.monotonic_nanoseconds(), callback.thread_id());
  StoreEvent(event);
}

void XenosFenceTrace::D3D12FenceCompleted(uint64_t fence_identity, uint64_t completed_value,
                                          bool succeeded) noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return;
  }
  uint64_t mask = 0;
  uint32_t flags = succeeded ? kXenosFenceTraceCompletionSucceeded : 0;
  for (size_t i = 0; i < watches_.size(); ++i) {
    Watch& watch = watches_[i];
    if (watch.stage != WatchStage::kSubmitted || watch.fence_identity != fence_identity ||
        !succeeded || completed_value < watch.fence_value) {
      continue;
    }
    watch.stage = WatchStage::kCompleted;
    watch.completion_observed = true;
    if (watch.readback_observed) {
      flags |= kXenosFenceTraceReadbackBeforeCompletion;
    } else {
      flags |= kXenosFenceTraceCompletionBeforeReadback;
    }
    mask |= UINT64_C(1) << i;
  }
  if (!mask && succeeded) {
    return;
  }
  XenosFenceTraceEvent event{};
  event.point = XenosFenceTracePoint::kD3D12FenceCompleted;
  event.flags = flags;
  event.correlation_mask = mask;
  event.fence_identity = fence_identity;
  event.fence_completed_value = completed_value;
  FillRing(event);
  FillCommon(event, callback.monotonic_nanoseconds(), callback.thread_id());
  StoreEvent(event);
}

bool XenosFenceTrace::HasSubmittedWatches() noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return false;
  }
  for (const Watch& watch : watches_) {
    if (watch.stage == WatchStage::kSubmitted) {
      return true;
    }
  }
  return false;
}

void XenosFenceTrace::PollTimeouts() noexcept {
  if (!enabled()) {
    return;
  }
  PollTimeoutsAt(MonotonicNanoseconds());
}

void XenosFenceTrace::PollTimeoutsAt(uint64_t monotonic_nanoseconds) noexcept {
  CallbackScope callback(*this);
  if (!callback) {
    return;
  }
  ExpireWatches(monotonic_nanoseconds, callback.thread_id());
}

void XenosFenceTrace::ExpireWatches(uint64_t now, uint32_t thread_id) noexcept {
  for (Watch& watch : watches_) {
    if (watch.stage == WatchStage::kUnused || IsTerminal(watch.stage) ||
        (watch.completion_observed && watch.readback_observed) ||
        now < watch.registered_nanoseconds ||
        now - watch.registered_nanoseconds < kXenosFenceTraceTimeoutNanoseconds) {
      continue;
    }
    watch.stage = WatchStage::kTimedOut;
    XenosFenceTraceEvent event{};
    event.point = XenosFenceTracePoint::kWatchTimedOut;
    event.correlation_token = watch.token;
    event.range_identity = watch.range_identity;
    event.fence_identity = watch.fence_identity;
    event.fence_value = watch.fence_value;
    FillRing(event);
    FillCommon(event, now, thread_id);
    StoreEvent(event);
  }
}

uint32_t XenosFenceTrace::CountInFlight() const noexcept {
  uint32_t count = 0;
  for (const Watch& watch : watches_) {
    if (watch.stage != WatchStage::kUnused && !IsTerminal(watch.stage) &&
        !(watch.completion_observed && watch.readback_observed)) {
      ++count;
    }
  }
  return count;
}

uint32_t XenosFenceTrace::CountUnresolved() const noexcept {
  uint32_t count = 0;
  for (const Watch& watch : watches_) {
    if (watch.stage != WatchStage::kUnused &&
        !(watch.completion_observed && watch.readback_observed)) {
      ++count;
    }
  }
  return count;
}

uint64_t XenosFenceTrace::TokensInRange(uint32_t start, uint32_t end) const noexcept {
  uint64_t mask = 0;
  for (size_t i = 0; i < watches_.size(); ++i) {
    const Watch& watch = watches_[i];
    if (watch.stage != WatchStage::kRegistered || watch.ring_generation != ring_generation_) {
      continue;
    }
    uint32_t packet_index = (watch.reservation_index + 8) % ring_capacity_dwords_;
    if (RangeContainsIndex(start, end, packet_index, ring_capacity_dwords_)) {
      mask |= UINT64_C(1) << i;
    }
  }
  return mask;
}

XenosFenceTrace::Watch* XenosFenceTrace::FindWatchByToken(uint64_t token) noexcept {
  for (Watch& watch : watches_) {
    if (watch.token == token && watch.epoch == epoch_ &&
        watch.ring_generation == ring_generation_) {
      return &watch;
    }
  }
  return nullptr;
}

XenosFenceTrace::Watch* XenosFenceTrace::FindWatchByPacket(uint32_t packet_start_index) noexcept {
  for (Watch& watch : watches_) {
    if (watch.stage != WatchStage::kRegistered || watch.epoch != epoch_ ||
        watch.ring_generation != ring_generation_) {
      continue;
    }
    if ((watch.reservation_index + 8) % ring_capacity_dwords_ == packet_start_index) {
      return &watch;
    }
  }
  return nullptr;
}

bool XenosFenceTrace::PacketCoveredByWatch(uint32_t start, uint32_t end) const noexcept {
  if (!ring_capacity_dwords_ || start == end) {
    return false;
  }
  uint32_t last = (end + ring_capacity_dwords_ - 1) % ring_capacity_dwords_;
  for (size_t i = 0; i < watches_.size(); ++i) {
    if (!(range_correlation_mask_ & (UINT64_C(1) << i))) {
      continue;
    }
    const Watch& watch = watches_[i];
    uint32_t watched_start = (watch.reservation_index + 1) % ring_capacity_dwords_;
    uint32_t watched_end = (watched_start + 64) % ring_capacity_dwords_;
    if (RangeContainsIndex(watched_start, watched_end, start, ring_capacity_dwords_) &&
        RangeContainsIndex(watched_start, watched_end, last, ring_capacity_dwords_)) {
      return true;
    }
  }
  return false;
}

void XenosFenceTrace::ClearRange() noexcept {
  range_active_ = false;
  range_start_index_ = 0;
  range_target_index_ = 0;
  range_actual_end_index_ = 0;
  range_identity_ = 0;
  range_correlation_mask_ = 0;
  range_contains_unrelated_pm4_ = false;
  range_failed_ = false;
}

XenosFenceTraceStatistics XenosFenceTrace::statistics() const noexcept {
  const auto read_statistics = [this] {
    XenosFenceTraceStatistics result{};
    result.stored = std::min<uint32_t>(next_slot_.load(std::memory_order_relaxed),
                                       uint32_t(kXenosFenceTraceCapacity));
    result.overflow = overflow_.load(std::memory_order_relaxed);
    result.lock_waits = lock_waits_.load(std::memory_order_relaxed);
    result.maximum_lock_wait_nanoseconds =
        maximum_lock_wait_nanoseconds_.load(std::memory_order_relaxed);
    result.dropped_callbacks = dropped_callbacks_.load(std::memory_order_relaxed);
    result.reentry_failures = reentry_failures_.load(std::memory_order_relaxed);
    result.last_sequence = next_sequence_.load(std::memory_order_relaxed);
    result.watched = watched_count_;
    result.in_flight = CountInFlight();
    result.unresolved = CountUnresolved();
    result.epoch = epoch_;
    return result;
  };
  if (active_callback_trace == this) {
    return read_statistics();
  }
  std::lock_guard state_lock(state_mutex_);
  return read_statistics();
}

bool XenosFenceTrace::CopyEvent(size_t index, XenosFenceTraceEvent& event) const noexcept {
  const auto copy_event = [this, index, &event] {
    if (index >= kXenosFenceTraceCapacity ||
        !slots_[index].committed.load(std::memory_order_acquire)) {
      return false;
    }
    event = slots_[index].event;
    return true;
  };
  if (active_callback_trace == this) {
    return copy_event();
  }
  std::lock_guard state_lock(state_mutex_);
  return copy_event();
}

bool XenosFenceTrace::Serialize(const XenosFenceTraceEvent* events, size_t event_count) const {
  std::filesystem::path temporary_path = output_path_;
  temporary_path += ".tmp";
  std::error_code error;
  const auto temporary_status = std::filesystem::symlink_status(temporary_path, error);
  if ((error && error != std::errc::no_such_file_or_directory) ||
      (!error && temporary_status.type() != std::filesystem::file_type::not_found)) {
    return false;
  }
  std::ofstream output(temporary_path, std::ios::out | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << "# schema=xenos_consumer_fence_trace_v2\n";
  output << "# capacity=" << kXenosFenceTraceCapacity << '\n';
  output << "# watch_limit=" << kXenosFenceTraceWatchLimit << '\n';
  output << "# timeout_nanoseconds=" << kXenosFenceTraceTimeoutNanoseconds << '\n';
  output << "sequence,monotonic_ns,thread_id,epoch,event,flags,ring_generation,"
            "ring_guest_virtual_base,ring_physical_base,ring_capacity_bytes,"
            "ring_capacity_dwords,ring_size_log2,ring_wrap_mask,read_index,write_index,"
            "previous_write_index,range_start_index,range_end_index,range_target_index,"
            "packet_start_index,packet_end_index,reservation_index,guest_reservation_address,"
            "physical_reservation_address,fetch_source,texture_format,width,height,"
            "read_pointer_writeback_address,read_pointer_writeback_old,"
            "read_pointer_writeback_new,correlation_token,correlation_mask,range_identity,"
            "command_list_identity,fence_identity,fence_value,fence_completed_value,"
            "stored_count,overflow_count,lock_wait_count,maximum_lock_wait_nanoseconds,"
            "dropped_callback_count,reentry_failure_count,watched_count,in_flight_count,"
            "unresolved_count\n";
  output << std::hex;
  for (size_t i = 0; i < event_count; ++i) {
    const XenosFenceTraceEvent& event = events[i];
    output << event.sequence << ',' << event.monotonic_nanoseconds << ',' << event.thread_id << ','
           << event.epoch << ',' << PointName(event.point) << ',' << event.flags << ','
           << event.ring_generation << ',' << event.ring_guest_virtual_base << ','
           << event.ring_physical_base << ',' << event.ring_capacity_bytes << ','
           << event.ring_capacity_dwords << ',' << event.ring_size_log2 << ','
           << event.ring_wrap_mask << ',' << event.read_index << ',' << event.write_index << ','
           << event.previous_write_index << ',' << event.range_start_index << ','
           << event.range_end_index << ',' << event.range_target_index << ','
           << event.packet_start_index << ',' << event.packet_end_index << ','
           << event.reservation_index << ',' << event.guest_reservation_address << ','
           << event.physical_reservation_address << ',' << event.fetch_source << ','
           << event.texture_format << ',' << event.width << ',' << event.height << ','
           << event.read_pointer_writeback_address << ',' << event.read_pointer_writeback_old << ','
           << event.read_pointer_writeback_new << ',' << event.correlation_token << ','
           << event.correlation_mask << ',' << event.range_identity << ','
           << event.command_list_identity << ',' << event.fence_identity << ',' << event.fence_value
           << ',' << event.fence_completed_value << ',' << event.stored_count << ','
           << event.overflow_count << ',' << event.lock_wait_count << ','
           << event.maximum_lock_wait_nanoseconds << ',' << event.dropped_callback_count << ','
           << event.reentry_failure_count << ',' << event.watched_count << ','
           << event.in_flight_count << ',' << event.unresolved_count << '\n';
  }
  output.close();
  if (!output) {
    std::filesystem::remove(temporary_path, error);
    return false;
  }
  if (std::filesystem::exists(output_path_, error) || error) {
    std::filesystem::remove(temporary_path, error);
    return false;
  }
  std::filesystem::rename(temporary_path, output_path_, error);
  if (error) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
  }
  return !error;
}

XenosFenceTrace& GetXenosFenceTrace() noexcept {
  static XenosFenceTrace trace;
  return trace;
}

}  // namespace rex::graphics::diagnostic
