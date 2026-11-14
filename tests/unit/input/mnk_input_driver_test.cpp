#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/ui_event.h>

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_mouse);
REXCVAR_DECLARE(bool, mnk_mouse_drag);
REXCVAR_DECLARE(bool, mnk_drag_invert);
REXCVAR_DECLARE(double, mnk_drag_sensitivity);
REXCVAR_DECLARE(int32_t, mnk_drag_min_deflection);

namespace rex::input::mnk {
struct MnkInputDriverTestAccess {
  static void Deflect(float dx, float dy, double sensitivity, bool invert, int32_t min_deflection,
                      int32_t& x, int32_t& y) {
    MnkInputDriver::DragStickDeflection(dx, dy, sensitivity, invert, min_deflection, x, y);
  }
  static bool CaptureRequested(const MnkInputDriver& driver) {
    return driver.mouse_capture_requested_.load();
  }
};
}  // namespace rex::input::mnk

namespace {
using rex::X_RESULT;
using rex::input::X_INPUT_STATE;
using rex::input::mnk::MnkInputDriver;
using Access = rex::input::mnk::MnkInputDriverTestAccess;
using Button = rex::ui::MouseEvent::Button;

constexpr rex::input::DeviceId kDevice = static_cast<rex::input::DeviceId>(0x4D4E4B00);

struct DragFlags {
  DragFlags() {
    REXCVAR_SET(mnk_mode, true);
    REXCVAR_SET(mnk_mouse, false);
    REXCVAR_SET(mnk_mouse_drag, true);
    REXCVAR_SET(mnk_drag_invert, true);
    REXCVAR_SET(mnk_drag_sensitivity, 1.0);
    REXCVAR_SET(mnk_drag_min_deflection, 0);
  }
  ~DragFlags() {
    REXCVAR_SET(mnk_mode, false);
    REXCVAR_SET(mnk_mouse, false);
    REXCVAR_SET(mnk_mouse_drag, false);
    REXCVAR_SET(mnk_drag_invert, true);
    REXCVAR_SET(mnk_drag_sensitivity, 1.0);
    REXCVAR_SET(mnk_drag_min_deflection, 0);
  }
};

void Press(MnkInputDriver& driver, Button button) {
  rex::ui::MouseEvent e(nullptr, button, 0, 0);
  driver.OnMouseDown(e);
}

void Release(MnkInputDriver& driver, Button button) {
  rex::ui::MouseEvent e(nullptr, button, 0, 0);
  driver.OnMouseUp(e);
}

// Each move reports motion from the previous position, which starts at (0, 0).
void MoveTo(MnkInputDriver& driver, int32_t x, int32_t y) {
  rex::ui::MouseEvent e(nullptr, Button::kNone, x, y);
  driver.OnMouseMove(e);
}

X_INPUT_STATE Poll(MnkInputDriver& driver) {
  X_INPUT_STATE state{};
  REQUIRE(driver.GetDeviceState(kDevice, &state) == X_ERROR_SUCCESS);
  return state;
}
}  // namespace

TEST_CASE("Drag deflection inverts and lifts slow drags past the minimum", "[input][mnk]") {
  int32_t x = 0;
  int32_t y = 0;

  // 10 px right and 5 px down at the 200x base scale, pushed against the mouse.
  Access::Deflect(10.0f, 5.0f, 1.0, true, 0, x, y);
  CHECK(x == -2000);
  CHECK(y == 1000);
  Access::Deflect(10.0f, 5.0f, 1.0, false, 0, x, y);
  CHECK(x == 2000);
  CHECK(y == -1000);

  // The larger axis reaches the minimum and the 2:1 direction survives.
  Access::Deflect(10.0f, 5.0f, 1.0, true, 8192, x, y);
  CHECK(x == -8192);
  CHECK(y == 4096);

  Access::Deflect(0.0f, 0.0f, 1.0, true, 8192, x, y);
  CHECK(x == 0);
  CHECK(y == 0);
  Access::Deflect(-1000.0f, 0.0f, 1.0, true, 8192, x, y);
  CHECK(x == INT16_MAX);
  CHECK(y == 0);
}

TEST_CASE("Drag drives the right stick only while the left button is held", "[input][mnk]") {
  DragFlags flags;
  MnkInputDriver driver(nullptr, 0);

  MoveTo(driver, 10, 0);
  X_INPUT_STATE state = Poll(driver);
  CHECK(state.gamepad.thumb_rx == 0);
  CHECK_FALSE(Access::CaptureRequested(driver));

  Press(driver, Button::kLeft);
  MoveTo(driver, 20, 0);
  state = Poll(driver);
  CHECK(state.gamepad.thumb_rx == -2000);
  CHECK(state.gamepad.thumb_ry == 0);
  CHECK(Access::CaptureRequested(driver));

  Release(driver, Button::kLeft);
  MoveTo(driver, 30, 0);
  state = Poll(driver);
  CHECK(state.gamepad.thumb_rx == 0);
  CHECK_FALSE(Access::CaptureRequested(driver));
}

TEST_CASE("Drag ignores other buttons and stays off with its setting off", "[input][mnk]") {
  DragFlags flags;
  MnkInputDriver driver(nullptr, 0);

  Press(driver, Button::kRight);
  MoveTo(driver, 10, 0);
  CHECK(Poll(driver).gamepad.thumb_rx == 0);
  Release(driver, Button::kRight);

  REXCVAR_SET(mnk_mouse_drag, false);
  Press(driver, Button::kLeft);
  MoveTo(driver, 20, 0);
  CHECK(Poll(driver).gamepad.thumb_rx == 0);
  CHECK_FALSE(Access::CaptureRequested(driver));

  // Mouse aim keeps its own direction and is not doubled by a held drag.
  REXCVAR_SET(mnk_mouse_drag, true);
  REXCVAR_SET(mnk_mouse, true);
  MoveTo(driver, 30, 0);
  CHECK(Poll(driver).gamepad.thumb_rx == 2000);
}

TEST_CASE("Drag ends on focus loss", "[input][mnk]") {
  DragFlags flags;
  MnkInputDriver driver(nullptr, 0);

  Press(driver, Button::kLeft);
  MoveTo(driver, 10, 0);
  CHECK(Poll(driver).gamepad.thumb_rx == -2000);

  rex::ui::UISetupEvent lost(nullptr);
  driver.OnLostFocus(lost);
  rex::ui::UISetupEvent got(nullptr);
  driver.OnGotFocus(got);

  // The button-up landed while unfocused, so the drag must not resume.
  MoveTo(driver, 20, 0);
  CHECK(Poll(driver).gamepad.thumb_rx == 0);
  CHECK_FALSE(Access::CaptureRequested(driver));
}

TEST_CASE("Drag never starts from a press an overlay owns", "[input][mnk]") {
  DragFlags flags;
  MnkInputDriver driver(nullptr, 0);
  bool overlay_open = true;
  driver.set_is_active_callback([&overlay_open] { return !overlay_open; });

  Press(driver, Button::kLeft);
  MoveTo(driver, 10, 0);
  CHECK(Poll(driver).gamepad.thumb_rx == 0);
  CHECK_FALSE(Access::CaptureRequested(driver));

  // The overlay closes under the held button; the press still belongs to it.
  overlay_open = false;
  MoveTo(driver, 20, 0);
  CHECK(Poll(driver).gamepad.thumb_rx == 0);
  CHECK_FALSE(Access::CaptureRequested(driver));

  Release(driver, Button::kLeft);
  Press(driver, Button::kLeft);
  MoveTo(driver, 30, 0);
  CHECK(Poll(driver).gamepad.thumb_rx == -2000);
}
