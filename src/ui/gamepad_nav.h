/**
 * @file        ui/gamepad_nav.h
 * @brief       Feeds a physical pad to Dear ImGui's gamepad navigation.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <imgui.h>

#include <rex/input/input.h>

namespace rex::ui {

class GamepadNav {
 public:
  /// Call once per frame before ImGui::NewFrame; `pad` is null if disconnected.
  void Update(ImGuiIO& io, bool wanted, const input::X_INPUT_GAMEPAD* pad);

  void Release(ImGuiIO& io);

  /// Any thread.
  void NotifyWindowOpened() { cursor_pending_.store(true, std::memory_order_relaxed); }

 private:
  enum Slot : size_t {
    kDpadUp,
    kDpadDown,
    kDpadLeft,
    kDpadRight,
    kActivate,
    kCancel,
    kTextInput,
    kPrevTab,
    kNextTab,
    kClose,
    kSlotCount,
  };

  void Send(ImGuiIO& io, Slot slot, float value);

  bool active_ = false;
  bool pad_ready_ = false;
  bool cancel_down_ = false;
  bool cancel_closes_ = false;
  std::atomic<bool> cursor_pending_{false};
  uint32_t suppressed_ = 0;
  std::array<float, kSlotCount> sent_ = {};
};

}  // namespace rex::ui
