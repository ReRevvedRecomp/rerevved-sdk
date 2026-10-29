#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <rex/input/sdl/sdl_input_driver.h>

namespace rex::input::sdl {
struct SDLInputDriverTestAccess {
  static void Init(SDLInputDriver& driver) { driver.InitializeSubsystems(); }
  static void Teardown(SDLInputDriver& driver) { driver.Teardown(); }
  static bool WatchAdded(const SDLInputDriver& driver) { return driver.event_watch_added_; }
  static bool EventsInit(const SDLInputDriver& driver) { return driver.sdl_events_initialized_; }
  static bool GamepadInit(const SDLInputDriver& driver) { return driver.SDL_Gamepad_initialized_; }
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