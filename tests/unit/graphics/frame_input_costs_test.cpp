#include "graphics/d3d12/frame_input_costs.h"

#include <catch2/catch_test_macros.hpp>

namespace rex::graphics::d3d12 {

TEST_CASE("Frame input costs disabled path retains no observations", "[graphics][frame_input]") {
  FrameInputCosts costs;
  CHECK_FALSE(costs.Bind(false, &costs, 1));
  CHECK(costs.ForOwner(&costs) == nullptr);
  {
    FrameInputCostScope scope(costs.ForOwner(&costs), FrameInputCosts::Stage::kDrawTotal);
    costs.Touch(10, 20);
    costs.ReadRequest(64);
    costs.ReadSucceeded(64);
    costs.AddTexture(FrameInputCosts::Texture::kCurrent);
  }
  CHECK_FALSE(costs.EndFrame(true));
  CHECK_FALSE(costs.Flush(FrameInputCosts::Reason::kShutdown));
}

TEST_CASE("Frame input costs separates nested callbacks and memory traffic",
          "[graphics][frame_input]") {
  using Stage = FrameInputCosts::Stage;
  using Texture = FrameInputCosts::Texture;
  FrameInputCosts costs;
  REQUIRE_FALSE(costs.Bind(true, &costs, 7));
  costs.Touch(10, 20);
  costs.AddTiming(Stage::kDrawTotal, costs.stream(), 150);
  costs.AddTiming(Stage::kDrawCallback, costs.stream(), 100);
  costs.AddTiming(Stage::kGuestRead, costs.stream(), 40);
  costs.ReadRequest(64);
  costs.ReadSucceeded(64);
  costs.ReadRequest(32);
  for (size_t i = 0; i < static_cast<size_t>(Texture::kCount); ++i) {
    costs.AddTexture(static_cast<Texture>(i));
  }
  costs.CallbackFailed();
  costs.Touch(11, 21);
  const auto report = costs.EndFrame(true);
  REQUIRE(report);
  CHECK(report->observer == 7);
  CHECK(report->first_frame == 10);
  CHECK(report->last_frame == 11);
  CHECK(report->first_submission == 20);
  CHECK(report->last_submission == 21);
  CHECK(report->completed_frames == 1);
  CHECK(report->timing(Stage::kDrawTotal).nanoseconds -
            report->timing(Stage::kDrawCallback).nanoseconds ==
        50);
  CHECK(report->timing(Stage::kGuestRead).nanoseconds == 40);
  CHECK(report->read_requests == 2);
  CHECK(report->read_successes == 1);
  CHECK(report->requested_bytes == 96);
  CHECK(report->copied_bytes == 64);
  CHECK(report->callback_failures == 1);
  CHECK(report->texture(Texture::kCurrent) == 1);
  CHECK(report->texture(Texture::kMissing) == 1);
  CHECK(report->texture(Texture::kStale) == 1);
  CHECK(report->texture(Texture::kGpuWritten) == 1);
  CHECK(report->texture(Texture::kNotRequired) == 1);
  CHECK(report->texture(Texture::kUnavailable) == 1);

  for (uint64_t i = 1; i < FrameInputCosts::kFramesPerReport; ++i) {
    costs.Touch(11 + i, 21 + i);
    CHECK_FALSE(costs.EndFrame(true));
  }
  costs.Touch(200, 300);
  const auto window = costs.EndFrame(true);
  REQUIRE(window);
  CHECK(window->completed_frames == FrameInputCosts::kFramesPerReport);
  CHECK(window->read_requests == 0);
  CHECK(window->timing(Stage::kDrawTotal).calls == 0);

  costs.Touch(201, 301);
  {
    FrameInputCostScope scope(&costs, Stage::kCopyTotal);
    scope.Finish();
    scope.Finish();
  }
  const auto partial = costs.Flush(FrameInputCosts::Reason::kShutdown);
  REQUIRE(partial);
  CHECK(partial->timing(Stage::kCopyTotal).calls == 1);
}

TEST_CASE("Frame input costs flushes partial windows without crossing streams",
          "[graphics][frame_input]") {
  using Reason = FrameInputCosts::Reason;
  FrameInputCosts costs;
  REQUIRE_FALSE(costs.Bind(true, &costs, 1));
  costs.Touch(1, 2);
  {
    FrameInputCostScope old_scope(&costs, FrameInputCosts::Stage::kDrawCallback);
    const auto replaced = costs.Bind(true, &costs, 2);
    REQUIRE(replaced);
    CHECK(replaced->observer == 1);
    CHECK(replaced->reason == Reason::kReplaced);
  }
  costs.Touch(3, 4);
  const auto failed = costs.EndFrame(false);
  REQUIRE(failed);
  CHECK(failed->observer == 2);
  CHECK(failed->completed_frames == 0);
  CHECK(failed->reason == Reason::kFailed);
  CHECK(failed->timing(FrameInputCosts::Stage::kDrawCallback).calls == 0);
  costs.Touch(5, 6);
  const auto detached = costs.Bind(false, &costs, 2);
  REQUIRE(detached);
  CHECK(detached->reason == Reason::kDetached);
  CHECK(costs.ForOwner(&costs) == nullptr);
  CHECK_FALSE(costs.Flush(Reason::kShutdown));
}

TEST_CASE("Frame input costs caps reports across observer replacements",
          "[graphics][frame_input]") {
  FrameInputCosts costs;
  for (uint64_t i = 1; i <= FrameInputCosts::kMaxReports; ++i) {
    REQUIRE_FALSE(costs.Bind(true, &costs, i));
    costs.Touch(i, i);
    REQUIRE(costs.Flush(FrameInputCosts::Reason::kDetached));
    CHECK_FALSE(costs.Bind(false, nullptr, 0));
  }
  CHECK_FALSE(costs.Bind(true, &costs, 999));
  CHECK_FALSE(costs.active());
  CHECK(costs.ForOwner(&costs) == nullptr);
  costs.Touch(999, 999);
  CHECK_FALSE(costs.EndFrame(true));
  CHECK_FALSE(costs.Flush(FrameInputCosts::Reason::kShutdown));
}

}  // namespace rex::graphics::d3d12
