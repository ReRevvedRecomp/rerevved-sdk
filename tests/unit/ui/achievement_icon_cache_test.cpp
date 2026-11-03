/**
 * @file        achievement_icon_cache_test.cpp
 * @brief       Achievement icon provider hook
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

namespace {

class TestTexture final : public rex::ui::ImmediateTexture {
 public:
  TestTexture(uint32_t width, uint32_t height) : ImmediateTexture(width, height) {}
};

class TestDrawer final : public rex::ui::ImmediateDrawer {
 public:
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                           rex::ui::ImmediateTextureFilter, bool,
                                                           const uint8_t* data) override {
    first_pixel = data[0];
    ++created;
    return std::make_unique<TestTexture>(width, height);
  }
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch&) override {}
  void Draw(const rex::ui::ImmediateDraw&) override {}
  void EndDrawBatch() override {}

  int created = 0;
  uint8_t first_pixel = 0;
};

struct ProviderGuard {
  ~ProviderGuard() { rex::ui::AchievementIconCache::SetProvider({}); }
};

rex::system::AchievementInfo Make(const char* icon_path) {
  rex::system::AchievementInfo info;
  info.icon_path = icon_path;
  return info;
}

}  // namespace

TEST_CASE("achievement icon provider supplies the texture", "[achievements][ui]") {
  ProviderGuard guard;
  TestDrawer drawer;
  rex::ui::AchievementIconCache cache(&drawer, nullptr);
  int calls = 0;
  rex::ui::AchievementIconCache::SetProvider([&](const rex::system::AchievementInfo& info) {
    ++calls;
    rex::ui::AchievementIconPixels pixels;
    if (info.icon_path == "provided") {
      pixels.width = 2;
      pixels.height = 3;
      pixels.rgba.assign(2 * 3 * 4, 7);
    }
    return pixels;
  });

  auto* texture = cache.GetIcon(Make("provided"));
  REQUIRE(texture != nullptr);
  CHECK(texture->width == 2);
  CHECK(texture->height == 3);
  CHECK(drawer.first_pixel == 7);

  CHECK(cache.GetIcon(Make("provided")) == texture);
  CHECK(calls == 1);
}

TEST_CASE("achievement icon provider results are validated", "[achievements][ui]") {
  ProviderGuard guard;
  TestDrawer drawer;
  rex::ui::AchievementIconCache cache(&drawer, nullptr);
  rex::ui::AchievementIconCache::SetProvider([](const rex::system::AchievementInfo& info) {
    rex::ui::AchievementIconPixels pixels;
    if (info.icon_path == "short") {
      pixels.width = 2;
      pixels.height = 2;
      pixels.rgba.assign(3, 0);
    }
    return pixels;
  });

  CHECK(cache.GetIcon(Make("short")) == nullptr);
  CHECK(cache.GetIcon(Make("empty")) == nullptr);
  CHECK(drawer.created == 0);
}

TEST_CASE("achievement icon provider can be removed", "[achievements][ui]") {
  ProviderGuard guard;
  TestDrawer drawer;
  rex::ui::AchievementIconCache::SetProvider([](const rex::system::AchievementInfo&) {
    rex::ui::AchievementIconPixels pixels;
    pixels.width = 1;
    pixels.height = 1;
    pixels.rgba.assign(4, 0);
    return pixels;
  });
  rex::ui::AchievementIconCache::SetProvider({});

  rex::ui::AchievementIconCache cache(&drawer, nullptr);
  CHECK(cache.GetIcon(Make("gone")) == nullptr);
  CHECK(drawer.created == 0);
}
