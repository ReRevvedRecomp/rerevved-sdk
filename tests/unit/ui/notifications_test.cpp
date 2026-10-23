#include <chrono>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <rex/ui/notifications.h>

#include "ui/notification_queue.h"

namespace rex::ui {
namespace {
using namespace std::chrono_literals;
using Clock = NotificationQueue::Clock;

TEST_CASE("notification queue shows toasts one after another", "[notifications]") {
  NotificationQueue queue;
  CHECK_FALSE(queue.Front(Clock::now()));
  REQUIRE(queue.Post("First", "one", 2s));
  REQUIRE(queue.Post("Second", "two", 3s));

  // A toast's time starts when it is first returned, not when it was posted.
  const auto t0 = Clock::now() + 1h;
  auto shown = queue.Front(t0);
  REQUIRE(shown);
  CHECK(shown->title == "First");
  CHECK(shown->message == "one");
  CHECK(shown->age_seconds == 0.0f);
  CHECK(shown->duration_seconds == 2.0f);
  shown = queue.Front(t0 + 1999ms);
  REQUIRE(shown);
  CHECK(shown->title == "First");

  const auto t1 = t0 + 2s;
  shown = queue.Front(t1);
  REQUIRE(shown);
  CHECK(shown->title == "Second");
  CHECK(shown->age_seconds == 0.0f);
  CHECK(shown->duration_seconds == 3.0f);
  CHECK(queue.size() == 1);
  CHECK_FALSE(queue.Front(t1 + 3s));
  CHECK(queue.size() == 0);
}

TEST_CASE("notification queue clamps durations and bounds pending toasts", "[notifications]") {
  NotificationQueue queue;
  REQUIRE(queue.Post("Short", "", 0ms));
  const auto t0 = Clock::now();
  REQUIRE(queue.Front(t0));
  CHECK(queue.Front(t0)->duration_seconds == 1.0f);
  CHECK(queue.Front(t0 + 999ms));
  CHECK_FALSE(queue.Front(t0 + 1s));

  for (size_t i = 0; i < NotificationQueue::kMaxPending; ++i)
    REQUIRE(queue.Post("Toast", std::to_string(i), 1s));
  CHECK_FALSE(queue.Post("Overflow", "", 1s));
  CHECK(queue.size() == NotificationQueue::kMaxPending);
  CHECK(queue.Front(t0)->message == "0");
  queue.Clear();
  CHECK(queue.size() == 0);
}

TEST_CASE("PostNotification stays bounded without an overlay layer", "[notifications]") {
  Notifications().Clear();
  for (int i = 0; i < 40; ++i)
    PostNotification("Headless", "no overlay drains this queue");
  CHECK(Notifications().size() == NotificationQueue::kMaxPending);
  const auto shown = Notifications().Front(Clock::now());
  REQUIRE(shown);
  CHECK(shown->duration_seconds == 10.0f);
  Notifications().Clear();
}

}  // namespace
}  // namespace rex::ui
