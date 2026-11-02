#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  /// Runs on the pad-reading thread; must not re-enter this object. nullptr waits for a read.
  void SetGamepadCaptureCallback(std::function<bool()> callback);

  /// Any thread; ignores guest capture. False if no pad, or once after a pad change.
  bool GetNavigationPadState(X_INPUT_GAMEPAD* out_gamepad);

  /// Replaces any previous assignment. Call before the guest starts polling.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

  GamepadFamily ActiveGamepadFamily() const;

  /// Physical pads only; excludes keyboard and mouse emulation.
  bool HasConnectedGamepad() const;

 private:
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  void PublishGamepadFamily();
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;

  struct GamepadGate {
    bool captured = false;
    uint16_t hidden = 0;
    uint32_t packet_bias = 0;
  };
  /// Take `ticket` from NextPadSample before sampling so older samples cannot undo latches.
  GamepadGate ApplyGamepadCapture(DeviceId id, X_INPUT_STATE& state, uint16_t physical_buttons,
                                  uint64_t ticket);
  uint64_t NextPadSample();

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;

  struct PadHold {
    bool captured = false;
    uint16_t held = 0;
    uint64_t applied = 0;
    uint32_t packet_bias = 0;
  };
  std::mutex capture_mutex_;
  std::function<bool()> capture_callback_;
  std::map<DeviceId, PadHold> pad_holds_;
  uint64_t pad_sample_counter_ = 0;

  std::mutex nav_pad_mutex_;
  InputDriver* nav_pad_driver_ = nullptr;
  DeviceId nav_pad_id_ = DeviceId::kInvalid;
  uint32_t nav_pad_epoch_ = 0;
  uint32_t nav_pad_acked_ = 0;

  // Written on the poll path like devices_; readers use only the atomics.
  DeviceId last_physical_ = DeviceId::kInvalid;
  std::atomic<GamepadFamily> gamepad_family_{GamepadFamily::kOther};
  std::atomic<bool> gamepad_connected_{false};
};

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input
