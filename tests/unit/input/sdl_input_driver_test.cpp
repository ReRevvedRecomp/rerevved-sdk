#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include <rex/input/sdl/sdl_input_driver.h>

namespace rex::input::sdl {
struct SDLInputDriverTestAccess {
  static void Init(SDLInputDriver& driver) { driver.InitializeSubsystems(); }
  static void Teardown(SDLInputDriver& driver) { driver.Teardown(); }
  static bool WatchAdded(const SDLInputDriver& driver) { return driver.event_watch_added_; }
  static bool EventsInit(const SDLInputDriver& driver) { return driver.sdl_events_initialized_; }
  static bool GamepadInit(const SDLInputDriver& driver) { return driver.SDL_Gamepad_initialized_; }
  static std::unique_lock<std::mutex> LockControllers(SDLInputDriver& driver) {
    return std::unique_lock<std::mutex>(driver.controllers_mutex_);
  }
  static std::unique_lock<std::mutex> Drain(SDLInputDriver& driver) {
    return driver.DrainAndLock();
  }
  static void Push(SDLInputDriver& driver, const SDL_Event& event) { driver.HandleEvent(event); }
  static size_t Pending(SDLInputDriver& driver) {
    std::lock_guard<std::mutex> guard(driver.event_queue_mutex_);
    return driver.pending_events_.size();
  }
  static int WatchRemovals() { return SDLInputDriver::WatchRemovalCount(); }
  static int SubsystemQuits() { return SDLInputDriver::SubsystemQuitCount(); }
};
}  // namespace rex::input::sdl

namespace {
using rex::input::sdl::SDLInputDriver;
using Access = rex::input::sdl::SDLInputDriverTestAccess;
}  // namespace

// Release the SDL watch and subsystems exactly once, with or without OnClosing
// (upstream issue 525). The test executable has its own SDL copy, so observe
// the driver's state through its flags and teardown counters.
TEST_CASE("SDL input driver destruction without OnClosing releases SDL once", "[input][sdl]") {
  const int removals = Access::WatchRemovals();
  const int quits = Access::SubsystemQuits();
  int expected_quits = 0;
  {
    SDLInputDriver driver(nullptr, 0);
    Access::Init(driver);
    REQUIRE(Access::EventsInit(driver));
    REQUIRE(Access::WatchAdded(driver));
    expected_quits = 1 + (Access::GamepadInit(driver) ? 1 : 0);
  }
  CHECK(Access::WatchRemovals() == removals + 1);
  CHECK(Access::SubsystemQuits() == quits + expected_quits);
}

TEST_CASE("SDL input driver teardown is idempotent across repeats and the destructor",
          "[input][sdl]") {
  const int removals = Access::WatchRemovals();
  const int quits = Access::SubsystemQuits();
  int expected_quits = 0;
  {
    SDLInputDriver driver(nullptr, 0);
    Access::Init(driver);
    REQUIRE(Access::EventsInit(driver));
    expected_quits = 1 + (Access::GamepadInit(driver) ? 1 : 0);
    Access::Teardown(driver);
    CHECK_FALSE(Access::WatchAdded(driver));
    CHECK_FALSE(Access::EventsInit(driver));
    CHECK_FALSE(Access::GamepadInit(driver));
    Access::Teardown(driver);
  }
  CHECK(Access::WatchRemovals() == removals + 1);
  CHECK(Access::SubsystemQuits() == quits + expected_quits);
}

TEST_CASE("SDL input driver that never initialized tears down nothing", "[input][sdl]") {
  const int removals = Access::WatchRemovals();
  const int quits = Access::SubsystemQuits();
  { SDLInputDriver driver(nullptr, 0); }
  CHECK(Access::WatchRemovals() == removals);
  CHECK(Access::SubsystemQuits() == quits);
}

TEST_CASE("SDL gamepad types map to glyph families", "[input][sdl]") {
  using rex::input::GamepadFamily;
  using rex::input::sdl::GamepadFamilyForType;
  CHECK(GamepadFamilyForType(SDL_GAMEPAD_TYPE_XBOX360) == GamepadFamily::kXbox);
  CHECK(GamepadFamilyForType(SDL_GAMEPAD_TYPE_XBOXONE) == GamepadFamily::kXbox);
  CHECK(GamepadFamilyForType(SDL_GAMEPAD_TYPE_PS3) == GamepadFamily::kPlayStation);
  CHECK(GamepadFamilyForType(SDL_GAMEPAD_TYPE_PS4) == GamepadFamily::kPlayStation);
  CHECK(GamepadFamilyForType(SDL_GAMEPAD_TYPE_PS5) == GamepadFamily::kPlayStation);
  CHECK(GamepadFamilyForType(SDL_GAMEPAD_TYPE_UNKNOWN) == GamepadFamily::kOther);
  CHECK(GamepadFamilyForType(SDL_GAMEPAD_TYPE_STANDARD) == GamepadFamily::kOther);
  CHECK(GamepadFamilyForType(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO) == GamepadFamily::kOther);
}

TEST_CASE("SDL drain takes the controllers lock before it takes the queued events",
          "[input][sdl]") {
  SDLInputDriver driver(nullptr, 0);
  SDL_Event event = {};
  event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;

  auto held = Access::LockControllers(driver);
  Access::Push(driver, event);
  std::atomic<bool> started = false;
  std::thread drainer([&] {
    started = true;
    auto guard = Access::Drain(driver);
  });
  while (!started) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  Access::Push(driver, event);
  held.unlock();
  drainer.join();
  CHECK(Access::Pending(driver) == 0);
}
