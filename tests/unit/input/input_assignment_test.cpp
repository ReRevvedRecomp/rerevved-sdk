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
using rex::input::InputDriver;
using rex::input::InputSystem;
using rex::input::SharedAssignment;
using rex::input::SlotAssignment;
using rex::input::X_INPUT_CAPABILITIES;
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

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void Add(uint64_t id, bool synthetic = false) {
    DeviceInfo info;
    info.id = static_cast<DeviceId>(id);
    info.synthetic = synthetic;
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
