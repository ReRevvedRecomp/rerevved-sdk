/**
 * @file        ui/bar_fill.h
 * @brief       Geometry for the `present_bar_fill` pillarbox/letterbox fill
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace rex::ui {

enum class BarFillMode {
  kBlack,  // Bars stay the cleared back buffer color.
  kBlur,   // Bars show a dimmed, blurred, aspect-filled copy of the guest output.
};

/// Parses a `present_bar_fill` value, case-insensitively. Unknown values are
/// kBlack.
inline BarFillMode ParseBarFillMode(std::string_view value) {
  constexpr std::string_view kBlur = "blur";
  if (value.size() != kBlur.size()) {
    return BarFillMode::kBlack;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) != kBlur[i]) {
      return BarFillMode::kBlack;
    }
  }
  return BarFillMode::kBlur;
}

/// Fraction of the blurred copy's brightness that reaches the bars.
inline constexpr float kBarFillDim = 0.4f;

/// The blur chain halves the guest output until neither side exceeds this.
inline constexpr uint32_t kBarFillBlurTargetSize = 96;

/// Upper bound of chain levels, enough for 2^8 * kBarFillBlurTargetSize.
inline constexpr size_t kBarFillBlurMaxLevels = 8;

struct BarFillRect {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

/// True when the fitted output rectangle leaves part of the host target
/// uncovered. The rectangle may extend outside the target.
inline bool HasBars(uint32_t host_width, uint32_t host_height, int32_t output_x, int32_t output_y,
                    uint32_t output_width, uint32_t output_height) {
  if (!host_width || !host_height || !output_width || !output_height) {
    return false;
  }
  return output_x > 0 || output_y > 0 ||
         int64_t(output_x) + int64_t(output_width) < int64_t(host_width) ||
         int64_t(output_y) + int64_t(output_height) < int64_t(host_height);
}

/// The smallest rectangle with the output rectangle's aspect ratio that covers
/// the host target, centered on it (aspect-fill: the overflowing axis is
/// cropped, never stretched). Returns an empty rectangle for degenerate input.
inline BarFillRect CalculateBarFillRect(uint32_t host_width, uint32_t host_height,
                                        uint32_t output_width, uint32_t output_height) {
  BarFillRect rect;
  if (!host_width || !host_height || !output_width || !output_height) {
    return rect;
  }
  if (uint64_t(host_width) * output_height >= uint64_t(host_height) * output_width) {
    rect.width = host_width;
    rect.height = uint32_t((uint64_t(host_width) * output_height + output_width - 1) /
                           uint64_t(output_width));
  } else {
    rect.height = host_height;
    rect.width = uint32_t((uint64_t(host_height) * output_width + output_height - 1) /
                          uint64_t(output_height));
  }
  // Rounding the offset towards the origin keeps the far edge covering the
  // target.
  rect.x = -int32_t((rect.width - host_width) / 2);
  rect.y = -int32_t((rect.height - host_height) / 2);
  return rect;
}

/// Fills `sizes_out` with the sizes of the successive 2x downsampling passes
/// from a source of the given size until neither side exceeds
/// kBarFillBlurTargetSize, always at least one level. Returns the level count
/// (at most max_levels, which must be at least 1).
inline size_t CalculateBarFillBlurLevels(uint32_t source_width, uint32_t source_height,
                                         std::pair<uint32_t, uint32_t>* sizes_out,
                                         size_t max_levels) {
  size_t count = 0;
  uint32_t width = std::max(source_width, uint32_t(1));
  uint32_t height = std::max(source_height, uint32_t(1));
  do {
    width = std::max((width + 1) / 2, uint32_t(1));
    height = std::max((height + 1) / 2, uint32_t(1));
    sizes_out[count++] = std::make_pair(width, height);
  } while (count < max_levels && std::max(width, height) > kBarFillBlurTargetSize);
  return count;
}

}  // namespace rex::ui
