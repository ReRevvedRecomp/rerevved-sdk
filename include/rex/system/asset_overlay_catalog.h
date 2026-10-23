/**
 * @file        system/asset_overlay_catalog.h
 * @brief       Installed asset-overlay package catalog
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rex::system {

inline constexpr uint32_t kAssetOverlayManifestVersion = 1;

bool IsValidAssetOverlayPackageId(std::string_view id);

struct AssetOverlayDiagnostic {
  bool blocking = true;
  std::string message;
  std::filesystem::path path;
};

struct AssetOverlayPackage {
  std::filesystem::path package_root;
  std::string folder_name;
  uint32_t manifest_version = 0;
  std::string id;
  std::string display_name;
  std::string version;
  std::string author;
  std::string description;
  std::filesystem::path assets_root;
  std::vector<AssetOverlayDiagnostic> diagnostics;
};

struct AssetOverlayCatalog {
  std::filesystem::path overlays_root;
  std::vector<AssetOverlayPackage> packages;
  std::vector<AssetOverlayDiagnostic> diagnostics;

  const AssetOverlayPackage* Find(std::string_view id) const;
};

// Discovers direct-child asset packs without loading or modifying package files.
AssetOverlayCatalog DiscoverAssetOverlayCatalog(const std::filesystem::path& overlays_root);

}  // namespace rex::system
