#include "graphics/d3d12/frame_capture.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <rex/graphics/register_file.h>

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

TEST_CASE("owned frame journal preserves ordered state and guest shaders",
          "[graphics][diagnostic_frame_capture]") {
  std::vector<uint32_t> registers(RegisterFile::kRegisterCount, 0xA5A5A5A5);
  const std::vector<uint32_t> vertex_ucode = {0x01020304, 0x05060708};
  const std::vector<uint32_t> pixel_ucode = {0x11121314};
  FrameCapture::ShaderView vertex_shader;
  vertex_shader.present = true;
  vertex_shader.hash = 0x1234;
  vertex_shader.dwords = vertex_ucode.data();
  vertex_shader.dword_count = vertex_ucode.size();
  FrameCapture::ShaderView pixel_shader;
  pixel_shader.present = true;
  pixel_shader.hash = 0x5678;
  pixel_shader.dwords = pixel_ucode.data();
  pixel_shader.dword_count = pixel_ucode.size();

  FrameCapture capture;
  REQUIRE(capture.BeginOwnedFrame(10, 20, 0x1000, 1280, 720, registers.data(), registers.size()));
  {
    auto draw = capture.BeginDraw(11, 21, 3, 0, false, {}, registers.data(), registers.size(),
                                  vertex_shader, pixel_shader);
    REQUIRE(draw.event_id());
    draw.SetResult(true);
  }
  {
    auto copy = capture.BeginCopy(11, 21, registers.data(), registers.size());
    REQUIRE(copy.event_id());
    copy.SetResult(true, "resolve");
  }
  REQUIRE_FALSE(
      capture.RecordSwapEnd(11, 22, 0x2000, 1280, 720, registers.data(), registers.size()).empty());

  const auto snapshot = capture.TakeOwnedFrame(11, 22);
  REQUIRE(snapshot);
  REQUIRE(snapshot->events.size() == 4);
  CHECK(snapshot->events[0].kind == diagnostic::DrawCaptureFrameEvent::Kind::kSwap);
  CHECK(snapshot->events[1].kind == diagnostic::DrawCaptureFrameEvent::Kind::kDraw);
  CHECK(snapshot->events[1].draw.guest_vertex_shader_hash == vertex_shader.hash);
  CHECK(snapshot->events[1].draw.guest_pixel_shader_hash == pixel_shader.hash);
  CHECK(snapshot->events[1].draw.draw.vertex_ucode == vertex_ucode);
  CHECK(snapshot->events[1].draw.draw.pixel_ucode == pixel_ucode);
  CHECK(snapshot->events[1].draw.draw.registers == registers);
  CHECK(snapshot->events[2].kind == diagnostic::DrawCaptureFrameEvent::Kind::kCopy);
  CHECK(snapshot->events[2].draw.draw.registers == registers);
  CHECK(snapshot->events[2].copy_mode == "resolve");
  CHECK(snapshot->events[3].kind == diagnostic::DrawCaptureFrameEvent::Kind::kSwap);
  CHECK(capture.IsIdle());
}

}  // namespace rex::graphics::d3d12
