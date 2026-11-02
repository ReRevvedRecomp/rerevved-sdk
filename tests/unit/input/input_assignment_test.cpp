#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_system.h>

// The X_STATUS_* / X_ERROR_* macros cast through these unqualified, so
// FakeDriver cannot match the base class signatures without them.
using rex::X_RESULT;
using rex::X_STATUS;

using rex::input::DeviceAssignment;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::GamepadFamily;
using rex::input::InputDriver;
using rex::input::InputSystem;
using rex::input::SharedAssignment;
using rex::input::SlotAssignment;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_GAMEPAD;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

DeviceInfo Pad(uint64_t id, uint32_t ordinal) {
  DeviceInfo info;
  info.id = static_cast<DeviceId>(id);
  info.ordinal = ordinal;
  return info;
}

std::vector<DeviceId> For(const DeviceAssignment& a, uint32_t user) {
  std::vector<DeviceId> out;
  a.DevicesForUser(user, out);
  return out;
}

/// Exercises assignment with no SDL, window, or OS behind it.
class FakeDriver final : public InputDriver {
 public:
  FakeDriver() : InputDriver(nullptr, 0) {}
  ~FakeDriver() override {
    if (destroyed) {
      *destroyed = true;
    }
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void Add(uint64_t id, bool synthetic = false, GamepadFamily family = GamepadFamily::kOther) {
    DeviceInfo info;
    info.id = static_cast<DeviceId>(id);
    info.synthetic = synthetic;
    info.family = family;
    devices_.push_back(info);
    states_[static_cast<DeviceId>(id)] = {};
  }

  void Remove(uint64_t id) {
    std::erase_if(devices_, [&](const DeviceInfo& d) { return d.id == static_cast<DeviceId>(id); });
    states_.erase(static_cast<DeviceId>(id));
  }

  void SetButtons(uint64_t id, uint16_t buttons) {
    states_[static_cast<DeviceId>(id)].gamepad.buttons = buttons;
  }

  void SetPacket(uint64_t id, uint32_t packet) {
    states_[static_cast<DeviceId>(id)].packet_number = packet;
  }

  void SetGuestGated(bool gated) { guest_gated_ = gated; }

  void SetLeftTrigger(uint64_t id, uint8_t value) {
    states_[static_cast<DeviceId>(id)].gamepad.left_trigger = value;
  }

  void QueueKeystroke(uint64_t id, uint16_t vk) {
    X_INPUT_KEYSTROKE ks = {};
    ks.virtual_key = vk;
    keystrokes_[static_cast<DeviceId>(id)] = ks;
  }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override { out = devices_; }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    auto it = states_.find(id);
    if (it == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out_state = it->second;
    if (guest_gated_) {
      out_state->gamepad = {};
    }
    if (after_state_read) {
      after_state_read();
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDevicePhysicalState(DeviceId id, X_INPUT_STATE* out_state) override {
    auto it = states_.find(id);
    if (it == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out_state = it->second;
    if (after_physical_read) {
      after_physical_read();
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    // Stamp the id so the test can tell which device answered.
    out->sub_type = static_cast<uint8_t>(id);
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    if (!states_.count(id)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    vibrations[id] = *vibration;
    ++vibration_calls;
    return vibration_result;
  }

  std::function<void()> after_state_read;
  std::function<void()> after_physical_read;
  bool* destroyed = nullptr;
  std::map<DeviceId, X_INPUT_VIBRATION> vibrations;
  size_t vibration_calls = 0;
  X_RESULT vibration_result = X_ERROR_SUCCESS;

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    auto it = keystrokes_.find(id);
    if (it == keystrokes_.end()) {
      return X_ERROR_EMPTY;
    }
    *out = it->second;
    return X_ERROR_SUCCESS;
  }

 private:
  bool guest_gated_ = false;
  std::vector<DeviceInfo> devices_;
  std::map<DeviceId, X_INPUT_STATE> states_;
  std::map<DeviceId, X_INPUT_KEYSTROKE> keystrokes_;
};

std::unique_ptr<InputSystem> SharedSystem(FakeDriver** out_driver) {
  auto driver = std::make_unique<FakeDriver>();
  *out_driver = driver.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(driver));
  system->SetDeviceAssignment(std::make_unique<SharedAssignment>());
  return system;
}

struct RestoreVibration {
  bool previous = REXCVAR_GET(input_vibration);
  ~RestoreVibration() { REXCVAR_SET(input_vibration, previous); }
};

}  // namespace

TEST_CASE("SharedAssignment puts every device on user 0", "[input]") {
  SharedAssignment shared;
  shared.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});

  REQUIRE(For(shared, 0) ==
          std::vector<DeviceId>{static_cast<DeviceId>(10), static_cast<DeviceId>(11)});
  REQUIRE(For(shared, 1).empty());
}

TEST_CASE("SlotAssignment does not renumber survivors when a pad is unplugged", "[input]") {
  SlotAssignment slots;
  slots.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});
  slots.OnDevicesChanged({Pad(11, 1)});

  REQUIRE(For(slots, 0).empty());
  REQUIRE(For(slots, 1) == std::vector<DeviceId>{static_cast<DeviceId>(11)});
}

TEST_CASE("Both controllers reach one guest user", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
}

TEST_CASE("Unplugging one controller leaves the other working", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Remove(1);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("Capabilities follow the controller in hand", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  REQUIRE(caps.sub_type == 2);
}

TEST_CASE("A keystroke from the second controller reaches the guest", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->QueueKeystroke(2, 0x5800);

  X_INPUT_KEYSTROKE ks = {};
  REQUIRE(system->GetKeystroke(0, 0, &ks) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(ks.virtual_key) == 0x5800);
}

TEST_CASE("A reconnected pad comes back to guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  driver->Add(1);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  // Replugging arrives as a new device id, not as the old one coming back.
  driver->Remove(1);
  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("A synthetic device does not push the first pad off guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  // The NOP stand-in is always present and enumerates before any pad connects.
  driver->Add(1, true);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(1, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("Vibration setting defaults to enabled for players", "[input]") {
  const auto* entry = rex::cvar::GetFlagInfo("input_vibration");
  REQUIRE(entry != nullptr);
  CHECK(entry->type == rex::cvar::FlagType::Boolean);
  CHECK(entry->default_value == "true");
  CHECK(entry->category == "Input");
  CHECK(entry->description == "Vibration. Send rumble to the controller.");
  CHECK(entry->audience == rex::cvar::Audience::kPlayer);
  CHECK(entry->lifecycle == rex::cvar::Lifecycle::kHotReload);
  CHECK(entry->persistence == rex::cvar::Persistence::kConfig);
}

TEST_CASE("Vibration gate dispatches zeroes to every assigned driver", "[input]") {
  RestoreVibration restore;
  FakeDriver* first = nullptr;
  auto system = SharedSystem(&first);
  first->Add(1);
  first->Add(2);
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* second = owned.get();
  second->Add(3);
  second->Add(4, true);
  system->AddDriver(std::move(owned));

  X_INPUT_VIBRATION request = {};
  request.left_motor_speed = 1234;
  request.right_motor_speed = 5678;
  for (const bool enabled : {true, false, true}) {
    REQUIRE(rex::cvar::SetFlagByName("input_vibration", enabled ? "true" : "false"));
    REQUIRE(system->SetState(0, &request) == X_ERROR_SUCCESS);
    for (const auto* driver : {first, second}) {
      REQUIRE(driver->vibrations.size() == 2);
      for (const auto& [id, vibration] : driver->vibrations) {
        CHECK(static_cast<uint16_t>(vibration.left_motor_speed) == (enabled ? 1234 : 0));
        CHECK(static_cast<uint16_t>(vibration.right_motor_speed) == (enabled ? 5678 : 0));
      }
    }
    CHECK(static_cast<uint16_t>(request.left_motor_speed) == 1234);
    CHECK(static_cast<uint16_t>(request.right_motor_speed) == 5678);
  }
  CHECK(first->vibration_calls == 6);
  CHECK(second->vibration_calls == 6);
}

TEST_CASE("Vibration gate preserves driver results and disconnected users", "[input]") {
  RestoreVibration restore;
  REQUIRE(rex::cvar::SetFlagByName("input_vibration", "false"));
  FakeDriver* first = nullptr;
  auto system = SharedSystem(&first);
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* second = owned.get();
  system->AddDriver(std::move(owned));
  X_INPUT_VIBRATION request = {};

  SECTION("no devices") {
    CHECK(system->SetState(0, &request) == X_ERROR_DEVICE_NOT_CONNECTED);
  }
  SECTION("synthetic only") {
    first->Add(1, true);
    CHECK(system->SetState(0, &request) == X_ERROR_SUCCESS);
  }
  SECTION("synthetic success does not hide pad failure") {
    first->Add(1);
    first->vibration_result = X_ERROR_FUNCTION_FAILED;
    second->Add(2, true);
    CHECK(system->SetState(0, &request) == X_ERROR_FUNCTION_FAILED);
  }
  SECTION("one successful pad is enough") {
    first->Add(1);
    first->vibration_result = X_ERROR_FUNCTION_FAILED;
    second->Add(2);
    CHECK(system->SetState(0, &request) == X_ERROR_SUCCESS);
  }
  first->Add(10);
  CHECK(system->SetState(1, &request) == X_ERROR_DEVICE_NOT_CONNECTED);
  InputSystem unassigned(nullptr);
  CHECK(unassigned.SetState(0, &request) == X_ERROR_DEVICE_NOT_CONNECTED);
}

// ReXApp::OnDestroy shuts the input system down before it destroys the window,
// so drivers never touch a freed window (upstream issue 525). The later
// Runtime teardown calls Shutdown again.
TEST_CASE("input system shutdown destroys drivers and is repeatable", "[input]") {
  bool destroyed = false;
  auto driver = std::make_unique<FakeDriver>();
  driver->destroyed = &destroyed;
  driver->Add(1);
  InputSystem system(nullptr);
  system.AddDriver(std::move(driver));
  CHECK_FALSE(destroyed);
  system.Shutdown();
  CHECK(destroyed);
  system.Shutdown();
}

TEST_CASE("Gamepad family falls back to the first connected pad", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  CHECK(system->ActiveGamepadFamily() == GamepadFamily::kOther);
  CHECK_FALSE(system->HasConnectedGamepad());
  driver->Add(9, true, GamepadFamily::kXbox);
  system->AttachWindow(nullptr);
  CHECK_FALSE(system->HasConnectedGamepad());
  driver->Add(1, false, GamepadFamily::kPlayStation);
  driver->Add(2, false, GamepadFamily::kXbox);
  system->AttachWindow(nullptr);
  CHECK(system->ActiveGamepadFamily() == GamepadFamily::kPlayStation);
  CHECK(system->HasConnectedGamepad());
}

TEST_CASE("An unknown pad still counts as connected", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1, false, GamepadFamily::kOther);
  system->AttachWindow(nullptr);
  CHECK(system->ActiveGamepadFamily() == GamepadFamily::kOther);
  CHECK(system->HasConnectedGamepad());
  system->Shutdown();
  CHECK_FALSE(system->HasConnectedGamepad());
}

TEST_CASE("Gamepad family follows the physical pad in hand", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(9, true, GamepadFamily::kOther);
  driver->Add(1, false, GamepadFamily::kPlayStation);
  driver->Add(2, false, GamepadFamily::kXbox);
  X_INPUT_STATE state = {};

  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(system->ActiveGamepadFamily() == GamepadFamily::kXbox);

  driver->SetButtons(2, 0);
  driver->SetButtons(9, X_INPUT_GAMEPAD_A);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(system->ActiveGamepadFamily() == GamepadFamily::kXbox);

  driver->Remove(2);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(system->ActiveGamepadFamily() == GamepadFamily::kPlayStation);

  driver->Remove(1);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(system->ActiveGamepadFamily() == GamepadFamily::kOther);
  CHECK_FALSE(system->HasConnectedGamepad());

  system->Shutdown();
  CHECK(system->ActiveGamepadFamily() == GamepadFamily::kOther);
}

TEST_CASE("Gamepad family forgets an unplugged pad whose id returns", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1, false, GamepadFamily::kXbox);
  driver->Add(2, false, GamepadFamily::kXbox);
  X_INPUT_STATE state = {};
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Remove(2);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  driver->Add(2, false, GamepadFamily::kPlayStation);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(system->ActiveGamepadFamily() == GamepadFamily::kXbox);
}

namespace {

constexpr uint16_t kVkPadA = 0x5800;
constexpr uint16_t kVkPadB = 0x5801;

uint16_t Buttons(const X_INPUT_STATE& state) {
  return static_cast<uint16_t>(state.gamepad.buttons);
}

uint32_t Packet(const X_INPUT_STATE& state) {
  return static_cast<uint32_t>(state.packet_number);
}

}  // namespace

TEST_CASE("A captured pad reads neutral to the guest", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  driver->SetLeftTrigger(1, 200);
  bool captured = false;
  system->SetGamepadCaptureCallback([&] { return captured; });

  X_INPUT_STATE open = {};
  REQUIRE(system->GetState(0, &open) == X_ERROR_SUCCESS);
  CHECK(Buttons(open) == X_INPUT_GAMEPAD_A);
  CHECK(open.gamepad.left_trigger == 200);

  captured = true;
  X_INPUT_STATE gated = {};
  REQUIRE(system->GetState(0, &gated) == X_ERROR_SUCCESS);
  CHECK(Buttons(gated) == 0);
  CHECK(gated.gamepad.left_trigger == 0);
  // The guest only re-reads a pad whose packet number moved.
  CHECK(Packet(gated) != Packet(open));
}

TEST_CASE("A button held when the capture ends stays released until it is let go",
          "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  bool captured = false;
  system->SetGamepadCaptureCallback([&] { return captured; });

  captured = true;
  driver->SetButtons(1, X_INPUT_GAMEPAD_B);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  captured = false;
  X_INPUT_STATE after = {};
  REQUIRE(system->GetState(0, &after) == X_ERROR_SUCCESS);
  CHECK(Buttons(after) == 0);
  CHECK(Packet(after) != Packet(state));

  driver->SetButtons(1, X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_A);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == X_INPUT_GAMEPAD_A);

  driver->SetButtons(1, 0);
  X_INPUT_STATE released = {};
  REQUIRE(system->GetState(0, &released) == X_ERROR_SUCCESS);
  CHECK(Buttons(released) == 0);
  CHECK(Packet(released) != Packet(state));

  driver->SetButtons(1, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("Capturing the pad leaves keyboard and mouse emulation alone", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1, /*synthetic=*/true);
  driver->Add(2);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  system->SetGamepadCaptureCallback([] { return true; });

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == X_INPUT_GAMEPAD_A);
}

TEST_CASE("Clearing the capture callback returns the pad", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  system->SetGamepadCaptureCallback([] { return true; });
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  system->SetGamepadCaptureCallback(nullptr);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == X_INPUT_GAMEPAD_A);
}

TEST_CASE("Keystrokes are withheld while the pad is captured and for held buttons",
          "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  driver->QueueKeystroke(1, kVkPadA);
  bool captured = false;
  system->SetGamepadCaptureCallback([&] { return captured; });

  X_INPUT_KEYSTROKE ks = {};
  REQUIRE(system->GetKeystroke(0, 0, &ks) == X_ERROR_SUCCESS);

  captured = true;
  CHECK(system->GetKeystroke(0, 0, &ks) == X_ERROR_EMPTY);

  captured = false;
  CHECK(system->GetKeystroke(0, 0, &ks) == X_ERROR_EMPTY);

  driver->QueueKeystroke(1, kVkPadB);
  REQUIRE(system->GetKeystroke(0, 0, &ks) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint16_t>(ks.virtual_key) == kVkPadB);

  driver->SetButtons(1, 0);
  driver->QueueKeystroke(1, kVkPadA);
  REQUIRE(system->GetKeystroke(0, 0, &ks) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint16_t>(ks.virtual_key) == kVkPadA);
}

TEST_CASE("The navigation pad is the first physical pad, captured or not", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  X_INPUT_GAMEPAD pad = {};
  CHECK_FALSE(system->GetNavigationPadState(&pad));

  driver->Add(1, /*synthetic=*/true);
  driver->Add(2);
  driver->Add(3);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  driver->SetButtons(3, X_INPUT_GAMEPAD_A);
  bool captured = true;
  system->SetGamepadCaptureCallback([&] { return captured; });

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->GetNavigationPadState(&pad));
  CHECK(static_cast<uint16_t>(pad.buttons) == X_INPUT_GAMEPAD_B);

  captured = false;
  driver->SetButtons(2, X_INPUT_GAMEPAD_A);
  REQUIRE(system->GetNavigationPadState(&pad));
  CHECK(static_cast<uint16_t>(pad.buttons) == X_INPUT_GAMEPAD_A);

  driver->Remove(2);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  driver->SetButtons(3, X_INPUT_GAMEPAD_B);
  CHECK_FALSE(system->GetNavigationPadState(&pad));
  REQUIRE(system->GetNavigationPadState(&pad));
  CHECK(static_cast<uint16_t>(pad.buttons) == X_INPUT_GAMEPAD_B);

  driver->Remove(3);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK_FALSE(system->GetNavigationPadState(&pad));

  driver->Add(4);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->GetNavigationPadState(&pad));
  system->Shutdown();
  CHECK_FALSE(system->GetNavigationPadState(&pad));
}

TEST_CASE("A driver that gates itself off still has its held buttons remembered",
          "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  bool captured = true;
  system->SetGamepadCaptureCallback([&] { return captured; });

  driver->SetGuestGated(true);
  driver->SetButtons(1, X_INPUT_GAMEPAD_B);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == 0);

  captured = false;
  driver->SetGuestGated(false);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == 0);

  driver->SetButtons(1, 0);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  driver->SetButtons(1, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("A press the guest never polled during the capture stays hidden", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  bool captured = false;
  system->SetGamepadCaptureCallback([&] { return captured; });
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  captured = true;
  driver->SetButtons(1, X_INPUT_GAMEPAD_B);
  X_INPUT_GAMEPAD pad = {};
  REQUIRE(system->GetNavigationPadState(&pad));
  CHECK(static_cast<uint16_t>(pad.buttons) == X_INPUT_GAMEPAD_B);

  captured = false;
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == 0);
  driver->SetButtons(1, 0);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  driver->SetButtons(1, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("The guest sees the gate's change next to a busier keyboard packet", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1, /*synthetic=*/true);
  driver->Add(2);
  driver->SetPacket(1, 1000);
  driver->SetPacket(2, 5);
  driver->SetButtons(2, X_INPUT_GAMEPAD_A);
  bool captured = false;
  system->SetGamepadCaptureCallback([&] { return captured; });

  X_INPUT_STATE open = {};
  REQUIRE(system->GetState(0, &open) == X_ERROR_SUCCESS);
  CHECK(Buttons(open) == X_INPUT_GAMEPAD_A);

  captured = true;
  X_INPUT_STATE gated = {};
  REQUIRE(system->GetState(0, &gated) == X_ERROR_SUCCESS);
  CHECK(Buttons(gated) == 0);
  CHECK(Packet(gated) != Packet(open));
}

TEST_CASE("Navigation moving to another pad reads as a pad that just arrived", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  X_INPUT_GAMEPAD pad = {};
  REQUIRE(system->GetNavigationPadState(&pad));

  driver->Remove(1);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK_FALSE(system->GetNavigationPadState(&pad));
  CHECK(system->GetNavigationPadState(&pad));
}

TEST_CASE("A closing B let go in the middle of a poll stays hidden", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  bool captured = true;
  system->SetGamepadCaptureCallback([&] { return captured; });
  driver->SetButtons(1, X_INPUT_GAMEPAD_B);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  captured = false;
  driver->after_state_read = [&] { driver->SetButtons(1, 0); };
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == 0);
}

TEST_CASE("A change of navigation pad stays pending until the UI reads it", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  X_INPUT_STATE state = {};
  X_INPUT_GAMEPAD pad = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->GetNavigationPadState(&pad));

  driver->Remove(1);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK_FALSE(system->GetNavigationPadState(&pad));
  CHECK(system->GetNavigationPadState(&pad));

  driver->Remove(2);
  CHECK(system->GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  driver->Add(5);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK_FALSE(system->GetNavigationPadState(&pad));
  CHECK(system->GetNavigationPadState(&pad));
}

TEST_CASE("A guest poll's stale sample cannot undo a newer latch", "[input][gamepad]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  bool captured = true;
  system->SetGamepadCaptureCallback([&] { return captured; });
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  // UI latches B after the guest samples neutral but before it acts.
  driver->after_physical_read = [&] {
    driver->after_physical_read = nullptr;
    driver->SetButtons(1, X_INPUT_GAMEPAD_B);
    X_INPUT_GAMEPAD pad = {};
    REQUIRE(system->GetNavigationPadState(&pad));
  };
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  captured = false;
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == 0);
  driver->SetButtons(1, 0);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  driver->SetButtons(1, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(Buttons(state) == X_INPUT_GAMEPAD_B);
}
