
/**
 * @file        ui/notifications.cpp
 * @brief       Notification queue. See rex/ui/notifications.h for the contract.
 */

#include "notification_queue.h"

#include <algorithm>

#include <rex/logging.h>
#include <rex/ui/notifications.h>

namespace rex::ui {

bool NotificationQueue::Post(std::string title, std::string message,
                             std::chrono::milliseconds duration) {
  std::lock_guard lock(mutex_);
  if (queue_.size() >= kMaxPending) {
    return false;
  }
  queue_.push_back({std::move(title), std::move(message), std::max(duration, kMinDuration), {}});
  return true;
}

std::optional<NotificationQueue::Visible> NotificationQueue::Front(Clock::time_point now) {
  std::lock_guard lock(mutex_);
  while (!queue_.empty()) {
    auto& front = queue_.front();
    if (!front.visible_since) {
      front.visible_since = now;
    }
    const auto age = now - *front.visible_since;
    if (age < front.duration) {
      return Visible{front.title, front.message, std::chrono::duration<float>(age).count(),
                     std::chrono::duration<float>(front.duration).count()};
    }
    queue_.pop_front();
  }
  return std::nullopt;
}

std::size_t NotificationQueue::size() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

void NotificationQueue::Clear() {
  std::lock_guard lock(mutex_);
  queue_.clear();
}

NotificationQueue& Notifications() {
  static NotificationQueue queue;
  return queue;
}

void PostNotification(std::string title, std::string message, std::chrono::milliseconds duration) {
  REXLOG_INFO("Notification: {}: {}", title, message);
  if (!Notifications().Post(std::move(title), std::move(message), duration)) {
    REXLOG_DEBUG("Notification dropped: {} toasts already waiting", NotificationQueue::kMaxPending);
  }
}

}  // namespace rex::ui
