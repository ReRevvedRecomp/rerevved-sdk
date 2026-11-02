/**
 * @file        ui/gamepad_nav.cpp
 * @brief       Pad to ImGui key mapping. See gamepad_nav.h for details.
 */

#include "gamepad_nav.h"

#include <algorithm>
#include <cstdlib>

#include <rex/input/state_merge.h>

namespace rex::ui {

namespace {

// The stronger stick axis wins to prevent two moves on diagonals.
struct StickStep {
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;
};

StickStep StickDirection(const input::X_INPUT_GAMEPAD& pad) {
  StickStep step;
  // The pad reports up as positive, like XInput.
  const int x = pad.thumb_lx;
  const int y = pad.thumb_ly;
  if (std::max(std::abs(x), std::abs(y)) <= rex::input::kThumbDeadzone) {
    return step;
  }
  if (std::abs(x) >= std::abs(y)) {
    step.left = x < 0;
    step.right = x > 0;
  } else {
    step.up = y > 0;
    step.down = y < 0;
  }
  return step;
}

}  // namespace

void GamepadNav::Update(ImGuiIO& io, bool wanted, const input::X_INPUT_GAMEPAD* pad) {
  if (!wanted) {
    Release(io);
    return;
  }

  if (!active_) {
    active_ = true;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    cursor_pending_.store(true, std::memory_order_relaxed);
  }
  // Highlight the first item before any D-pad press.
  if (cursor_pending_.exchange(false, std::memory_order_relaxed)) {
    ImGui::SetNavCursorVisible(true);
  }

  std::array<float, kSlotCount> values = {};
  if (pad) {
    const uint16_t buttons = pad->buttons;
    const auto digital = [buttons](uint16_t mask) { return (buttons & mask) ? 1.0f : 0.0f; };
    const StickStep stick = StickDirection(*pad);
    values[kDpadUp] = stick.up ? 1.0f : digital(input::X_INPUT_GAMEPAD_DPAD_UP);
    values[kDpadDown] = stick.down ? 1.0f : digital(input::X_INPUT_GAMEPAD_DPAD_DOWN);
    values[kDpadLeft] = stick.left ? 1.0f : digital(input::X_INPUT_GAMEPAD_DPAD_LEFT);
    values[kDpadRight] = stick.right ? 1.0f : digital(input::X_INPUT_GAMEPAD_DPAD_RIGHT);
    values[kActivate] = digital(input::X_INPUT_GAMEPAD_A);
    values[kCancel] = digital(input::X_INPUT_GAMEPAD_B);
    values[kTextInput] = digital(input::X_INPUT_GAMEPAD_Y);
    values[kPrevTab] = digital(input::X_INPUT_GAMEPAD_LEFT_SHOULDER);
    values[kNextTab] = digital(input::X_INPUT_GAMEPAD_RIGHT_SHOULDER);
    if (!pad_ready_) {
      // Ignore presses intended for the previous input owner.
      pad_ready_ = true;
      for (size_t i = 0; i < kSlotCount; ++i) {
        if (values[i] > 0.0f) {
          suppressed_ |= 1u << i;
        }
      }
    }
  } else {
    pad_ready_ = false;
  }

  // Decide before ImGui handles this frame, so popup cancel cannot close a window.
  const bool cancel_down = values[kCancel] > 0.0f && !(suppressed_ & (1u << kCancel));
  if (cancel_down && !cancel_down_) {
    cancel_closes_ =
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  }
  cancel_down_ = cancel_down;
  values[kClose] = cancel_down && cancel_closes_ ? 1.0f : 0.0f;

  for (size_t i = 0; i < kSlotCount; ++i) {
    if (values[i] <= 0.0f) {
      suppressed_ &= ~(1u << i);
    } else if (suppressed_ & (1u << i)) {
      values[i] = 0.0f;
    }
    Send(io, static_cast<Slot>(i), values[i]);
  }
}

void GamepadNav::Release(ImGuiIO& io) {
  if (!active_) {
    return;
  }
  for (size_t i = 0; i < kSlotCount; ++i) {
    Send(io, static_cast<Slot>(i), 0.0f);
  }
  io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
  active_ = false;
  pad_ready_ = false;
  cancel_down_ = false;
  cancel_closes_ = false;
  suppressed_ = 0;
}

void GamepadNav::Send(ImGuiIO& io, Slot slot, float value) {
  if (sent_[slot] == value) {
    return;
  }
  sent_[slot] = value;
  static constexpr ImGuiKey kKeys[kSlotCount] = {
      ImGuiKey_GamepadDpadUp,    ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft,
      ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
      ImGuiKey_GamepadFaceUp,    ImGuiKey_GamepadL1,       ImGuiKey_GamepadR1,
      ImGuiKey_GamepadBack,
  };
  io.AddKeyAnalogEvent(kKeys[slot], value > 0.0f, value);
}

}  // namespace rex::ui
