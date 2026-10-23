
/**
 * @file        ui/notification_queue.h
 * @brief       Ordered toast queue behind rex::ui::PostNotification.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace rex::ui {

// Thread-safe FIFO of toasts. Only the front toast is visible; its duration
// starts the first time Front() returns it.
class NotificationQueue {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::size_t kMaxPending = 16;
  static constexpr std::chrono::milliseconds kMinDuration{1000};

  struct Visible {
    std::string title;
    std::string message;
    float age_seconds = 0.0f;
    float duration_seconds = 0.0f;
  };

  // False when kMaxPending toasts already wait.
  bool Post(std::string title, std::string message, std::chrono::milliseconds duration);
  // Drops expired toasts and returns the one to draw at `now`, if any.
  std::optional<Visible> Front(Clock::time_point now);
  std::size_t size() const;
  void Clear();

 private:
  struct Entry {
    std::string title;
    std::string message;
    std::chrono::milliseconds duration{};
    std::optional<Clock::time_point> visible_since;
  };

  mutable std::mutex mutex_;
  std::deque<Entry> queue_;
};

// The process queue that PostNotification fills and the toast dialog drains.
NotificationQueue& Notifications();

}  // namespace rex::ui
