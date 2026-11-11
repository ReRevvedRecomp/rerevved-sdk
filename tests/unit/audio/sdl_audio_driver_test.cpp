#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>

#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/thread.h>

#include "../test_memory.h"

namespace {
struct ProbeDriver : rex::audio::sdl::SDLAudioDriver {
  using SDLAudioDriver::SDLAudioDriver;
  bool silent() const { return silent_pacer_running_; }
};
}  // namespace

// CTest sets SDL_AUDIO_DRIVER to a missing backend at process start; the runtime
// DLL holds its own SDL copy, which an SDL_SetHint call here would not reach.
TEST_CASE("audio driver without an output device still consumes frames", "[.][no-audio-device]") {
  REQUIRE(std::getenv("SDL_AUDIO_DRIVER"));
  auto& memory = rex::testing::GetTestMemory();
  auto semaphore = rex::thread::Semaphore::Create(0, 8);
  REQUIRE(semaphore);

  ProbeDriver driver(&memory, semaphore.get());
  CHECK(driver.Initialize());
  CHECK(driver.silent());

  const uint32_t frame = memory.SystemHeapAlloc(6 * 256 * sizeof(float));
  REQUIRE(frame);
  driver.SubmitFrame(frame);
  driver.SubmitFrame(frame);
  for (int i = 0; i < 2; ++i) {
    CHECK(rex::thread::Wait(semaphore.get(), false, std::chrono::milliseconds(1000)) ==
          rex::thread::WaitResult::kSuccess);
  }

  driver.Shutdown();
  memory.SystemHeapFree(frame);
}
