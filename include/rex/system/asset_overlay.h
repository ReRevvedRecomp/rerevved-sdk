/**
 * @file        system/asset_overlay.h
 * @brief       Ordered asset-overlay file resolution
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/result.h>
#include <rex/system/asset_overlay_catalog.h>

namespace rex::system {

struct AssetOverlayResolution {
  std::string package_id;
  std::filesystem::path asset_path;
  std::vector<uint8_t> bytes;
  std::vector<std::string> shadowed_package_ids;
};

// The logical key is relative to the package assets/ directory. The first
// existing regular file in the selected top-to-bottom order wins.
rex::Result<AssetOverlayResolution> ResolveAssetOverlay(
    std::span<const AssetOverlayPackage> selected_packages, std::string_view logical_key,
    size_t max_bytes);

}  // namespace rex::system
