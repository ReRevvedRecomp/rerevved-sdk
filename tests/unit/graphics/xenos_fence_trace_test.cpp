#include <rex/graphics/xenos_fence_trace.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace rex::graphics::diagnostic {

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
  REQUIRE(trace.Start(path));
  trace.RingInitialized(0x100000, 8, 0, 0);
  trace.ReadPointerWritebackConfigured(0x200000);
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
  trace.ReadPointerWriteback(0x200000, 7, 10);
  trace.D3D12SwapRecording(1, 1, false);
  trace.D3D12Submission(1, 2, 3, 1);
  trace.D3D12FenceCompleted(3, 1, true);
  trace.PollTimeoutsAt(UINT64_MAX);

  const auto stats = trace.statistics();
  CHECK(stats.stored == 0);
  CHECK(stats.watched == 0);
  CHECK_FALSE(std::filesystem::exists(path));
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
  trace.ReadPointerWriteback(0x200004, 0, 200);
  trace.ReadPointerWriteback(0x200000, 0, 199);
  trace.D3D12FenceCompleted(0xB, 1, true);
  CHECK(trace.statistics().in_flight == 1);
  trace.ReadPointerWriteback(0x200000, 0, 200);

  uint64_t second = Watch(trace, 240);
  REQUIRE(second != 0);
  REQUIRE(trace.ConsumerRangeBegin(200, 400) != 0);
  CHECK(trace.SwapDecoded(248, 253, 0x400000, 6, 1280, 720) == second);
  trace.D3D12SwapRecording(second, 2, false);
  trace.D3D12Submission(2, 0xC, 0xB, 2);
  trace.ConsumerRangeEnd(400, true);
  trace.ReadPointerWriteback(0x200000, 200, 400);
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
  trace.ReadPointerWriteback(0x200000, 0, 200);
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
  REQUIRE(trace.FinishAndFlush());

  {
    std::ifstream input(path);
    REQUIRE(input);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(contents.find("schema=xenos_consumer_fence_trace_v1") != std::string::npos);
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
