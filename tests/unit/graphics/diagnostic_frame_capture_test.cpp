#include "graphics/d3d12/frame_capture.h"

#include <catch2/catch_test_macros.hpp>

namespace rex::graphics::d3d12 {

TEST_CASE("frame capture policy bounds the journal", "[graphics][diagnostic_frame_capture]") {
  CHECK(FrameCapturePolicy::CanAppend(0, 0, FrameCapturePolicy::kMaxBytes));
  CHECK_FALSE(FrameCapturePolicy::CanAppend(0, 1, FrameCapturePolicy::kMaxBytes));
  CHECK_FALSE(FrameCapturePolicy::CanAppend(FrameCapturePolicy::kMaxEvents, 0, 0));
  CHECK(FrameCapturePolicy::CanAppend(FrameCapturePolicy::kMaxEvents - 1,
                                      FrameCapturePolicy::kMaxBytes - 1, 1));
}

TEST_CASE("frame capture policy requires both completed guest boundaries",
          "[graphics][diagnostic_frame_capture]") {
  CHECK_FALSE(FrameCapturePolicy::IsComplete(false, true, true, true, true));
  CHECK_FALSE(FrameCapturePolicy::IsComplete(true, false, true, true, true));
  CHECK_FALSE(FrameCapturePolicy::IsComplete(true, true, false, true, true));
  CHECK_FALSE(FrameCapturePolicy::IsComplete(true, true, true, false, true));
  CHECK_FALSE(FrameCapturePolicy::IsComplete(true, true, true, true, false));
  CHECK(FrameCapturePolicy::IsComplete(true, true, true, true, true));
}

}  // namespace rex::graphics::d3d12
