#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/ui/frame_stats.h>

using Clock = rex::ui::FrameRateMeter::Clock;
using std::chrono::milliseconds;

TEST_CASE("frame rate meter reports frames per second over one-second windows", "[ui]") {
  rex::ui::FrameRateMeter meter;
  const auto start = Clock::time_point{} + std::chrono::hours(1);
  CHECK(meter.Snapshot(start).frame_count == 0);

  // 61 frames 1/60 s apart span exactly one second.
  for (int i = 0; i <= 60; ++i) {
    meter.Record(start + milliseconds(i * 1000) / 60);
  }
  const auto stats = meter.Snapshot(start + milliseconds(1000));
  CHECK(stats.frame_count == 61);
  CHECK(stats.fps == Catch::Approx(60.0).epsilon(0.01));
  CHECK(stats.frame_time_ms == Catch::Approx(1000.0 / 60.0).epsilon(0.01));
}

TEST_CASE("frame rate meter reads zero after frames stop", "[ui]") {
  rex::ui::FrameRateMeter meter;
  const auto start = Clock::time_point{} + std::chrono::hours(1);
  for (int i = 0; i <= 30; ++i) {
    meter.Record(start + milliseconds(i * 1000) / 30);
  }
  CHECK(meter.Snapshot(start + milliseconds(1500)).fps > 0.0);
  const auto stale = meter.Snapshot(start + milliseconds(2001));
  CHECK(stale.fps == 0.0);
  CHECK(stale.frame_count == 31);
}
