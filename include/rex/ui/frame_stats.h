/**
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace rex::ui {

struct FrameStats {
  double frame_time_ms = 0;
  double fps = 0;
  uint64_t frame_count = 0;
};

// Thread-safe frame rate over one-second windows. A source that stops
// producing frames reads as 0 FPS after one second.
class FrameRateMeter final {
 public:
  using Clock = std::chrono::steady_clock;

  void Record(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    last_frame_ = now;
    if (!stats_.frame_count++) {
      window_start_ = now;
      return;
    }
    ++intervals_;
    const auto elapsed = now - window_start_;
    if (elapsed >= std::chrono::seconds(1)) {
      const double seconds = std::chrono::duration<double>(elapsed).count();
      stats_.fps = double(intervals_) / seconds;
      stats_.frame_time_ms = seconds * 1000.0 / double(intervals_);
      window_start_ = now;
      intervals_ = 0;
    }
  }

  FrameStats Snapshot(Clock::time_point now) const {
    std::lock_guard lock(mutex_);
    FrameStats result = stats_;
    if (result.frame_count && now - last_frame_ >= std::chrono::seconds(1)) {
      result.fps = 0;
      result.frame_time_ms = 0;
    }
    return result;
  }

 private:
  mutable std::mutex mutex_;
  FrameStats stats_;
  uint64_t intervals_ = 0;
  Clock::time_point window_start_;
  Clock::time_point last_frame_;
};

}  // namespace rex::ui
