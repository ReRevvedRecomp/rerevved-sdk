#include <rex/graphics/diagnostic_draw_capture.h>

#include <catch2/catch_test_macros.hpp>

namespace rex::graphics::diagnostic {

TEST_CASE("draw capture eligibility reports the first unsupported condition",
          "[graphics][diagnostic_draw_capture]") {
  DrawCaptureEligibility eligibility;
  CHECK(GetDrawCaptureSkipReason(eligibility) == DrawCaptureSkipReason::kRenderTargetPath);

  eligibility.rov_path = true;
  CHECK(GetDrawCaptureSkipReason(eligibility) == DrawCaptureSkipReason::kResolutionScale);
  eligibility.resolution_scale_is_one = true;
  CHECK(GetDrawCaptureSkipReason(eligibility) == DrawCaptureSkipReason::kMissingShader);
  eligibility.vertex_shader_present = true;
  eligibility.pixel_shader_present = true;
  CHECK(GetDrawCaptureSkipReason(eligibility) == DrawCaptureSkipReason::kPrimitive);
  eligibility.rasterized_triangle_list = true;
  CHECK(GetDrawCaptureSkipReason(eligibility) == DrawCaptureSkipReason::kStencilOnly);
  eligibility.color_target_written = true;
  CHECK(GetDrawCaptureSkipReason(eligibility) == DrawCaptureSkipReason::kMemexport);
  eligibility.memexport_unused = true;
  CHECK(GetDrawCaptureSkipReason(eligibility) == DrawCaptureSkipReason::kTessellation);
  eligibility.tessellation_disabled = true;
  CHECK(GetDrawCaptureSkipReason(eligibility) == DrawCaptureSkipReason::kIndexSource);
  eligibility.original_index_source = true;
  CHECK_FALSE(GetDrawCaptureSkipReason(eligibility).has_value());
  eligibility.used_texture_mask = 1;
  CHECK(GetDrawCaptureSkipReason(eligibility) == DrawCaptureSkipReason::kTexture);
}

TEST_CASE("draw capture range validation rejects overflow and bounded totals",
          "[graphics][diagnostic_draw_capture]") {
  DrawCaptureRange range;
  CHECK(ValidateDrawCaptureRange(0x100, 0x200, 0x1000, 0x400, range));
  CHECK(range.base == 0x100);
  CHECK(range.size == 0x200);
  CHECK_FALSE(ValidateDrawCaptureRange(0xF00, 0x200, 0x1000, 0x400, range));
  CHECK_FALSE(ValidateDrawCaptureRange(0x1000, 1, 0x1000, 0x400, range));
  CHECK_FALSE(ValidateDrawCaptureRange(0, 0x401, 0x1000, 0x400, range));

  std::vector<DrawCaptureRange> ranges;
  uint64_t total_size = 0;
  CHECK(AppendDrawCaptureRange(0, 0x200, 0x1000, 0x300, ranges, total_size));
  CHECK_FALSE(AppendDrawCaptureRange(0x200, 0x200, 0x1000, 0x300, ranges, total_size));
  CHECK(total_size == 0x200);
}

}  // namespace rex::graphics::diagnostic
