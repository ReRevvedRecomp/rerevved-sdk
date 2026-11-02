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

#include <algorithm>
#include <cmath>

#include <rex/dbg.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
REXCVAR_DEFINE_BOOL(input_vibration, true, "Input", "Vibration. Send rumble to the controller.")
    .audience(rex::cvar::Audience::kPlayer);
namespace rex::input {

namespace {

// Synthetic devices are parked past every physical ordinal so they cannot push
// a real pad off guest user 0. SlotAssignment routes them by their synthetic
// flag and never reads this value.
constexpr uint32_t kSyntheticOrdinal = UINT32_MAX;

// Pad button a keystroke virtual key reports; 0 for analog and unknown keys.
uint16_t ButtonForVirtualKey(uint16_t virtual_key) {
  using VK = rex::ui::VirtualKey;
  switch (static_cast<VK>(virtual_key)) {
    case VK::kXInputPadDpadUp:
      return X_INPUT_GAMEPAD_DPAD_UP;
    case VK::kXInputPadDpadDown:
      return X_INPUT_GAMEPAD_DPAD_DOWN;
    case VK::kXInputPadDpadLeft:
      return X_INPUT_GAMEPAD_DPAD_LEFT;
    case VK::kXInputPadDpadRight:
      return X_INPUT_GAMEPAD_DPAD_RIGHT;
    case VK::kXInputPadStart:
      return X_INPUT_GAMEPAD_START;
    case VK::kXInputPadBack:
      return X_INPUT_GAMEPAD_BACK;
    case VK::kXInputPadLThumbPress:
      return X_INPUT_GAMEPAD_LEFT_THUMB;
    case VK::kXInputPadRThumbPress:
      return X_INPUT_GAMEPAD_RIGHT_THUMB;
    case VK::kXInputPadLShoulder:
      return X_INPUT_GAMEPAD_LEFT_SHOULDER;
    case VK::kXInputPadRShoulder:
      return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    case VK::kXInputPadA:
      return X_INPUT_GAMEPAD_A;
    case VK::kXInputPadB:
      return X_INPUT_GAMEPAD_B;
    case VK::kXInputPadX:
      return X_INPUT_GAMEPAD_X;
    case VK::kXInputPadY:
      return X_INPUT_GAMEPAD_Y;
    default:
      return 0;
  }
}

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  {
    // GetNavigationPadState may be mid-poll on the UI thread.
    std::lock_guard<std::mutex> lock(nav_pad_mutex_);
    nav_pad_driver_ = nullptr;
    nav_pad_id_ = DeviceId::kInvalid;
    nav_pad_acked_ = nav_pad_epoch_;
  }
  // device_owners_ holds raw driver pointers.
  devices_.clear();
  device_owners_.clear();
  drivers_.clear();
  last_physical_ = DeviceId::kInvalid;
  PublishGamepadFamily();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
  // The UI needs pad families before the guest's first poll.
  RefreshDevices();
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetGamepadCaptureCallback(std::function<bool()> callback) {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  capture_callback_ = std::move(callback);
}

bool InputSystem::GetNavigationPadState(X_INPUT_GAMEPAD* out_gamepad) {
  std::lock_guard<std::mutex> lock(nav_pad_mutex_);
  if (!nav_pad_driver_) {
    nav_pad_acked_ = nav_pad_epoch_;
    return false;
  }
  if (nav_pad_acked_ != nav_pad_epoch_) {
    // Force the UI to handle this as a newly arrived pad.
    nav_pad_acked_ = nav_pad_epoch_;
    return false;
  }
  X_INPUT_STATE state = {};
  const uint64_t ticket = NextPadSample();
  if (nav_pad_driver_->GetDevicePhysicalState(nav_pad_id_, &state) != X_ERROR_SUCCESS) {
    return false;
  }
  X_INPUT_STATE latched = state;
  ApplyGamepadCapture(nav_pad_id_, latched, state.gamepad.buttons, ticket);
  if (out_gamepad) {
    *out_gamepad = state.gamepad;
  }
  return true;
}

uint64_t InputSystem::NextPadSample() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  return ++pad_sample_counter_;
}

InputSystem::GamepadGate InputSystem::ApplyGamepadCapture(DeviceId id, X_INPUT_STATE& state,
                                                          uint16_t physical_buttons,
                                                          uint64_t ticket) {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  PadHold& hold = pad_holds_[id];
  const bool captured = capture_callback_ && capture_callback_();
  const uint16_t physical = physical_buttons;
  const bool stale = ticket < hold.applied;
  hold.applied = std::max(hold.applied, ticket);
  // Keep captured buttons hidden until released, even after capture ends.
  uint16_t held = captured ? physical : static_cast<uint16_t>(hold.held & physical);
  if (stale) {
    held = hold.held;
  }
  if (captured != hold.captured || held != hold.held) {
    ++hold.packet_bias;
    hold.captured = captured;
    hold.held = held;
  }
  if (captured) {
    state.gamepad = X_INPUT_GAMEPAD{};
  } else {
    // The driver may already be hiding buttons of its own.
    state.gamepad.buttons = static_cast<uint16_t>(state.gamepad.buttons & ~held);
  }
  return {captured, held, hold.packet_bias};
}

void InputSystem::SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment) {
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void InputSystem::RefreshDevices() {
  std::vector<DeviceInfo> seen;
  std::vector<InputDriver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  // Carry forward ordinals already handed out, so a device keeps its guest user
  // when another pad is unplugged.
  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Preserve live device assignments, then give each new device the lowest
  // free ordinal. Reconnects count as new devices, so a growing counter could
  // exceed the guest user limit.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Only physical devices consume an ordinal, so the first pad to connect is
    // guest user 0 however many synthetic devices enumerated ahead of it.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](uint32_t candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    uint32_t ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        pad_holds_.erase(old.id);
      }
      if (last_physical_ == old.id) {
        last_physical_ = DeviceId::kInvalid;
      }
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable: synthetic devices share one ordinal, and their relative order
  // decides which answers GetCapabilities when no pad is attached.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
  PublishGamepadFamily();
}

void InputSystem::PublishGamepadFamily() {
  GamepadFamily family = GamepadFamily::kOther;
  // devices_ is in ordinal order, so this is the lowest connected pad.
  const DeviceInfo* first_physical = nullptr;
  for (const auto& device : devices_) {
    if (!device.synthetic) {
      first_physical = &device;
      break;
    }
  }
  if (const auto* info = DeviceInfoFor(last_physical_)) {
    family = info->family;
  } else if (first_physical) {
    family = first_physical->family;
  }
  gamepad_family_.store(family, std::memory_order_relaxed);
  gamepad_connected_.store(first_physical != nullptr, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(nav_pad_mutex_);
    const DeviceId id = first_physical ? first_physical->id : DeviceId::kInvalid;
    if (id != nav_pad_id_) {
      // A pad that appears where the UI last saw none needs no announcement.
      const bool seen_absent =
          nav_pad_acked_ == nav_pad_epoch_ && nav_pad_id_ == DeviceId::kInvalid;
      ++nav_pad_epoch_;
      if (seen_absent) {
        nav_pad_acked_ = nav_pad_epoch_;
      }
    }
    nav_pad_id_ = id;
    nav_pad_driver_ = first_physical ? DriverForDevice(first_physical->id) : nullptr;
  }
}

GamepadFamily InputSystem::ActiveGamepadFamily() const {
  return gamepad_family_.load(std::memory_order_relaxed);
}

bool InputSystem::HasConnectedGamepad() const {
  return gamepad_connected_.load(std::memory_order_relaxed);
}

InputDriver* InputSystem::DriverForDevice(DeviceId id) {
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* InputSystem::DeviceInfoFor(DeviceId id) const {
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  if (!out_caps || !assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  if (ids.empty()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Prefer the pad in hand, so button glyphs follow it rather than whichever
  // device enumerated first.
  DeviceId chosen = active_devices_.Active(user_index);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }

  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return driver->GetDeviceCapabilities(chosen, flags, out_caps);
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  uint32_t gate_bias = 0;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    // UI capture excludes keyboard and mouse emulation.
    const DeviceInfo* info = DeviceInfoFor(id);
    const bool physical = info && !info->synthetic;
    // Sample physical buttons first, including driver-hidden buttons. A release
    // between samples stays hidden one more poll rather than letting an older
    // guest sample appear as a press.
    X_INPUT_STATE raw = {};
    const uint64_t ticket = physical ? NextPadSample() : 0;
    const bool have_raw = physical && driver->GetDevicePhysicalState(id, &raw) == X_ERROR_SUCCESS;
    X_INPUT_STATE state = {};
    if (driver->GetDeviceState(id, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    if (physical) {
      const uint16_t buttons = have_raw ? static_cast<uint16_t>(raw.gamepad.buttons)
                                        : static_cast<uint16_t>(state.gamepad.buttons);
      gate_bias += ApplyGamepadCapture(id, state, buttons, ticket).packet_bias;
    }
    active_devices_.Observe(user_index, id, state.gamepad);
    if (!IsNeutral(state.gamepad) && id != last_physical_ && physical) {
      last_physical_ = id;
      PublishGamepadFamily();
    }
    if (!any) {
      merged = state;
      any = true;
    } else {
      MergeInto(merged, state);
    }
  }

  if (!any) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // Bias after merging so a larger keyboard packet cannot mask a pad gate change.
  merged.packet_number = static_cast<uint32_t>(merged.packet_number) + gate_bias;
  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  // Dispatch zeroes too, so disabling vibration stops an already running motor.
  X_INPUT_VIBRATION silent = {};
  if (!REXCVAR_GET(input_vibration)) {
    vibration = &silent;
  }

  // Every pad on this user belongs to the same player, so all of them buzz.
  // Only pads decide the result: synthetic devices accept any vibration and
  // would otherwise report success for a pad that never rumbled.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  X_RESULT pad_error = X_ERROR_DEVICE_NOT_CONNECTED;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    const DeviceInfo* info = DeviceInfoFor(id);
    if (!driver || !info) {
      continue;
    }
    X_RESULT result = driver->SetDeviceVibration(id, vibration);
    if (info->synthetic) {
      any_synthetic = true;
      continue;
    }
    any_pad = true;
    if (result == X_ERROR_SUCCESS) {
      pad_rumbled = true;
    } else {
      pad_error = result;
    }
  }
  if (any_pad) {
    return pad_rumbled ? X_ERROR_SUCCESS : pad_error;
  }
  return any_synthetic ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    GamepadGate gate;
    if (const DeviceInfo* info = DeviceInfoFor(id); info && !info->synthetic) {
      // Keep the gate current for keystroke-only polls; let the driver produce
      // the keystroke to preserve its edge tracking.
      X_INPUT_STATE physical = {};
      const uint64_t ticket = NextPadSample();
      if (driver->GetDevicePhysicalState(id, &physical) == X_ERROR_SUCCESS) {
        gate = ApplyGamepadCapture(id, physical, physical.gamepad.buttons, ticket);
      }
    }
    X_RESULT result = driver->GetDeviceKeystroke(id, flags, out_keystroke);
    if (result == X_ERROR_SUCCESS &&
        (gate.captured || (gate.hidden & ButtonForVirtualKey(out_keystroke->virtual_key)))) {
      result = X_ERROR_EMPTY;
    }
    if (result == X_ERROR_SUCCESS) {
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
      return result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace rex::input
