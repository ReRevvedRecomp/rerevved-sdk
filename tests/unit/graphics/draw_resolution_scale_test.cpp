#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/cache.h>

namespace {
using namespace rex::graphics;

constexpr uint32_t kMax = TextureCache::kMaxDrawResolutionScaleAlongAxis;

uint32_t Auto(uint32_t surface_width, uint32_t surface_height, uint32_t max_scale = kMax) {
  return AutoDrawResolutionScale(surface_width, surface_height, 1280, 720, max_scale);
}

class ScaleCvarReset {
 public:
  ~ScaleCvarReset() {
    rex::cvar::ResetToDefault("resolution_scale");
    rex::cvar::ResetToDefault("draw_resolution_scale_x");
    rex::cvar::ResetToDefault("draw_resolution_scale_y");
  }
};
}  // namespace

TEST_CASE("auto draw resolution scale matches the presented image height", "[graphics]") {
  CHECK(Auto(1280, 720) == 1);
  CHECK(Auto(1920, 1080) == 2);
  CHECK(Auto(2560, 1440) == 2);
  CHECK(Auto(3440, 1440) == 2);
  CHECK(Auto(5120, 1440) == 2);
  CHECK(Auto(3840, 2160) == 3);
  CHECK(Auto(7680, 2160) == 3);
  CHECK(Auto(1280, 1024) == 1);
  CHECK(Auto(2560, 1600) == 2);
  CHECK(Auto(0, 0) == 1);
  CHECK(Auto(3840, 2160, 2) == 2);
  // Fractional scale ratios round up; the tighter axis limits the fitted image.
  CHECK(Auto(1281, 721) == 2);
  CHECK(Auto(1280, 721) == 1);
}

TEST_CASE("auto draw resolution scale tolerates unknown guest sizes and limits", "[graphics]") {
  CHECK(AutoDrawResolutionScale(3840, 2160, 0, 720, kMax) == 1);
  CHECK(AutoDrawResolutionScale(3840, 2160, 1280, 0, kMax) == 1);
  CHECK(AutoDrawResolutionScale(3840, 2160, 1280, 720, 0) == 1);
  CHECK(AutoDrawResolutionScale(8192, 8192, 1280, 720, kMax) == kMax);
}

TEST_CASE("resolution_scale 0 resolves from the display geometry", "[graphics]") {
  ScaleCvarReset reset;
  const DisplayGeometry uhd{1280, 720, 3840, 2160};
  uint32_t x = 0;
  uint32_t y = 0;
  DrawResolutionScaleRequest request;

  // The SDK default uses the per-axis settings.
  REQUIRE(TextureCache::GetConfigDrawResolutionScale(x, y, uhd, &request));
  CHECK(x == 1);
  CHECK(y == 1);
  CHECK_FALSE(request.auto_x);

  REQUIRE(rex::cvar::SetFlagByName("resolution_scale", "0"));
  REQUIRE(TextureCache::GetConfigDrawResolutionScale(x, y, uhd, &request));
  CHECK(x == 3);
  CHECK(y == 3);
  CHECK(request.auto_x);
  CHECK(request.auto_y);

  // An unknown surface is not scaled.
  REQUIRE(TextureCache::GetConfigDrawResolutionScale(x, y, {}, &request));
  CHECK(x == 1);
  CHECK(y == 1);

  // A non-default per-axis scale still overrides its axis.
  REQUIRE(rex::cvar::SetFlagByName("draw_resolution_scale_y", "2"));
  REQUIRE(TextureCache::GetConfigDrawResolutionScale(x, y, uhd, &request));
  CHECK(x == 3);
  CHECK(y == 2);
  CHECK(request.auto_x);
  CHECK_FALSE(request.auto_y);
}

TEST_CASE("resolution_scale 0 stays auto as an application default", "[graphics]") {
  ScaleCvarReset reset;
  REQUIRE(rex::cvar::SetFlagAsApplicationDefault("resolution_scale", "0"));
  uint32_t x = 0;
  uint32_t y = 0;
  const bool not_clamped =
      TextureCache::GetConfigDrawResolutionScale(x, y, {1280, 720, 1920, 1080});
  // Put the SDK default back for the other cases.
  REQUIRE(rex::cvar::SetFlagAsApplicationDefault("resolution_scale", "1"));
  CHECK(not_clamped);
  CHECK(x == 2);
  CHECK(y == 2);
}
