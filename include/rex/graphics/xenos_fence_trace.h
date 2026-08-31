#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace rex::graphics::diagnostic {

constexpr size_t kXenosFenceTraceCapacity = 512;
constexpr size_t kXenosFenceTraceWatchLimit = 8;
constexpr uint64_t kXenosFenceTraceTimeoutNanoseconds = UINT64_C(30000000000);

enum class XenosFenceTracePoint : uint8_t {
  kTraceStarted,
  kTraceReset,
  kRingInitialized,
  kRingMappingObserved,
  kReadPointerWritebackConfigured,
  kWatchRegistered,
  kWritePointerPublished,
  kConsumerRangeBegin,
  kSwapDecoded,
  kD3D12Submission,
  kD3D12FenceCompleted,
  kConsumerRangeEnd,
  kReadPointerWriteback,
  kWatchNotDecoded,
  kWatchTimedOut,
  kTraceFinished,
};

enum XenosFenceTraceFlags : uint32_t {
  kXenosFenceTraceGuestVirtualBaseValid = 1u << 0,
  kXenosFenceTraceContainsWatchedSwap = 1u << 1,
  kXenosFenceTraceContainsUnrelatedPm4 = 1u << 2,
  kXenosFenceTraceRangeFailed = 1u << 3,
  kXenosFenceTraceSharedSubmission = 1u << 4,
  kXenosFenceTraceCompletionSucceeded = 1u << 5,
  kXenosFenceTraceReadbackBeforeCompletion = 1u << 6,
  kXenosFenceTraceCompletionBeforeReadback = 1u << 7,
};

struct XenosFenceTraceEvent {
  uint64_t sequence = 0;
  uint64_t monotonic_nanoseconds = 0;
  uint32_t thread_id = 0;
  uint32_t epoch = 0;
  XenosFenceTracePoint point = XenosFenceTracePoint::kTraceStarted;
  uint32_t flags = 0;

  uint32_t ring_generation = 0;
  uint32_t ring_guest_virtual_base = 0;
  uint32_t ring_physical_base = 0;
  uint32_t ring_capacity_bytes = 0;
  uint32_t ring_capacity_dwords = 0;
  uint32_t ring_size_log2 = 0;
  uint32_t ring_wrap_mask = 0;

  uint32_t read_index = 0;
  uint32_t write_index = 0;
  uint32_t previous_write_index = 0;
  uint32_t range_start_index = 0;
  uint32_t range_end_index = 0;
  uint32_t range_target_index = 0;
  uint32_t packet_start_index = 0;
  uint32_t packet_end_index = 0;
  uint32_t reservation_index = 0;

  uint32_t guest_reservation_address = 0;
  uint32_t physical_reservation_address = 0;
  uint32_t fetch_source = 0;
  uint32_t texture_format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t read_pointer_writeback_address = 0;
  uint32_t read_pointer_writeback_old = 0;
  uint32_t read_pointer_writeback_new = 0;

  uint64_t correlation_token = 0;
  uint64_t correlation_mask = 0;
  uint64_t range_identity = 0;
  uint64_t command_list_identity = 0;
  uint64_t fence_identity = 0;
  uint64_t fence_value = 0;
  uint64_t fence_completed_value = 0;

  uint32_t stored_count = 0;
  uint32_t overflow_count = 0;
  uint32_t contention_drop_count = 0;
  uint32_t watched_count = 0;
  uint32_t in_flight_count = 0;
  uint32_t unresolved_count = 0;
};

struct XenosFenceTraceStatistics {
  uint32_t stored = 0;
  uint32_t overflow = 0;
  uint32_t contention_drops = 0;
  uint32_t watched = 0;
  uint32_t in_flight = 0;
  uint32_t unresolved = 0;
  uint32_t epoch = 0;
  uint64_t last_sequence = 0;
};

class XenosFenceTrace final {
 public:
  XenosFenceTrace() = default;

  bool Start(const std::filesystem::path& output_path);
  bool FinishAndFlush();
  bool enabled() const noexcept;

  void ResetObservationEpoch() noexcept;
  void RingInitialized(uint32_t physical_base, uint32_t size_log2, uint32_t initial_read_index,
                       uint32_t initial_write_index) noexcept;
  void ReadPointerWritebackConfigured(uint32_t address) noexcept;
  uint64_t WatchSwapReservation(uint32_t guest_virtual_address, uint32_t physical_address) noexcept;
  void WritePointerPublished(uint32_t previous_value, uint32_t value) noexcept;

  uint64_t ConsumerRangeBegin(uint32_t start_index, uint32_t target_index) noexcept;
  void PrimaryPacketObserved(uint32_t start_index, uint32_t end_index, bool succeeded) noexcept;
  uint64_t SwapDecoded(uint32_t packet_start_index, uint32_t packet_end_index,
                       uint32_t fetch_source, uint32_t texture_format, uint32_t width,
                       uint32_t height) noexcept;
  void ConsumerRangeEnd(uint32_t actual_end_index, bool succeeded) noexcept;
  void ReadPointerWriteback(uint32_t address, uint32_t old_value, uint32_t new_value) noexcept;

  void D3D12SwapRecording(uint64_t correlation_token, uint64_t submission_identity,
                          bool submission_had_prior_guest_work) noexcept;
  void D3D12Submission(uint64_t submission_identity, uint64_t command_list_identity,
                       uint64_t fence_identity, uint64_t fence_value) noexcept;
  void D3D12FenceCompleted(uint64_t fence_identity, uint64_t completed_value,
                           bool succeeded) noexcept;
  bool HasSubmittedWatches() const noexcept;

  void PollTimeouts() noexcept;
  void PollTimeoutsAt(uint64_t monotonic_nanoseconds) noexcept;

  XenosFenceTraceStatistics statistics() const noexcept;
  bool CopyEvent(size_t index, XenosFenceTraceEvent& event) const noexcept;

  static uint64_t MonotonicNanoseconds() noexcept;
  static bool RangeContainsIndex(uint32_t start, uint32_t end, uint32_t index,
                                 uint32_t capacity_dwords) noexcept;

 private:
  enum class WatchStage : uint8_t {
    kUnused,
    kRegistered,
    kDecoded,
    kRecording,
    kSubmitted,
    kCompleted,
    kNotDecoded,
    kReset,
    kTimedOut,
  };

  struct Watch {
    WatchStage stage = WatchStage::kUnused;
    uint32_t epoch = 0;
    uint32_t ring_generation = 0;
    uint32_t guest_reservation_address = 0;
    uint32_t physical_reservation_address = 0;
    uint32_t reservation_index = 0;
    uint32_t packet_start_index = 0;
    uint32_t packet_end_index = 0;
    uint64_t token = 0;
    uint64_t registered_nanoseconds = 0;
    uint64_t range_identity = 0;
    uint64_t submission_identity = 0;
    uint64_t fence_identity = 0;
    uint64_t fence_value = 0;
    bool readback_observed = false;
    bool completion_observed = false;
    bool submission_had_prior_guest_work = false;
  };

  struct Slot {
    std::atomic<bool> committed{false};
    XenosFenceTraceEvent event{};
  };

  bool TryLock(std::unique_lock<std::mutex>& lock) noexcept;
  static bool IsTerminal(WatchStage stage) noexcept;
  bool StoreEvent(XenosFenceTraceEvent event) noexcept;
  void FillCommon(XenosFenceTraceEvent& event, uint64_t now) noexcept;
  void FillRing(XenosFenceTraceEvent& event) const noexcept;
  void ExpireWatches(uint64_t now) noexcept;
  uint32_t CountInFlight() const noexcept;
  uint32_t CountUnresolved() const noexcept;
  uint64_t TokensInRange(uint32_t start, uint32_t end) const noexcept;
  Watch* FindWatchByToken(uint64_t token) noexcept;
  Watch* FindWatchByPacket(uint32_t packet_start_index) noexcept;
  bool PacketCoveredByWatch(uint32_t start, uint32_t end) const noexcept;
  void ClearRange() noexcept;
  bool Serialize(const XenosFenceTraceEvent* events, size_t event_count) const;

  mutable std::mutex state_mutex_;
  std::array<Slot, kXenosFenceTraceCapacity> slots_{};
  std::array<Watch, kXenosFenceTraceWatchLimit> watches_{};
  std::atomic<bool> enabled_{false};
  std::atomic<uint32_t> next_slot_{0};
  std::atomic<uint32_t> overflow_{0};
  std::atomic<uint32_t> contention_drops_{0};
  std::atomic<uint64_t> next_sequence_{0};
  std::filesystem::path output_path_;
  bool flush_in_progress_ = false;

  uint32_t epoch_ = 0;
  uint32_t ring_generation_ = 0;
  uint32_t ring_guest_virtual_base_ = 0;
  uint32_t ring_physical_base_ = 0;
  uint32_t ring_capacity_bytes_ = 0;
  uint32_t ring_capacity_dwords_ = 0;
  uint32_t ring_size_log2_ = 0;
  uint32_t read_pointer_writeback_address_ = 0;
  uint32_t last_read_index_ = 0;
  uint32_t last_write_index_ = 0;
  uint32_t watched_count_ = 0;
  uint64_t next_token_ = 0;
  uint64_t next_range_identity_ = 0;

  bool range_active_ = false;
  uint32_t range_start_index_ = 0;
  uint32_t range_target_index_ = 0;
  uint32_t range_actual_end_index_ = 0;
  uint64_t range_identity_ = 0;
  uint64_t range_correlation_mask_ = 0;
  bool range_contains_unrelated_pm4_ = false;
  bool range_failed_ = false;

  bool finished_range_valid_ = false;
  uint32_t finished_range_start_index_ = 0;
  uint32_t finished_range_end_index_ = 0;
  uint32_t finished_range_target_index_ = 0;
  uint64_t finished_range_identity_ = 0;
  uint64_t finished_range_correlation_mask_ = 0;
};

XenosFenceTrace& GetXenosFenceTrace() noexcept;

}  // namespace rex::graphics::diagnostic
