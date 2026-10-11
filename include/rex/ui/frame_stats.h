/**
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#pragma once

#include <cstdint>

namespace rex::ui {

struct FrameStats {
  double frame_time_ms = 0;
  double fps = 0;
  uint64_t frame_count = 0;
};

}  // namespace rex::ui
