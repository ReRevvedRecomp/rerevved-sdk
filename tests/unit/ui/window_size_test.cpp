#include <catch2/catch_test_macros.hpp>

#include <rex/ui/window_size.h>

using rex::ui::EffectiveWindowSize;
using rex::ui::FitWindowSize;
using rex::ui::ParseWindowSize;
using rex::ui::WindowSize;
using rex::ui::WindowSizeKind;

TEST_CASE("window_size parses fixed sizes and maximized", "[ui]") {
  CHECK(ParseWindowSize("").kind == WindowSizeKind::kUnset);

  const WindowSize fixed = ParseWindowSize("1920x1080");
  CHECK(fixed.kind == WindowSizeKind::kFixed);
  CHECK(fixed.width == 1920);
  CHECK(fixed.height == 1080);

  CHECK(ParseWindowSize("2560X1440").width == 2560);
  CHECK(ParseWindowSize("maximized").kind == WindowSizeKind::kMaximized);
  CHECK(ParseWindowSize("Maximized").kind == WindowSizeKind::kMaximized);
}

TEST_CASE("window_size rejects malformed values", "[ui]") {
  for (const char* bad : {"x", "1920x", "x1080", "1920", "0x720", "1280x0", "-1280x720",
                          "1280x720x2", "1280 x720", "9000x720", "1280x9000", "max", "1080p"}) {
    INFO(bad);
    CHECK(ParseWindowSize(bad).kind == WindowSizeKind::kUnset);
  }
}

TEST_CASE("explicit window_width or window_height outranks window_size", "[ui]") {
  CHECK(EffectiveWindowSize("1920x1080", 0, 0).kind == WindowSizeKind::kFixed);
  CHECK(EffectiveWindowSize("maximized", 0, 0).kind == WindowSizeKind::kMaximized);
  CHECK(EffectiveWindowSize("1920x1080", 1000, 0).kind == WindowSizeKind::kUnset);
  CHECK(EffectiveWindowSize("1920x1080", 0, 600).kind == WindowSizeKind::kUnset);
  CHECK(EffectiveWindowSize("maximized", 1000, 600).kind == WindowSizeKind::kUnset);
  CHECK(EffectiveWindowSize("", 0, 0).kind == WindowSizeKind::kUnset);
}

TEST_CASE("window_size is shrunk to the display and keeps its aspect", "[ui]") {
  const WindowSize uhd{WindowSizeKind::kFixed, 3840, 2160};

  const WindowSize same = FitWindowSize(uhd, 3840, 2160);
  CHECK(same.width == 3840);
  CHECK(same.height == 2160);

  const WindowSize roomy = FitWindowSize(uhd, 5000, 3000);
  CHECK(roomy.width == 3840);
  CHECK(roomy.height == 2160);

  // Width is the tighter bound.
  const WindowSize fhd = FitWindowSize(uhd, 1920, 1200);
  CHECK(fhd.width == 1920);
  CHECK(fhd.height == 1080);

  // Height is the tighter bound.
  const WindowSize short_display = FitWindowSize(uhd, 3840, 1080);
  CHECK(short_display.width == 1920);
  CHECK(short_display.height == 1080);

  // Unknown bounds and non-fixed sizes pass through.
  CHECK(FitWindowSize(uhd, 0, 0).width == 3840);
  CHECK(FitWindowSize({WindowSizeKind::kMaximized, 0, 0}, 100, 100).kind ==
        WindowSizeKind::kMaximized);

  const WindowSize tiny = FitWindowSize({WindowSizeKind::kFixed, 8000, 10}, 100, 100);
  CHECK(tiny.width == 100);
  CHECK(tiny.height == 1);
}
