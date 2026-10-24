#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include <rex/ui/bar_fill.h>

using rex::ui::BarFillMode;
using rex::ui::BarFillRect;
using rex::ui::CalculateBarFillBlurLevels;
using rex::ui::CalculateBarFillRect;
using rex::ui::HasBars;
using rex::ui::ParseBarFillMode;

TEST_CASE("bar_fill parses the cvar value", "[ui]") {
  CHECK(ParseBarFillMode("blur") == BarFillMode::kBlur);
  CHECK(ParseBarFillMode("BLUR") == BarFillMode::kBlur);
  CHECK(ParseBarFillMode("black") == BarFillMode::kBlack);
  CHECK(ParseBarFillMode("") == BarFillMode::kBlack);
  CHECK(ParseBarFillMode("blurry") == BarFillMode::kBlack);
}

TEST_CASE("bar_fill detects bars", "[ui]") {
  // 16:9 in a 21:9 window: pillarbox.
  CHECK(HasBars(2560, 1080, 320, 0, 1920, 1080));
  // 4:3 content in a 16:9 window.
  CHECK(HasBars(1920, 1080, 240, 0, 1440, 1080));
  // 16:9 in a 16:10 window: letterbox.
  CHECK(HasBars(1920, 1200, 0, 60, 1920, 1080));
  // One-sided remainder.
  CHECK(HasBars(1920, 1080, 0, 0, 1900, 1080));
  CHECK(HasBars(1920, 1080, 0, 0, 1920, 1079));
  // Exact fit and overscan (output larger than the target) have no bars.
  CHECK_FALSE(HasBars(1920, 1080, 0, 0, 1920, 1080));
  CHECK_FALSE(HasBars(1920, 1080, -10, -5, 1940, 1090));
  CHECK_FALSE(HasBars(1920, 1080, 0, -60, 1920, 1200));
  // Cropped along one axis but with a bar along the other.
  CHECK(HasBars(1920, 1080, 100, -30, 1720, 1140));
  // Degenerate input.
  CHECK_FALSE(HasBars(0, 1080, 0, 0, 1920, 1080));
  CHECK_FALSE(HasBars(1920, 1080, 0, 0, 0, 1080));
}

TEST_CASE("bar_fill rect covers the target with the output aspect", "[ui]") {
  // 16:9 output pillarboxed in 2560x1080: widths match, the height overflows.
  BarFillRect rect = CalculateBarFillRect(2560, 1080, 1920, 1080);
  CHECK(rect.width == 2560);
  CHECK(rect.height == 1440);
  CHECK(rect.x == 0);
  CHECK(rect.y == -180);

  // 4:3 output pillarboxed in a 16:9 window.
  rect = CalculateBarFillRect(1920, 1080, 1440, 1080);
  CHECK(rect.width == 1920);
  CHECK(rect.height == 1440);
  CHECK(rect.x == 0);
  CHECK(rect.y == -180);

  // 1920x810 output letterboxed in a 16:10 window: the fill width overflows.
  rect = CalculateBarFillRect(1920, 1200, 1920, 810);
  CHECK(rect.height == 1200);
  CHECK(rect.width == 2845);
  CHECK(rect.x == -462);
  CHECK(rect.y == 0);

  // Same aspect: exactly the target.
  rect = CalculateBarFillRect(1280, 720, 1920, 1080);
  CHECK(rect.width == 1280);
  CHECK(rect.height == 720);
  CHECK(rect.x == 0);
  CHECK(rect.y == 0);

  // Degenerate input gives an empty rectangle.
  rect = CalculateBarFillRect(0, 720, 1920, 1080);
  CHECK(rect.width == 0);
  CHECK(rect.height == 0);
  rect = CalculateBarFillRect(1280, 720, 0, 1080);
  CHECK(rect.width == 0);
  CHECK(rect.height == 0);
}

TEST_CASE("bar_fill rect always covers the target and keeps the aspect", "[ui]") {
  const uint32_t hosts[][2] = {{1, 1},       {3, 7},       {800, 600},   {1280, 720},
                               {1921, 1081}, {2560, 1080}, {3440, 1440}, {3840, 1080},
                               {1080, 1920}, {5120, 1440}, {8192, 8192}};
  const uint32_t outputs[][2] = {{1, 1}, {640, 480}, {1920, 1080}, {1279, 719}, {1440, 1080}};
  for (const auto& host : hosts) {
    for (const auto& output : outputs) {
      INFO(host[0] << "x" << host[1] << " output " << output[0] << "x" << output[1]);
      const BarFillRect rect = CalculateBarFillRect(host[0], host[1], output[0], output[1]);
      REQUIRE(rect.width >= host[0]);
      REQUIRE(rect.height >= host[1]);
      CHECK(rect.x <= 0);
      CHECK(rect.y <= 0);
      CHECK(int64_t(rect.x) + rect.width >= int64_t(host[0]));
      CHECK(int64_t(rect.y) + rect.height >= int64_t(host[1]));
      // Centered within a pixel.
      CHECK(std::abs(int64_t(rect.x) * 2 + int64_t(rect.width) - int64_t(host[0])) <= 1);
      CHECK(std::abs(int64_t(rect.y) * 2 + int64_t(rect.height) - int64_t(host[1])) <= 1);
      // Aspect preserved to within one pixel of rounding on the derived axis.
      const double expected_ratio = double(output[0]) / double(output[1]);
      const double ratio = double(rect.width) / double(rect.height);
      CHECK(std::abs(ratio - expected_ratio) <=
            std::max(1.0, expected_ratio) / double(rect.height) + 1e-9);
    }
  }
}

TEST_CASE("bar_fill blur levels halve down to the target size", "[ui]") {
  std::array<std::pair<uint32_t, uint32_t>, rex::ui::kBarFillBlurMaxLevels> sizes;

  size_t count = CalculateBarFillBlurLevels(1280, 720, sizes.data(), sizes.size());
  REQUIRE(count == 4);
  CHECK(sizes[0] == std::make_pair(640u, 360u));
  CHECK(sizes[1] == std::make_pair(320u, 180u));
  CHECK(sizes[2] == std::make_pair(160u, 90u));
  CHECK(sizes[3] == std::make_pair(80u, 45u));

  count = CalculateBarFillBlurLevels(3840, 2160, sizes.data(), sizes.size());
  REQUIRE(count == 6);
  CHECK(sizes[5] == std::make_pair(60u, 34u));

  // Odd sizes round up.
  count = CalculateBarFillBlurLevels(641, 361, sizes.data(), sizes.size());
  REQUIRE(count >= 1);
  CHECK(sizes[0] == std::make_pair(321u, 181u));

  // A source already small enough still gets one pass.
  count = CalculateBarFillBlurLevels(64, 36, sizes.data(), sizes.size());
  REQUIRE(count == 1);
  CHECK(sizes[0] == std::make_pair(32u, 18u));

  // Tiny sources never reach zero.
  count = CalculateBarFillBlurLevels(1, 1, sizes.data(), sizes.size());
  REQUIRE(count == 1);
  CHECK(sizes[0] == std::make_pair(1u, 1u));

  // The count is capped.
  count = CalculateBarFillBlurLevels(16384, 16384, sizes.data(), 3);
  CHECK(count == 3);
  count = CalculateBarFillBlurLevels(8192, 8192, sizes.data(), sizes.size());
  CHECK(count <= rex::ui::kBarFillBlurMaxLevels);
  CHECK(std::max(sizes[count - 1].first, sizes[count - 1].second) <=
        rex::ui::kBarFillBlurTargetSize);
}
