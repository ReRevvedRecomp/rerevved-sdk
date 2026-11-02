/**
 * @file        system/asset_overlay_loadout.h
 * @brief       Ordered asset-overlay loadout and bundled default
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/system/asset_overlay_catalog.h>

namespace rex::system {

inline constexpr std::string_view kAssetOverlayOrderFileName = "asset_order.txt";
inline constexpr std::string_view kAssetOverlayDefaultOrderFileName = "default_asset_order.txt";

struct AssetOverlayLoadoutEntry {
  std::string id;
  size_t line = 0;
};

struct AssetOverlayLoadoutDiagnostic {
  size_t line = 0;
  std::string message;
  std::filesystem::path path;
};

struct AssetOverlayLoadoutFile {
  // The profile-local destination. This remains the profile path when a
  // bundled default supplies the entries, so callers can materialize an
  // explicit profile override without losing its destination.
  std::filesystem::path path;
  // The file that supplied the parsed entries, when one was read.
  std::filesystem::path source_path;
  // True only when the profile-local asset_order.txt exists. A bundled
  // default deliberately leaves this false so the UI can offer Save.
  bool exists = false;
  bool uses_bundled_default = false;
  std::vector<AssetOverlayLoadoutEntry> entries;
  std::vector<AssetOverlayLoadoutDiagnostic> diagnostics;
};

struct AssetOverlayLoadoutSelection {
  std::vector<std::string> requested_ids;
  std::vector<AssetOverlayPackage> packages;
  std::vector<AssetOverlayLoadoutDiagnostic> diagnostics;

  bool IsValid() const { return diagnostics.empty(); }
};

enum class AssetOverlayLoadoutApplyStatus {
  kSuccess,
  kInvalidDesired,
  kInvalidCurrent,
  kInvalidProfile,
  kIoError,
};

struct AssetOverlayLoadoutApplyResult {
  AssetOverlayLoadoutApplyStatus status = AssetOverlayLoadoutApplyStatus::kIoError;
  std::vector<AssetOverlayLoadoutDiagnostic> diagnostics;

  bool succeeded() const { return status == AssetOverlayLoadoutApplyStatus::kSuccess; }
};

AssetOverlayLoadoutFile ReadAssetOverlayLoadout(const std::filesystem::path& profile_root);
// Falls back to <bundled_root>/default_<file_name>. A present profile file, including an
// explicitly empty file, always takes precedence over the bundled default.
AssetOverlayLoadoutFile ReadAssetOverlayLoadout(
    const std::filesystem::path& profile_root, const std::filesystem::path& bundled_root,
    std::string_view file_name = kAssetOverlayOrderFileName);
AssetOverlayLoadoutSelection SelectAssetOverlayLoadout(const AssetOverlayCatalog& catalog,
                                                       const AssetOverlayLoadoutFile& loadout);
AssetOverlayLoadoutSelection ValidateAssetOverlayLoadout(const AssetOverlayCatalog& catalog,
                                                         std::span<const std::string> ids,
                                                         const std::filesystem::path& path = {});
AssetOverlayLoadoutApplyResult ApplyAssetOverlayLoadout(
    const std::filesystem::path& profile_root, const AssetOverlayCatalog& catalog,
    std::span<const std::string> ids, bool replace_invalid_current = false,
    std::string_view file_name = kAssetOverlayOrderFileName);

}  // namespace rex::system
