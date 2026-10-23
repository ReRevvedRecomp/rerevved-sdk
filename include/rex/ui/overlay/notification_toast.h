/**
 * @file        rex/ui/overlay/notification_toast.h
 * @brief       Draws toasts queued by rex::ui::PostNotification.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

// Shows one queued notification at a time in the bottom-right corner, above
// the achievement toast, with the ToastStyle colors and the chrome panel skin.
// ReXApp creates it with the overlay stack; a title without ReXApp may create
// one on its own ImGuiDrawer.
class NotificationToastDialog : public ImGuiDialog {
 public:
  explicit NotificationToastDialog(ImGuiDrawer* imgui_drawer);
  ~NotificationToastDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace rex::ui
