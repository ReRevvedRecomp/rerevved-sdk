/**
 ******************************************************************************
 * ReXGlue - Bounded Xenos draw capture helpers                              *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace rex::graphics::diagnostic {

// The marker is checked only from D3D12CommandProcessor::IssueSwap after the
// capture directory has been selected by the cvar.
inline constexpr char kDrawCaptureArmMarker[] = "arm";

// A capture is deliberately bounded so malformed guest state cannot turn a
// diagnostic one-shot into an unbounded readback or file operation. The limits
// are 96 ranges, 16 MiB of guest geometry, and 4 MiB per shader.
inline constexpr uint32_t kDrawCaptureMaxRangeCount = 96;
inline constexpr uint64_t kDrawCaptureMaxGeometryBytes = UINT64_C(16) * 1024 * 1024;
inline constexpr uint64_t kDrawCaptureMaxShaderBytes = UINT64_C(4) * 1024 * 1024;

struct DrawCaptureRange {
  uint32_t base = 0;
  uint32_t size = 0;
};

enum class DrawCaptureSkipReason : uint32_t {
  kRenderTargetPath,
  kResolutionScale,
  kMissingShader,
  kPrimitive,
  kStencilOnly,
  kTexture,
  kMemexport,
  kTessellation,
  kIndexSource,
  kMalformedInput,
  kRangeLimit,
  kReadbackAllocation,
  kSubmissionFailure,
  kFileWrite,
  kCount,
};

struct DrawCaptureEligibility {
  bool rov_path = false;
  bool resolution_scale_is_one = false;
  bool vertex_shader_present = false;
  bool pixel_shader_present = false;
  bool rasterized_triangle_list = false;
  bool color_target_written = false;
  bool memexport_unused = false;
  bool tessellation_disabled = false;
  bool original_index_source = false;
  uint32_t used_texture_mask = 0;
};

inline std::optional<DrawCaptureSkipReason> GetDrawCaptureSkipReason(
    const DrawCaptureEligibility& eligibility) {
  if (!eligibility.rov_path) {
    return DrawCaptureSkipReason::kRenderTargetPath;
  }
  if (!eligibility.resolution_scale_is_one) {
    return DrawCaptureSkipReason::kResolutionScale;
  }
  if (!eligibility.vertex_shader_present || !eligibility.pixel_shader_present) {
    return DrawCaptureSkipReason::kMissingShader;
  }
  if (!eligibility.rasterized_triangle_list) {
    return DrawCaptureSkipReason::kPrimitive;
  }
  if (!eligibility.color_target_written) {
    return DrawCaptureSkipReason::kStencilOnly;
  }
  if (eligibility.used_texture_mask != 0) {
    return DrawCaptureSkipReason::kTexture;
  }
  if (!eligibility.memexport_unused) {
    return DrawCaptureSkipReason::kMemexport;
  }
  if (!eligibility.tessellation_disabled) {
    return DrawCaptureSkipReason::kTessellation;
  }
  if (!eligibility.original_index_source) {
    return DrawCaptureSkipReason::kIndexSource;
  }
  return std::nullopt;
}

inline const char* GetDrawCaptureSkipReasonName(DrawCaptureSkipReason reason) {
  switch (reason) {
    case DrawCaptureSkipReason::kRenderTargetPath:
      return "render_target_path";
    case DrawCaptureSkipReason::kResolutionScale:
      return "resolution_scale";
    case DrawCaptureSkipReason::kMissingShader:
      return "missing_shader";
    case DrawCaptureSkipReason::kPrimitive:
      return "primitive";
    case DrawCaptureSkipReason::kStencilOnly:
      return "stencil_only";
    case DrawCaptureSkipReason::kTexture:
      return "texture";
    case DrawCaptureSkipReason::kMemexport:
      return "memexport";
    case DrawCaptureSkipReason::kTessellation:
      return "tessellation";
    case DrawCaptureSkipReason::kIndexSource:
      return "index_source";
    case DrawCaptureSkipReason::kMalformedInput:
      return "malformed_input";
    case DrawCaptureSkipReason::kRangeLimit:
      return "range_limit";
    case DrawCaptureSkipReason::kReadbackAllocation:
      return "readback_allocation";
    case DrawCaptureSkipReason::kSubmissionFailure:
      return "submission_failure";
    case DrawCaptureSkipReason::kFileWrite:
      return "file_write";
    case DrawCaptureSkipReason::kCount:
      break;
  }
  return "unknown";
}

inline bool ValidateDrawCaptureRange(uint64_t base, uint64_t size, uint64_t aperture_size,
                                     uint64_t max_size, DrawCaptureRange& range_out) {
  range_out = {};
  if (!size || size > max_size || base > std::numeric_limits<uint32_t>::max() ||
      size > std::numeric_limits<uint32_t>::max() || base > aperture_size ||
      size > aperture_size - base) {
    return false;
  }
  range_out.base = uint32_t(base);
  range_out.size = uint32_t(size);
  return true;
}

inline bool AppendDrawCaptureRange(uint64_t base, uint64_t size, uint64_t aperture_size,
                                   uint64_t max_total_size, std::vector<DrawCaptureRange>& ranges,
                                   uint64_t& total_size_out) {
  DrawCaptureRange range;
  if (ranges.size() >= kDrawCaptureMaxRangeCount ||
      !ValidateDrawCaptureRange(base, size, aperture_size, max_total_size, range) ||
      total_size_out > max_total_size - range.size) {
    return false;
  }
  ranges.push_back(range);
  total_size_out += range.size;
  return true;
}

}  // namespace rex::graphics::diagnostic
