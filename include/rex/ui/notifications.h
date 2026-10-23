/**
 * @file        rex/ui/notifications.h
 * @brief       Generic on-screen notification toasts
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <string>

namespace rex::ui {

// Thread-safe. Queues a toast drawn by the app's overlay layer (styled like the
// achievement toast, using the app Style/chrome) for `duration`; several queue
// one after another. Each toast's time starts when it is first drawn, and a
// duration below one second shows for one second. Every post is also logged.
// At most 16 toasts wait at once; later posts are dropped until one is shown,
// so a host without an overlay layer, such as a headless test, stays bounded.
void PostNotification(std::string title, std::string message,
                      std::chrono::milliseconds duration = std::chrono::seconds(10));

}  // namespace rex::ui
