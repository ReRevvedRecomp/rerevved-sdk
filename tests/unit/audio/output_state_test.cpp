#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/audio/downmix.h>
#include <rex/audio/output_state.h>

using namespace rex::audio;

TEST_CASE("player volume scales the master gain", "[audio]") {
  CHECK(ComposeOutputGain(1.0f, 100) == Catch::Approx(1.0f));
  CHECK(ComposeOutputGain(1.0f, 50) == Catch::Approx(0.5f));
  CHECK(ComposeOutputGain(1.0f, 0) == 0.0f);
  // A title or mod gain composes with the player volume instead of being
  // replaced by it.
  CHECK(ComposeOutputGain(0.5f, 50) == Catch::Approx(0.25f));
  CHECK(ComposeOutputGain(2.0f, 25) == Catch::Approx(0.5f));
  CHECK(ComposeOutputGain(0.0f, 100) == 0.0f);
}

TEST_CASE("player volume outside 0 to 100 is clamped", "[audio]") {
  CHECK(ComposeOutputGain(1.0f, -10) == 0.0f);
  CHECK(ComposeOutputGain(1.0f, 250) == Catch::Approx(1.0f));
}

TEST_CASE("output gain getter feeds the composition", "[audio]") {
  const float previous = GetOutputGain();
  SetOutputGain(0.5f);
  CHECK(ComposeOutputGain(GetOutputGain(), 50) == Catch::Approx(0.25f));
  SetOutputGain(previous);
}

TEST_CASE("silence follows mute and background play", "[audio]") {
  // muted, play_in_background, window_focused
  CHECK_FALSE(ShouldSilenceOutput(false, true, true));
  CHECK_FALSE(ShouldSilenceOutput(false, true, false));
  CHECK_FALSE(ShouldSilenceOutput(false, false, true));
  CHECK(ShouldSilenceOutput(false, false, false));
  CHECK(ShouldSilenceOutput(true, true, true));
  CHECK(ShouldSilenceOutput(true, false, false));
}

TEST_CASE("output focus defaults to focused and tracks updates", "[audio]") {
  CHECK(IsOutputFocused());
  SetOutputFocused(false);
  CHECK_FALSE(IsOutputFocused());
  SetOutputFocused(true);
  CHECK(IsOutputFocused());
}
