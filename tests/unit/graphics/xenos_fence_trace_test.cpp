#include <rex/graphics/xenos_fence_trace.h>
#include <rex/thread.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rex::graphics::diagnostic {

struct XenosFenceTraceTestAccess {
  static std::unique_lock<std::mutex> LockState(XenosFenceTrace& trace) {
    return std::unique_lock<std::mutex>(trace.state_mutex_);
  }

  static uint32_t LockWaits(const XenosFenceTrace& trace) {
    return trace.lock_waits_.load(std::memory_order_relaxed);
  }

  static bool InvokeSameThreadReentry(XenosFenceTrace& trace) {
    XenosFenceTrace::CallbackScope callback(trace);
    if (!callback) {
      return false;
    }
    trace.WritePointerPublished(0, 1);
    XenosFenceTraceEvent event{};
    return trace.statistics().reentry_failures == 1 && trace.CopyEvent(0, event);
  }

  static bool InvokeReadbackAdmissionReentry(XenosFenceTrace& trace) {
    XenosFenceTrace::CallbackScope callback(trace);
    if (!callback) {
      return false;
    }
    auto admission = trace.AdmitReadPointerWriteback();
    return trace.statistics().reentry_failures == 1;
  }

  static uint64_t ObserveAdmission(const XenosFenceTrace& trace) {
    return trace.admission_state_.load(std::memory_order_acquire);
  }

  static bool TryStaleAdmission(XenosFenceTrace& trace, uint64_t observed) {
    return trace.admission_state_.compare_exchange_strong(
        observed, observed + 1, std::memory_order_acq_rel, std::memory_order_acquire);
  }
};

namespace {

std::filesystem::path TracePath(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         (std::string("rex-xenos-fence-trace-") + std::string(name) + ".csv");
}

std::vector<XenosFenceTraceEvent> Events(const XenosFenceTrace& trace) {
  std::vector<XenosFenceTraceEvent> events;
  XenosFenceTraceEvent event{};
  for (size_t i = 0; i < kXenosFenceTraceCapacity; ++i) {
    if (trace.CopyEvent(i, event)) {
      events.push_back(event);
    }
  }
  return events;
}

const XenosFenceTraceEvent* FindLast(const std::vector<XenosFenceTraceEvent>& events,
                                     XenosFenceTracePoint point) {
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->point == point) {
      return &*it;
    }
  }
  return nullptr;
}

void Initialize(XenosFenceTrace& trace, const std::filesystem::path& path) {
  std::filesystem::remove(path);
  std::filesystem::path temporary_path = path;
  temporary_path += ".tmp";
  std::filesystem::remove(temporary_path);
  REQUIRE(trace.Start(path));
  trace.RingInitialized(0x100000, 8, 0, 0);
  trace.ReadPointerWritebackConfigured(0x200000);
}

bool WaitForLockWaits(const XenosFenceTrace& trace, uint32_t minimum) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (XenosFenceTraceTestAccess::LockWaits(trace) >= minimum) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

uint64_t Watch(XenosFenceTrace& trace, uint32_t index) {
  return trace.WatchSwapReservation(0x80000000 + index * 4, 0x100000 + index * 4);
}

uint64_t Decode(XenosFenceTrace& trace, uint32_t reservation_index, uint32_t range_end = 200) {
  REQUIRE(trace.ConsumerRangeBegin(0, range_end) != 0);
  trace.PrimaryPacketObserved(reservation_index + 1, reservation_index + 8, true);
  uint64_t token =
      trace.SwapDecoded(reservation_index + 8, reservation_index + 13, 0x300000, 6, 1280, 720);
  REQUIRE(token != 0);
  trace.PrimaryPacketObserved(reservation_index + 8, reservation_index + 13, true);
  return token;
}

void Readback(XenosFenceTrace& trace, uint32_t address, uint32_t old_value, uint32_t new_value) {
  auto admission = trace.AdmitReadPointerWriteback();
  trace.ReadPointerWriteback(std::move(admission), address, old_value, new_value);
}

}  // namespace

TEST_CASE("Xenos fence trace disabled path is inert", "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("disabled");
  std::filesystem::remove(path);

  trace.ResetObservationEpoch();
  trace.RingInitialized(0x100000, 8, 7, 9);
  trace.ReadPointerWritebackConfigured(0x200000);
  CHECK(trace.WatchSwapReservation(0x80000000, 0x100000) == 0);
  trace.WritePointerPublished(9, 10);
  CHECK(trace.ConsumerRangeBegin(7, 10) == 0);
  trace.PrimaryPacketObserved(7, 8, true);
  CHECK(trace.SwapDecoded(7, 12, 0x300000, 6, 1280, 720) == 0);
  trace.ConsumerRangeEnd(10, true);
  Readback(trace, 0x200000, 7, 10);
  trace.D3D12SwapRecording(1, 1, false);
  trace.D3D12Submission(1, 2, 3, 1);
  trace.D3D12FenceCompleted(3, 1, true);
  trace.PollTimeoutsAt(UINT64_MAX);

  const auto stats = trace.statistics();
  CHECK(stats.stored == 0);
  CHECK(stats.lock_waits == 0);
  CHECK(stats.dropped_callbacks == 0);
  CHECK(stats.reentry_failures == 0);
  CHECK(stats.watched == 0);
  CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Xenos fence trace waits through the former sticky startup collision",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("startup-collision");
  std::filesystem::remove(path);
  REQUIRE(trace.Start(path));

  auto state_lock = XenosFenceTraceTestAccess::LockState(trace);
  auto reset = std::async(std::launch::async, [&trace] { trace.ResetObservationEpoch(); });
  REQUIRE(WaitForLockWaits(trace, 1));
  auto initialized =
      std::async(std::launch::async, [&trace] { trace.RingInitialized(0x100000, 8, 0, 0); });
  REQUIRE(WaitForLockWaits(trace, 2));
  auto configured =
      std::async(std::launch::async, [&trace] { trace.ReadPointerWritebackConfigured(0x200000); });
  REQUIRE(WaitForLockWaits(trace, 3));
  state_lock.unlock();
  reset.get();
  initialized.get();
  configured.get();

  trace.RingInitialized(0x100000, 8, 0, 0);
  CHECK(Watch(trace, 16) != 0);
  const auto stats = trace.statistics();
  CHECK(stats.lock_waits >= 3);
  CHECK(stats.maximum_lock_wait_nanoseconds > 0);
  CHECK(stats.dropped_callbacks == 0);
  CHECK(stats.reentry_failures == 0);
  CHECK(stats.watched == 1);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace detects same-thread reentry without blocking",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("same-thread-reentry");
  Initialize(trace, path);

  const uint32_t saved_thread_id = rex::thread::current_thread_id();
  rex::thread::set_current_thread_id(0);
  const bool reentry_completed = XenosFenceTraceTestAccess::InvokeSameThreadReentry(trace);
  rex::thread::set_current_thread_id(saved_thread_id);
  REQUIRE(reentry_completed);
  const auto stats = trace.statistics();
  CHECK(stats.reentry_failures == 1);
  CHECK(stats.dropped_callbacks == 0);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace diagnostic reads serialize with reset",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("read-reset");
  Initialize(trace, path);

  auto state_lock = XenosFenceTraceTestAccess::LockState(trace);
  auto reset = std::async(std::launch::async, [&trace] { trace.ResetObservationEpoch(); });
  REQUIRE(WaitForLockWaits(trace, 1));
  auto copy = std::async(std::launch::async, [&trace] {
    XenosFenceTraceEvent event{};
    return trace.CopyEvent(0, event);
  });
  CHECK(copy.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  state_lock.unlock();

  reset.get();
  CHECK(copy.get());
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace close rejects stale pre-admission observations",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("stale-admission");
  Initialize(trace, path);

  const uint64_t observed = XenosFenceTraceTestAccess::ObserveAdmission(trace);
  REQUIRE(trace.FinishAndFlush());
  CHECK_FALSE(XenosFenceTraceTestAccess::TryStaleAdmission(trace, observed));
  trace.WritePointerPublished(0, 1);
  CHECK_FALSE(trace.enabled());
  const auto stats = trace.statistics();
  CHECK(stats.dropped_callbacks == 0);
  CHECK(stats.reentry_failures == 0);
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace drains an admitted callback before one final serialization",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("drain");
  Initialize(trace, path);

  auto state_lock = XenosFenceTraceTestAccess::LockState(trace);
  auto registration = std::async(std::launch::async, [&trace] { return Watch(trace, 16); });
  REQUIRE(WaitForLockWaits(trace, 1));
  auto finalization = std::async(std::launch::async, [&trace] { return trace.FinishAndFlush(); });
  CHECK(finalization.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  const uint64_t lock_release_time = XenosFenceTrace::MonotonicNanoseconds();
  state_lock.unlock();

  CHECK(registration.get() != 0);
  CHECK(finalization.get());
  CHECK_FALSE(trace.FinishAndFlush());
  const auto stats = trace.statistics();
  CHECK(stats.watched == 1);
  CHECK(stats.dropped_callbacks == 0);
  CHECK(stats.reentry_failures == 0);
  const auto events = Events(trace);
  const auto* registration_event = FindLast(events, XenosFenceTracePoint::kWatchRegistered);
  REQUIRE(registration_event);
  CHECK(registration_event->monotonic_nanoseconds <= lock_release_time);

  {
    std::ifstream input(path);
    REQUIRE(input);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(contents.find(",watch_registered,") < contents.find(",trace_finished,"));
  }
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace drains readback admission across the guest write",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("readback-admission-drain");
  Initialize(trace, path);
  const uint64_t token = Watch(trace, 16);
  REQUIRE(token != 0);
  CHECK(Decode(trace, 16) == token);
  trace.D3D12SwapRecording(token, 1, false);
  trace.D3D12Submission(1, 2, 3, 1);
  trace.ConsumerRangeEnd(200, true);
  trace.D3D12FenceCompleted(3, 1, true);

  auto state_lock = XenosFenceTraceTestAccess::LockState(trace);
  auto readback_admission = trace.AdmitReadPointerWriteback();
  std::atomic<uint32_t> guest_read_pointer{0};
  guest_read_pointer.store(200, std::memory_order_release);
  auto commit =
      std::async(std::launch::async, [&trace, admission = std::move(readback_admission)]() mutable {
        trace.ReadPointerWriteback(std::move(admission), 0x200000, 0, 200);
        trace.ReadPointerWriteback(std::move(admission), 0x200000, 0, 200);
      });
  REQUIRE(guest_read_pointer.load(std::memory_order_acquire) == 200);
  REQUIRE(WaitForLockWaits(trace, 1));
  auto finalization = std::async(std::launch::async, [&trace] { return trace.FinishAndFlush(); });
  CHECK(finalization.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  state_lock.unlock();
  commit.get();

  CHECK(finalization.get());
  const auto stats = trace.statistics();
  CHECK(stats.valid_for_promotion());
  CHECK(stats.dropped_callbacks == 0);
  const auto events = Events(trace);
  const auto* readback = FindLast(events, XenosFenceTracePoint::kReadPointerWriteback);
  REQUIRE(readback);
  CHECK(std::count_if(events.begin(), events.end(), [](const XenosFenceTraceEvent& event) {
          return event.point == XenosFenceTracePoint::kReadPointerWriteback;
        }) == 1);
  {
    std::ifstream input(path);
    REQUIRE(input);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(contents.find(",read_pointer_writeback,") < contents.find(",trace_finished,"));
  }
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace reports an abandoned readback admission as a drop",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("readback-admission-abandoned");
  Initialize(trace, path);
  { auto readback_admission = trace.AdmitReadPointerWriteback(); }
  CHECK(trace.statistics().dropped_callbacks == 1);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace finish-first readback remains rejected",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("readback-finish-first");
  Initialize(trace, path);
  const uint64_t token = Watch(trace, 16);
  REQUIRE(token != 0);
  CHECK(Decode(trace, 16) == token);
  trace.D3D12SwapRecording(token, 1, false);
  trace.D3D12Submission(1, 2, 3, 1);
  trace.ConsumerRangeEnd(200, true);
  trace.D3D12FenceCompleted(3, 1, true);

  REQUIRE(trace.FinishAndFlush());
  auto admission = trace.AdmitReadPointerWriteback();
  trace.ReadPointerWriteback(std::move(admission), 0x200000, 0, 200);
  const auto stats = trace.statistics();
  CHECK(stats.in_flight == 1);
  CHECK(stats.unresolved == 1);
  CHECK_FALSE(stats.valid_for_promotion());
  CHECK_FALSE(FindLast(Events(trace), XenosFenceTracePoint::kReadPointerWriteback));
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace readback admission preserves occurrence time and commit sequence",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("readback-time-sequence");
  Initialize(trace, path);
  const uint64_t token = Watch(trace, 16);
  REQUIRE(token != 0);
  CHECK(Decode(trace, 16) == token);
  trace.ConsumerRangeEnd(200, true);

  auto admission = trace.AdmitReadPointerWriteback();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  trace.WritePointerPublished(0, 200);
  trace.ReadPointerWriteback(std::move(admission), 0x200000, 0, 200);

  const auto events = Events(trace);
  const auto* publication = FindLast(events, XenosFenceTracePoint::kWritePointerPublished);
  const auto* readback = FindLast(events, XenosFenceTracePoint::kReadPointerWriteback);
  REQUIRE(publication);
  REQUIRE(readback);
  CHECK(readback->monotonic_nanoseconds < publication->monotonic_nanoseconds);
  CHECK(readback->sequence > publication->sequence);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace readback admission rejects reentry with zero thread identity",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("readback-reentry-zero-thread");
  Initialize(trace, path);
  const uint32_t saved_thread_id = rex::thread::current_thread_id();
  rex::thread::set_current_thread_id(0);
  CHECK(XenosFenceTraceTestAccess::InvokeReadbackAdmissionReentry(trace));
  rex::thread::set_current_thread_id(saved_thread_id);
  CHECK(trace.statistics().reentry_failures == 1);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace reset overtaking readback admission rejects promotion",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("readback-reset");
  Initialize(trace, path);
  REQUIRE(Watch(trace, 16) != 0);
  REQUIRE(trace.ConsumerRangeBegin(0, 200) != 0);
  trace.ConsumerRangeEnd(200, true);

  auto admission = trace.AdmitReadPointerWriteback();
  trace.ResetObservationEpoch();
  trace.ReadPointerWriteback(std::move(admission), 0x200000, 0, 200);
  const auto stats = trace.statistics();
  CHECK(stats.unresolved == 1);
  CHECK_FALSE(stats.valid_for_promotion());
  CHECK_FALSE(FindLast(Events(trace), XenosFenceTracePoint::kReadPointerWriteback));
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace preserves callbacks under bounded concurrent stress",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("stress");
  Initialize(trace, path);

  std::vector<std::thread> threads;
  for (uint32_t thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&trace, thread] {
      for (uint32_t index = 0; index < 25; ++index) {
        trace.ReadPointerWritebackConfigured(0x200000 + (thread * 25 + index) * 4);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  const auto stats = trace.statistics();
  CHECK(stats.stored == 103);
  CHECK(stats.overflow == 0);
  CHECK(stats.dropped_callbacks == 0);
  CHECK(stats.reentry_failures == 0);
  const auto events = Events(trace);
  REQUIRE(events.size() == 103);
  for (size_t index = 0; index < events.size(); ++index) {
    CHECK(events[index].sequence == index + 1);
  }
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace promotion gates are independent and tolerate waits",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTraceStatistics stats{};
  stats.watched = 1;
  stats.lock_waits = 1;
  stats.maximum_lock_wait_nanoseconds = 100;
  CHECK(stats.valid_for_promotion());

  stats.watched = 0;
  CHECK_FALSE(stats.valid_for_promotion());
  stats.watched = 1;
  stats.dropped_callbacks = 1;
  CHECK_FALSE(stats.valid_for_promotion());
  stats.dropped_callbacks = 0;
  stats.reentry_failures = 1;
  CHECK_FALSE(stats.valid_for_promotion());
  stats.reentry_failures = 0;
  stats.overflow = 1;
  CHECK_FALSE(stats.valid_for_promotion());
  stats.overflow = 0;
  stats.in_flight = 1;
  CHECK_FALSE(stats.valid_for_promotion());
  stats.in_flight = 0;
  stats.unresolved = 1;
  CHECK_FALSE(stats.valid_for_promotion());
}

TEST_CASE("Xenos fence trace ring arithmetic handles wraparound", "[graphics][xenos_fence_trace]") {
  CHECK(XenosFenceTrace::RangeContainsIndex(500, 20, 510, 512));
  CHECK(XenosFenceTrace::RangeContainsIndex(500, 20, 3, 512));
  CHECK_FALSE(XenosFenceTrace::RangeContainsIndex(500, 20, 200, 512));
  CHECK_FALSE(XenosFenceTrace::RangeContainsIndex(20, 20, 20, 512));

  XenosFenceTrace trace;
  const auto path = TracePath("ring");
  Initialize(trace, path);
  const auto events = Events(trace);
  const auto* initialized = FindLast(events, XenosFenceTracePoint::kRingInitialized);
  REQUIRE(initialized);
  CHECK(initialized->ring_capacity_bytes == 2048);
  CHECK(initialized->ring_capacity_dwords == 512);
  CHECK(initialized->ring_wrap_mask == 511);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace bounds watched reservations and reports unrelated PM4",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("bound");
  Initialize(trace, path);

  for (uint32_t i = 0; i < kXenosFenceTraceWatchLimit; ++i) {
    CHECK(Watch(trace, 16) != 0);
  }
  CHECK(Watch(trace, 400) == 0);

  REQUIRE(trace.ConsumerRangeBegin(0, 300) != 0);
  trace.PrimaryPacketObserved(17, 24, true);
  CHECK(trace.SwapDecoded(24, 29, 0x300000, 6, 1280, 720) != 0);
  trace.PrimaryPacketObserved(24, 29, true);
  trace.PrimaryPacketObserved(200, 201, true);
  trace.ConsumerRangeEnd(300, true);

  const auto events = Events(trace);
  const auto* end = FindLast(events, XenosFenceTracePoint::kConsumerRangeEnd);
  REQUIRE(end);
  CHECK(end->flags & kXenosFenceTraceContainsWatchedSwap);
  CHECK(end->flags & kXenosFenceTraceContainsUnrelatedPm4);
  CHECK(trace.statistics().watched == kXenosFenceTraceWatchLimit);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace serializes concurrent reservation and publication callbacks",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("reservation-publication-race");
  Initialize(trace, path);

  std::atomic<bool> go{false};
  std::array<uint64_t, kXenosFenceTraceWatchLimit> tokens{};
  std::vector<std::thread> threads;
  for (uint32_t index = 0; index < kXenosFenceTraceWatchLimit; ++index) {
    threads.emplace_back([&trace, &go, &tokens, index] {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      tokens[index] = Watch(trace, 16 + index * 32);
    });
  }
  threads.emplace_back([&trace, &go] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    trace.WritePointerPublished(0, 400);
  });
  go.store(true, std::memory_order_release);
  for (auto& thread : threads) {
    thread.join();
  }

  for (uint64_t token : tokens) {
    CHECK(token != 0);
  }
  const auto stats = trace.statistics();
  CHECK(stats.watched == kXenosFenceTraceWatchLimit);
  CHECK(stats.dropped_callbacks == 0);
  CHECK(stats.reentry_failures == 0);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace supports multiple swaps in one submission",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("shared-submission");
  Initialize(trace, path);
  uint64_t first = Watch(trace, 16);
  uint64_t second = Watch(trace, 96);
  REQUIRE(first != 0);
  REQUIRE(second != 0);

  REQUIRE(trace.ConsumerRangeBegin(0, 180) != 0);
  uint64_t first_decoded = trace.SwapDecoded(24, 29, 0x300000, 6, 1280, 720);
  uint64_t second_decoded = trace.SwapDecoded(104, 109, 0x400000, 6, 1280, 720);
  CHECK(first_decoded == first);
  CHECK(second_decoded == second);
  trace.D3D12SwapRecording(first, 7, false);
  trace.D3D12SwapRecording(second, 7, false);
  trace.D3D12Submission(7, 0xABC, 0xDEF, 7);

  const auto events = Events(trace);
  const auto* submission = FindLast(events, XenosFenceTracePoint::kD3D12Submission);
  REQUIRE(submission);
  CHECK(submission->correlation_mask == 3);
  CHECK(submission->flags & kXenosFenceTraceSharedSubmission);
  CHECK(trace.HasSubmittedWatches());
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace distinguishes readback and completion order",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("ordering");
  Initialize(trace, path);
  uint64_t first = Watch(trace, 16);
  REQUIRE(first != 0);
  CHECK(Decode(trace, 16) == first);
  trace.D3D12SwapRecording(first, 1, true);
  trace.D3D12Submission(1, 0xA, 0xB, 1);
  trace.ConsumerRangeEnd(200, true);
  Readback(trace, 0x200004, 0, 200);
  Readback(trace, 0x200000, 0, 199);
  trace.D3D12FenceCompleted(0xB, 1, true);
  CHECK(trace.statistics().in_flight == 1);
  Readback(trace, 0x200000, 0, 200);

  uint64_t second = Watch(trace, 240);
  REQUIRE(second != 0);
  REQUIRE(trace.ConsumerRangeBegin(200, 400) != 0);
  CHECK(trace.SwapDecoded(248, 253, 0x400000, 6, 1280, 720) == second);
  trace.D3D12SwapRecording(second, 2, false);
  trace.D3D12Submission(2, 0xC, 0xB, 2);
  trace.ConsumerRangeEnd(400, true);
  Readback(trace, 0x200000, 200, 400);
  trace.D3D12FenceCompleted(0xB, 2, true);

  const auto events = Events(trace);
  bool saw_readback_before_completion = false;
  bool saw_completion_before_readback = false;
  for (const auto& event : events) {
    saw_readback_before_completion = saw_readback_before_completion ||
                                     (event.flags & kXenosFenceTraceReadbackBeforeCompletion) != 0;
    saw_completion_before_readback = saw_completion_before_readback ||
                                     (event.flags & kXenosFenceTraceCompletionBeforeReadback) != 0;
  }
  CHECK(saw_readback_before_completion);
  CHECK(saw_completion_before_readback);
  CHECK(trace.statistics().in_flight == 0);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace preserves concurrent consumer submission fence and readback",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("consumer-d3d-race");
  Initialize(trace, path);
  const uint64_t token = Watch(trace, 16);
  REQUIRE(token != 0);
  CHECK(Decode(trace, 16) == token);
  trace.D3D12SwapRecording(token, 1, false);

  std::thread submission([&trace] { trace.D3D12Submission(1, 0xA, 0xB, 1); });
  std::thread range_end([&trace] { trace.ConsumerRangeEnd(200, true); });
  submission.join();
  range_end.join();

  std::thread completion([&trace] { trace.D3D12FenceCompleted(0xB, 1, true); });
  std::thread readback([&trace] { Readback(trace, 0x200000, 0, 200); });
  completion.join();
  readback.join();

  const auto stats = trace.statistics();
  CHECK(stats.in_flight == 0);
  CHECK(stats.unresolved == 0);
  CHECK(stats.dropped_callbacks == 0);
  CHECK(stats.reentry_failures == 0);
  const auto events = Events(trace);
  REQUIRE(FindLast(events, XenosFenceTracePoint::kConsumerRangeEnd));
  REQUIRE(FindLast(events, XenosFenceTracePoint::kD3D12Submission));
  REQUIRE(FindLast(events, XenosFenceTracePoint::kD3D12FenceCompleted));
  REQUIRE(FindLast(events, XenosFenceTracePoint::kReadPointerWriteback));
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace excludes completed watches from later wrapped ranges",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("index-reuse");
  Initialize(trace, path);
  uint64_t first = Watch(trace, 16);
  REQUIRE(first != 0);
  CHECK(Decode(trace, 16) == first);
  trace.D3D12SwapRecording(first, 1, false);
  trace.D3D12Submission(1, 2, 3, 1);
  trace.ConsumerRangeEnd(200, true);
  Readback(trace, 0x200000, 0, 200);
  trace.D3D12FenceCompleted(3, 1, true);

  uint64_t second = Watch(trace, 16);
  REQUIRE(second != 0);
  REQUIRE(trace.ConsumerRangeBegin(500, 80) != 0);
  const auto events = Events(trace);
  const auto* range = FindLast(events, XenosFenceTracePoint::kConsumerRangeBegin);
  REQUIRE(range);
  CHECK(range->correlation_mask == 2);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace fences an observed reservation that was not decoded",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("not-decoded");
  Initialize(trace, path);
  REQUIRE(Watch(trace, 16) != 0);
  REQUIRE(trace.ConsumerRangeBegin(0, 100) != 0);
  trace.PrimaryPacketObserved(24, 29, true);
  trace.ConsumerRangeEnd(100, true);
  auto events = Events(trace);
  REQUIRE(FindLast(events, XenosFenceTracePoint::kWatchNotDecoded));
  CHECK(trace.statistics().unresolved == 1);

  uint64_t replacement = Watch(trace, 16);
  REQUIRE(replacement != 0);
  REQUIRE(trace.ConsumerRangeBegin(500, 80) != 0);
  CHECK(trace.SwapDecoded(24, 29, 0x300000, 6, 1280, 720) == replacement);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace rejects stale tokens after reset and times out unresolved work",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("reset-timeout");
  Initialize(trace, path);
  uint64_t stale = Watch(trace, 16);
  REQUIRE(stale != 0);
  trace.ResetObservationEpoch();
  trace.RingInitialized(0x100000, 8, 0, 0);
  trace.D3D12SwapRecording(stale, 1, false);
  trace.D3D12Submission(1, 2, 3, 1);
  CHECK_FALSE(trace.HasSubmittedWatches());

  uint64_t pending = Watch(trace, 32);
  REQUIRE(pending != 0);
  trace.PollTimeoutsAt(UINT64_MAX);
  const auto events = Events(trace);
  REQUIRE(FindLast(events, XenosFenceTracePoint::kWatchTimedOut));
  CHECK(trace.statistics().in_flight == 0);
  REQUIRE(trace.FinishAndFlush());
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace overflow and serialization are explicit and allowlisted",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("overflow");
  Initialize(trace, path);
  REQUIRE(Watch(trace, 16) != 0);
  for (uint32_t i = 0; i < kXenosFenceTraceCapacity + 32; ++i) {
    trace.WritePointerPublished(i, i + 1);
  }
  CHECK(trace.statistics().overflow > 0);
  CHECK(trace.statistics().dropped_callbacks > 0);
  REQUIRE(trace.FinishAndFlush());

  {
    std::ifstream input(path);
    REQUIRE(input);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(contents.find("schema=xenos_consumer_fence_trace_v2") != std::string::npos);
    CHECK(contents.find("lock_wait_count") != std::string::npos);
    CHECK(contents.find("maximum_lock_wait_nanoseconds") != std::string::npos);
    CHECK(contents.find("dropped_callback_count") != std::string::npos);
    CHECK(contents.find("reentry_failure_count") != std::string::npos);
    CHECK(contents.find("reservation_words") == std::string::npos);
    CHECK(contents.find("descriptor") == std::string::npos);
    CHECK(contents.find("filename") == std::string::npos);
    CHECK(contents.find(",trace_finished,") != std::string::npos);
  }
  std::filesystem::remove(path);
}

TEST_CASE("Xenos fence trace can retry a failed safe-boundary flush",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto directory = TracePath("retry-parent");
  const auto path = directory / "trace.csv";
  std::filesystem::remove_all(directory);
  Initialize(trace, path);
  CHECK_FALSE(trace.FinishAndFlush());
  CHECK(trace.enabled());
  REQUIRE(std::filesystem::create_directory(directory));
  REQUIRE(trace.FinishAndFlush());
  CHECK_FALSE(trace.enabled());

  {
    std::ofstream prior(path, std::ios::out | std::ios::trunc);
    REQUIRE(prior);
    prior << "prior-artifact";
  }
  XenosFenceTrace replacement;
  CHECK_FALSE(replacement.Start(path));
  {
    std::ifstream prior(path);
    REQUIRE(prior);
    std::string contents((std::istreambuf_iterator<char>(prior)), std::istreambuf_iterator<char>());
    CHECK(contents == "prior-artifact");
  }
  std::filesystem::remove_all(directory);
}

TEST_CASE("Xenos fence trace preserves a pre-existing temporary sidecar",
          "[graphics][xenos_fence_trace]") {
  XenosFenceTrace trace;
  const auto path = TracePath("existing-sidecar");
  std::filesystem::path temporary_path = path;
  temporary_path += ".tmp";
  std::filesystem::remove(path);
  {
    std::ofstream prior(temporary_path, std::ios::out | std::ios::trunc);
    REQUIRE(prior);
    prior << "prior-sidecar";
  }

  CHECK_FALSE(trace.Start(path));
  {
    std::ifstream prior(temporary_path);
    REQUIRE(prior);
    std::string contents((std::istreambuf_iterator<char>(prior)), std::istreambuf_iterator<char>());
    CHECK(contents == "prior-sidecar");
  }
  std::filesystem::remove(temporary_path);
}

}  // namespace rex::graphics::diagnostic
