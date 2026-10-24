#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/system/xvideo.h>

namespace {

class VideoModeCvarReset {
 public:
  ~VideoModeCvarReset() {
    for (const char* name :
         {"window_width", "window_height", "video_mode_width", "video_mode_height", "resolution"}) {
      rex::cvar::ResetToDefault(name);
    }
  }
};

rex::system::X_VIDEO_MODE QueryMode() {
  rex::system::X_VIDEO_MODE mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  return mode;
}

}  // namespace

TEST_CASE("host window size does not change the guest video mode", "[kernel][video]") {
  VideoModeCvarReset reset;
  REQUIRE(rex::cvar::SetFlagByName("window_width", "3440"));
  REQUIRE(rex::cvar::SetFlagByName("window_height", "1440"));

  const auto mode = QueryMode();
  CHECK(mode.display_width == 1280);
  CHECK(mode.display_height == 720);
}

TEST_CASE("guest video mode follows video_mode and the resolution preset", "[kernel][video]") {
  VideoModeCvarReset reset;
  REQUIRE(rex::cvar::SetFlagByName("window_width", "3440"));
  REQUIRE(rex::cvar::SetFlagByName("window_height", "1440"));

  REQUIRE(rex::cvar::SetFlagByName("resolution", "1080p"));
  CHECK(QueryMode().display_width == 1920);
  CHECK(QueryMode().display_height == 1080);

  REQUIRE(rex::cvar::SetFlagByName("video_mode_width", "1600"));
  REQUIRE(rex::cvar::SetFlagByName("video_mode_height", "900"));
  CHECK(QueryMode().display_width == 1600);
  CHECK(QueryMode().display_height == 900);
}
