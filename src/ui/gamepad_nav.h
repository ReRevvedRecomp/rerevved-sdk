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

/// Overlay navigation: D-pad/left stick move, A activates, Y starts text input,
/// B cancels an item or popup, and windows handle LB/RB tabs. With neither item
/// nor popup active, B holds ImGuiKey_GamepadBack for OverlayPadClosePressed.
/// Decide before ImGui::NewFrame so popup cancel cannot close its window.
/// Buttons held on pad arrival count only after release.
class GamepadNav {
 public:
  /// Call once per frame before ImGui::NewFrame with the ImGui context current.
  /// `wanted` means an interactive window is open; `pad` is null if disconnected.
  void Update(ImGuiIO& io, bool wanted, const input::X_INPUT_GAMEPAD* pad);

  /// Releases every key and turns navigation off.
  void Release(ImGuiIO& io);

  /// Shows focus next frame even if hidden by the mouse. Any thread.
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
  // Pad seen since navigation started or the last disconnect.
  bool pad_ready_ = false;
  bool cancel_down_ = false;
  bool cancel_closes_ = false;
  std::atomic<bool> cursor_pending_{false};
  // Suppress slots held on pad arrival until released.
  uint32_t suppressed_ = 0;
  std::array<float, kSlotCount> sent_ = {};
};

}  // namespace rex::ui
