#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rex::graphics::d3d12 {

// Scalar-only diagnostics on the command processor thread. No input views,
// guest-memory tokens or GPU resources survive a callback here.
class FrameInputCosts {
 public:
  enum class Stage : size_t {
    kDrawTotal,
    kDrawShader,
    kDrawTextures,
    kDrawCallback,
    kCopyTotal,
    kCopyCallback,
    kBeginCallback,
    kSwapCallback,
    kEndCallback,
    kGuestRead,
    kCount,
  };

  enum class Texture : size_t {
    kUnavailable,
    kNotRequired,
    kGpuWritten,
    kCurrent,
    kStale,
    kMissing,
    kCount,
  };

  enum class Reason { kWindow, kDetached, kReplaced, kFailed, kShutdown };

  struct Timing {
    uint64_t calls = 0;
    uint64_t nanoseconds = 0;
  };

  struct Report {
    uint64_t stream = 0;
    uint64_t observer = 0;
    Reason reason = Reason::kWindow;
    uint64_t first_frame = 0;
    uint64_t last_frame = 0;
    uint64_t first_submission = 0;
    uint64_t last_submission = 0;
    uint64_t completed_frames = 0;
    std::array<Timing, static_cast<size_t>(Stage::kCount)> timings{};
    std::array<uint64_t, static_cast<size_t>(Texture::kCount)> textures{};
    uint64_t read_requests = 0;
    uint64_t read_successes = 0;
    uint64_t requested_bytes = 0;
    uint64_t copied_bytes = 0;
    uint64_t callback_failures = 0;
    bool touched = false;

    const Timing& timing(Stage stage) const { return timings[static_cast<size_t>(stage)]; }
    uint64_t texture(Texture state) const { return textures[static_cast<size_t>(state)]; }
  };

  // The first frame is reported alone; subsequent windows cover loading and
  // owner-driven play without an unbounded per-draw log or a gameplay timer.
  static constexpr uint64_t kFramesPerReport = 120;
  static constexpr uint64_t kMaxReports = 256;

  std::optional<Report> Bind(bool enabled, const void* owner, uint64_t observer) noexcept {
    owner = enabled && observer ? owner : nullptr;
    observer = owner ? observer : 0;
    if (owner == owner_ && observer == observer_) {
      return std::nullopt;
    }
    auto report = Flush(owner ? Reason::kReplaced : Reason::kDetached);
    owner_ = owner;
    observer_ = observer;
    window_ = {};
    if (owner) {
      ++stream_;
    }
    return report;
  }

  bool active() const noexcept { return owner_ && reports_ < kMaxReports; }
  uint64_t stream() const noexcept { return stream_; }
  FrameInputCosts* ForOwner(const void* owner) noexcept {
    return active() && owner == owner_ ? this : nullptr;
  }

  void Touch(uint64_t frame, uint64_t submission) noexcept {
    if (!active()) {
      return;
    }
    if (!window_.touched) {
      window_.first_frame = frame;
      window_.first_submission = submission;
      window_.touched = true;
    }
    window_.last_frame = frame;
    window_.last_submission = submission;
  }

  void AddTiming(Stage stage, uint64_t stream, uint64_t nanoseconds) noexcept {
    if (!active() || stream != stream_) {
      return;
    }
    auto& timing = window_.timings[static_cast<size_t>(stage)];
    ++timing.calls;
    timing.nanoseconds += nanoseconds;
  }

  void AddTexture(Texture state) noexcept {
    if (active()) {
      ++window_.textures[static_cast<size_t>(state)];
    }
  }

  void ReadRequest(uint64_t size) noexcept {
    if (active()) {
      ++window_.read_requests;
      window_.requested_bytes += size;
    }
  }

  void ReadSucceeded(uint64_t size) noexcept {
    if (active()) {
      ++window_.read_successes;
      window_.copied_bytes += size;
    }
  }

  void CallbackFailed() noexcept {
    if (active()) {
      ++window_.callback_failures;
    }
  }

  std::optional<Report> EndFrame(bool succeeded) noexcept {
    if (!active()) {
      return std::nullopt;
    }
    if (!succeeded) {
      return Flush(Reason::kFailed);
    }
    ++window_.completed_frames;
    return window_.completed_frames >= (reports_ ? kFramesPerReport : 1) ? Flush(Reason::kWindow)
                                                                         : std::nullopt;
  }

  std::optional<Report> Flush(Reason reason) noexcept {
    if (!active() || !window_.touched) {
      return std::nullopt;
    }
    Report report = window_;
    report.stream = stream_;
    report.observer = observer_;
    report.reason = reason;
    ++reports_;
    window_ = {};
    return report;
  }

 private:
  const void* owner_ = nullptr;
  uint64_t observer_ = 0;
  uint64_t stream_ = 0;
  // The cap spans observer replacements and detach/re-attach cycles.
  uint64_t reports_ = 0;
  Report window_{};
};

class FrameInputCostScope {
 public:
  using Clock = std::chrono::steady_clock;

  FrameInputCostScope(FrameInputCosts* costs, FrameInputCosts::Stage stage) noexcept
      : costs_(costs && costs->active() ? costs : nullptr), stage_(stage) {
    if (costs_) {
      stream_ = costs_->stream();
      started_ = Clock::now();
    }
  }
  ~FrameInputCostScope() noexcept { Finish(); }
  void Finish() noexcept {
    if (costs_) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started_);
      costs_->AddTiming(stage_, stream_, static_cast<uint64_t>(elapsed.count()));
      costs_ = nullptr;
    }
  }
  FrameInputCostScope(const FrameInputCostScope&) = delete;
  FrameInputCostScope& operator=(const FrameInputCostScope&) = delete;

 private:
  FrameInputCosts* costs_ = nullptr;
  FrameInputCosts::Stage stage_;
  uint64_t stream_ = 0;
  Clock::time_point started_{};
};

}  // namespace rex::graphics::d3d12
