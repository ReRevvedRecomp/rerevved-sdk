#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/swap_presentation.h>

namespace rex::graphics {
namespace {

system::SwapPresentationAspect Request(uint32_t x, uint32_t y, uint64_t sequence) {
  system::SwapPresentationAspect request;
  request.aspect_x = x;
  request.aspect_y = y;
  request.policy_sequence = sequence;
  request.override_valid = x != 0 && y != 0;
  return request;
}

constexpr uint32_t kNativeX = 1280;
constexpr uint32_t kNativeY = 720;

class BareGraphicsSystem final : public GraphicsSystem {
 public:
  std::string name() const override { return "bare"; }

 protected:
  void CreateProvider(bool) override {}
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override { return {}; }
};

}  // namespace

TEST_CASE("new content uses the requested aspect or defaults to native",
          "[graphics][swap]") {
  SwapAspectTracker tracker;
  const SwapContentKey first{1, 1};
  const SwapContentKey second{1, 2};

  auto native = tracker.Decide(Request(0, 0, 0), &first, kNativeX, kNativeY);
  CHECK(native.display_x == kNativeX);
  CHECK(native.display_y == kNativeY);
  CHECK_FALSE(native.repeated);
  CHECK(native.content_id == 1);
  tracker.Commit(native);

  auto wide = tracker.Decide(Request(7, 3, 1), &second, kNativeX, kNativeY);
  CHECK(wide.display_x == 7);
  CHECK(wide.display_y == 3);
  CHECK_FALSE(wide.repeated);
  CHECK(wide.content_id == 2);
}

TEST_CASE("an explicit clear presents new content at the native aspect", "[graphics][swap]") {
  SwapAspectTracker tracker;
  const SwapContentKey content{1, 1};
  const auto decision = tracker.Decide(Request(0, 0, 4), &content, kNativeX, kNativeY);
  CHECK(decision.display_x == kNativeX);
  CHECK(decision.display_y == kNativeY);
}

TEST_CASE("repeated content keeps the aspect it was first shown with", "[graphics][swap]") {
  SwapAspectTracker tracker;
  const SwapContentKey content{3, 9};

  auto wide = tracker.Decide(Request(7, 3, 1), &content, kNativeX, kNativeY);
  tracker.Commit(wide);

  const auto repeat_native = tracker.Decide(Request(0, 0, 2), &content, kNativeX, kNativeY);
  CHECK(repeat_native.repeated);
  CHECK(repeat_native.content_id == wide.content_id);
  CHECK(repeat_native.display_x == 7);
  CHECK(repeat_native.display_y == 3);

  const auto repeat_other = tracker.Decide(Request(32, 9, 3), &content, kNativeX, kNativeY);
  CHECK(repeat_other.repeated);
  CHECK(repeat_other.display_x == 7);
  CHECK(repeat_other.display_y == 3);
}

TEST_CASE("rewritten content at the same address is new content", "[graphics][swap]") {
  SwapAspectTracker tracker;
  const SwapContentKey original{3, 9};
  const SwapContentKey rewritten_key{3, 10};
  const SwapContentKey recreated_key{4, 9};
  tracker.Commit(tracker.Decide(Request(7, 3, 1), &original, kNativeX, kNativeY));

  const auto rewritten = tracker.Decide(Request(0, 0, 2), &rewritten_key, kNativeX, kNativeY);
  CHECK_FALSE(rewritten.repeated);
  CHECK(rewritten.content_id == 2);
  CHECK(rewritten.display_x == kNativeX);

  const auto recreated = tracker.Decide(Request(0, 0, 2), &recreated_key, kNativeX, kNativeY);
  CHECK_FALSE(recreated.repeated);
}

TEST_CASE("a failed refresh preserves the shown image's aspect",
          "[graphics][swap]") {
  SwapAspectTracker tracker;
  const SwapContentKey shown{1, 1};
  const SwapContentKey failed{1, 2};
  tracker.Commit(tracker.Decide(Request(7, 3, 1), &shown, kNativeX, kNativeY));

  (void)tracker.Decide(Request(0, 0, 2), &failed, kNativeX, kNativeY);  // Not committed.

  const auto again = tracker.Decide(Request(0, 0, 3), &shown, kNativeX, kNativeY);
  CHECK(again.repeated);
  CHECK(again.display_x == 7);
  CHECK(again.display_y == 3);
}

TEST_CASE("unidentified content is never treated as a repeat", "[graphics][swap]") {
  SwapAspectTracker tracker;
  const SwapContentKey content{1, 1};
  tracker.Commit(tracker.Decide(Request(7, 3, 1), &content, kNativeX, kNativeY));

  const auto unknown = tracker.Decide(Request(0, 0, 2), nullptr, kNativeX, kNativeY);
  CHECK_FALSE(unknown.content_known);
  CHECK_FALSE(unknown.repeated);
  CHECK(unknown.display_x == kNativeX);
  tracker.Commit(unknown);

  const auto after = tracker.Decide(Request(0, 0, 3), &content, kNativeX, kNativeY);
  CHECK_FALSE(after.repeated);
  CHECK(after.display_x == kNativeX);
}

TEST_CASE("surface size is published as one coherent pair", "[graphics][swap]") {
  BareGraphicsSystem system;
  uint32_t width = 7;
  uint32_t height = 7;
  CHECK_FALSE(system.GetDisplaySurfaceSize(width, height));
  CHECK(width == 0);
  CHECK(height == 0);

  system.SetDisplaySurfaceSize(3440, 1440);
  CHECK(system.GetDisplaySurfaceSize(width, height));
  CHECK(width == 3440);
  CHECK(height == 1440);
  CHECK(system.display_surface_width() == 3440);
  CHECK(system.display_surface_height() == 1440);

  system.SetDisplaySurfaceSize(0, 1440);
  CHECK_FALSE(system.GetDisplaySurfaceSize(width, height));
  CHECK(width == 0);
  CHECK(height == 0);
}

TEST_CASE("presentation aspect requests are refused without a supporting backend",
          "[graphics][swap]") {
  BareGraphicsSystem system;
  CHECK_FALSE(system.SetPresentationAspect(7, 3));
  CHECK_FALSE(system.SetPresentationAspect(0, 0));
  CHECK_FALSE(system.SetPresentationAspect(7, 0));
  CHECK_FALSE(system.SetPresentationAspect(0, 3));

  system::SwapPresentationAspect pending;
  CHECK_FALSE(system.TakePresentationAspect(pending));
  CHECK(pending.policy_sequence == 0);
}

}  // namespace rex::graphics
