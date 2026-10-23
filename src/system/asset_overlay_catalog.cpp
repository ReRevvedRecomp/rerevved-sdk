

/**
 * @file        system/asset_overlay_catalog.cpp
 * @brief       Installed asset-overlay package catalog
 */

#include <rex/system/asset_overlay_catalog.h>

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

#include <toml++/toml.hpp>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::system {
namespace {

namespace fs = std::filesystem;

bool IsWindowsReparsePoint(const fs::path& path) {
#if REX_PLATFORM_WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  (void)path;
  return false;
#endif
}

void AddDiagnostic(AssetOverlayPackage& package, bool blocking, std::string message,
                   const fs::path& path = {}) {
  package.diagnostics.push_back({blocking, std::move(message), path});
}

void AddDiagnostic(AssetOverlayCatalog& catalog, bool blocking, std::string message,
                   const fs::path& path = {}) {
  catalog.diagnostics.push_back({blocking, std::move(message), path});
}

const toml::node* FindNode(const toml::table& table, std::string_view key) {
  auto it = table.find(key);
  return it == table.end() ? nullptr : &it->second;
}

bool ReadRequiredString(const toml::table& table, std::string_view key, std::string& result,
                        AssetOverlayPackage& package) {
  const auto* node = FindNode(table, key);
  if (!node) {
    AddDiagnostic(package, true, "missing required [asset_pack]." + std::string(key));
    return false;
  }
  auto value = node->value<std::string>();
  if (!value) {
    AddDiagnostic(package, true, "[asset_pack]." + std::string(key) + " must be a string");
    return false;
  }
  if (value->empty()) {
    AddDiagnostic(package, true, "[asset_pack]." + std::string(key) + " must be nonempty");
    return false;
  }
  result = std::move(*value);
  return true;
}

bool ReadOptionalString(const toml::table& table, std::string_view key, std::string& result,
                        AssetOverlayPackage& package) {
  const auto* node = FindNode(table, key);
  if (!node) {
    return true;
  }
  auto value = node->value<std::string>();
  if (!value) {
    AddDiagnostic(package, true, "[asset_pack]." + std::string(key) + " must be a string");
    return false;
  }
  result = std::move(*value);
  return true;
}

bool HasSafeNonemptyAssetsTree(const fs::path& root, std::string& error_message) {
  std::error_code error;
  const auto root_status = fs::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory) {
    error_message = "missing assets directory";
    return false;
  }
  if (error) {
    error_message = "could not inspect assets directory: " + error.message();
    return false;
  }
  if (fs::is_symlink(root_status) || IsWindowsReparsePoint(root) ||
      !fs::is_directory(root_status)) {
    error_message = "assets must be a regular directory without links or reparse points";
    return false;
  }

  bool has_regular_file = false;
  fs::recursive_directory_iterator iterator(root, error);
  const fs::recursive_directory_iterator end;
  while (!error && iterator != end) {
    const auto entry = *iterator;
    const auto status = entry.symlink_status(error);
    if (error) {
      break;
    }
    if (fs::is_symlink(status) || IsWindowsReparsePoint(entry.path())) {
      error_message = "assets contain a link or reparse point";
      return false;
    }
    if (fs::is_regular_file(status)) {
      has_regular_file = true;
    } else if (!fs::is_directory(status)) {
      error_message = "assets contain a non-regular entry";
      return false;
    }
    iterator.increment(error);
  }
  if (error) {
    error_message = "could not enumerate assets directory: " + error.message();
    return false;
  }
  if (!has_regular_file) {
    error_message = "assets directory must contain a regular file";
    return false;
  }
  return true;
}

AssetOverlayPackage ParsePackage(const fs::path& root) {
  AssetOverlayPackage package;
  package.package_root = root;
  package.folder_name = root.filename().string();
  package.display_name = package.folder_name;

  const auto manifest_path = root / "asset-pack.toml";
  std::error_code error;
  const auto manifest_status = fs::symlink_status(manifest_path, error);
  if (error || fs::is_symlink(manifest_status) || IsWindowsReparsePoint(manifest_path) ||
      !fs::is_regular_file(manifest_status)) {
    AddDiagnostic(package, true, "missing asset-pack.toml manifest", manifest_path);
    return package;
  }

  toml::table table;
  try {
    table = toml::parse_file(manifest_path.string());
  } catch (const toml::parse_error& parse_error) {
    AddDiagnostic(package, true,
                  "failed to parse asset-pack.toml: " + std::string(parse_error.what()),
                  manifest_path);
    return package;
  }

  for (const auto& [key, value] : table) {
    if (key != "manifest_version" && key != "asset_pack") {
      AddDiagnostic(package, false, "unknown manifest field '" + std::string(key) + "'",
                    manifest_path);
    }
  }

  bool valid = true;
  const auto* manifest_version = FindNode(table, "manifest_version");
  if (!manifest_version) {
    AddDiagnostic(package, true, "missing required manifest_version", manifest_path);
    valid = false;
  } else if (manifest_version->is_integer() &&
             manifest_version->as_integer()->get() == kAssetOverlayManifestVersion) {
    package.manifest_version = kAssetOverlayManifestVersion;
  } else {
    AddDiagnostic(package, true, "manifest_version must be integer 1", manifest_path);
    valid = false;
  }

  const auto* pack_node = FindNode(table, "asset_pack");
  const auto* pack_table = pack_node ? pack_node->as_table() : nullptr;
  if (!pack_table) {
    AddDiagnostic(package, true, "missing required [asset_pack] table", manifest_path);
    return package;
  }
  constexpr std::array<std::string_view, 5> known_fields = {"id", "name", "version", "author",
                                                            "description"};
  for (const auto& [key, value] : *pack_table) {
    if (std::find(known_fields.begin(), known_fields.end(), key) == known_fields.end()) {
      AddDiagnostic(package, false, "unknown [asset_pack] field '" + std::string(key) + "'",
                    manifest_path);
    }
  }
  valid = ReadRequiredString(*pack_table, "id", package.id, package) && valid;
  valid = ReadRequiredString(*pack_table, "name", package.display_name, package) && valid;
  valid = ReadRequiredString(*pack_table, "version", package.version, package) && valid;
  valid = ReadOptionalString(*pack_table, "author", package.author, package) && valid;
  valid = ReadOptionalString(*pack_table, "description", package.description, package) && valid;
  if (!package.id.empty() && !IsValidAssetOverlayPackageId(package.id)) {
    AddDiagnostic(package, true, "asset pack id does not match lowercase package ID grammar",
                  manifest_path);
    valid = false;
  }
  if (!package.id.empty() && package.id != package.folder_name) {
    AddDiagnostic(package, true, "folder name does not match asset pack id", manifest_path);
    valid = false;
  }

  // Keep package versions intentionally small and deterministic: major.minor.
  const auto separator = package.version.find('.');
  if (package.version.empty() || separator == std::string::npos ||
      package.version.find('.', separator + 1) != std::string::npos ||
      package.version.find_first_not_of("0123456789.") != std::string::npos || separator == 0 ||
      separator + 1 == package.version.size()) {
    AddDiagnostic(package, true, "asset pack version must use major.minor numeric syntax",
                  manifest_path);
    valid = false;
  }
  if (!valid) {
    return package;
  }

  package.assets_root = root / "assets";
  std::string assets_error;
  if (!HasSafeNonemptyAssetsTree(package.assets_root, assets_error)) {
    AddDiagnostic(package, true, assets_error, package.assets_root);
    return package;
  }
  return package;
}

}  // namespace

bool IsValidAssetOverlayPackageId(std::string_view id) {
  if (id.empty() || id.size() > 63 || id.front() == '-' || id.back() == '-') {
    return false;
  }
  bool previous_hyphen = false;
  for (const unsigned char character : id) {
    if (character == '-') {
      if (previous_hyphen) {
        return false;
      }
      previous_hyphen = true;
    } else if ((character < 'a' || character > 'z') && (character < '0' || character > '9')) {
      return false;
    } else {
      previous_hyphen = false;
    }
  }
  return true;
}

const AssetOverlayPackage* AssetOverlayCatalog::Find(std::string_view id) const {
  auto it = std::find_if(packages.begin(), packages.end(), [id](const auto& package) {
    return package.id == id && package.folder_name == id;
  });
  return it == packages.end() ? nullptr : &*it;
}

AssetOverlayCatalog DiscoverAssetOverlayCatalog(const fs::path& overlays_root) {
  AssetOverlayCatalog catalog;
  catalog.overlays_root = overlays_root;
  std::error_code error;
  const auto root_status = fs::symlink_status(overlays_root, error);
  if (error || fs::is_symlink(root_status) || IsWindowsReparsePoint(overlays_root) ||
      !fs::is_directory(root_status)) {
    AddDiagnostic(catalog, false, "asset-overrides root does not exist or is not a directory",
                  overlays_root);
    return catalog;
  }

  std::vector<fs::directory_entry> entries;
  fs::directory_iterator iterator(overlays_root, error);
  const fs::directory_iterator end;
  while (!error && iterator != end) {
    const auto entry = *iterator;
    const auto status = entry.symlink_status(error);
    if (!error && !fs::is_symlink(status) && !IsWindowsReparsePoint(entry.path()) &&
        fs::is_directory(status)) {
      entries.push_back(entry);
    }
    iterator.increment(error);
  }
  if (error) {
    AddDiagnostic(catalog, true, "failed to enumerate asset-overrides root: " + error.message(),
                  overlays_root);
  }
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    return left.path().filename().generic_string() < right.path().filename().generic_string();
  });
  catalog.packages.reserve(entries.size());
  for (const auto& entry : entries) {
    catalog.packages.push_back(ParsePackage(entry.path()));
  }
  return catalog;
}

}  // namespace rex::system
