/**
 * @file        rex/ui/overlay/achievement_icon_cache.h
 * @brief       Shared achievement icon texture cache for built-in overlays.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/system/achievement_store.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImmediateDrawer;
class ImmediateTexture;

// Tightly packed R8G8B8A8 pixels. Empty or inconsistent data means no icon.
struct AchievementIconPixels {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

using AchievementIconProvider =
    std::function<AchievementIconPixels(const rex::system::AchievementInfo&)>;

class AchievementIconCache {
 public:
  AchievementIconCache(ImmediateDrawer* immediate_drawer, rex::Runtime* runtime);

  // Registers the process-wide provider that every cache consults first. Pass
  // an empty function to remove it. Providers run on the UI thread, once per
  // cache and icon, and should cache their own decoded results.
  static void SetProvider(AchievementIconProvider provider);

  // Loads the achievement icon in priority order:
  // 1. provider pixels
  // 2. explicit metadata icon_path
  // 3. embedded metadata icon_path
  // 4. metadata icons/<image_id>.png
  // 5. embedded metadata icons/<image_id>.png
  // 6. title XDBF image <image_id> embedded in the loaded XEX
  ImmediateTexture* GetIcon(const rex::system::AchievementInfo& achievement);

 private:
  std::unique_ptr<ImmediateTexture> CreateTextureFromBytes(const uint8_t* data, size_t size);
  std::unique_ptr<ImmediateTexture> LoadMetadataTexture(const std::filesystem::path& relative_path);
  std::unique_ptr<ImmediateTexture> LoadXdbfTexture(uint32_t image_id);

  ImmediateDrawer* immediate_drawer_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<ImmediateTexture>> cache_;
};

}  // namespace rex::ui
