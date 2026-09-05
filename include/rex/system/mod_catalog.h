/**
 * @file        system/mod_catalog.h
 * @brief       Version-1 native mod package catalog
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/system/mod_plugin.h>

namespace rex::system {

inline constexpr uint32_t kModManifestVersion = 1;

bool IsValidModPackageId(std::string_view id);

enum class ModDiagnosticSeverity {
  kWarning,
  kError,
};

struct ModDiagnostic {
  ModDiagnosticSeverity severity = ModDiagnosticSeverity::kError;
  bool blocking = true;
  std::string message;
  std::filesystem::path path;
};

enum class ModPackageStatus {
  kInvalidManifest,
  kIncompatibleGameVersion,
  kUnsupportedPlatform,
  kMissingPayload,
  kPluginAbiMismatch,
  kReady,
  kLoadFailed,
};

// A discovered package is a value object. Discovery only reads this record's
// manifest and payload paths; it never loads, invokes, or modifies package code.
struct ModPackage {
  std::filesystem::path mod_root;
  std::string folder_name;

  // Manifest fields. id is the package identity and must equal folder_name.
  uint32_t manifest_version = 0;
  std::string id;
  std::string display_name;
  std::string version;
  std::string author;
  std::string description;
  std::filesystem::path icon_path;
  std::string code;
  uint32_t plugin_abi = 0;
  std::string min_game_version;

  // Physical platform payload information discovered under code/.
  std::string runtime_platform;
  std::vector<std::string> available_platforms;
  std::filesystem::path plugin_path;

  ModPackageStatus status = ModPackageStatus::kInvalidManifest;
  bool game_compatible = false;
  bool platform_compatible = false;
  bool plugin_abi_compatible = false;
  bool desired = false;
  std::optional<size_t> desired_order;
  bool active = false;
  std::optional<size_t> active_order;
  std::vector<ModDiagnostic> diagnostics;

  bool HasBlockingError() const;
  std::string StatusText() const;
};

struct ModCatalog {
  std::filesystem::path mods_root;
  std::vector<ModPackage> packages;
  std::vector<ModDiagnostic> diagnostics;

  const ModPackage* Find(std::string_view id) const;
  ModPackage* Find(std::string_view id);

  // Runtime-only state used by the read-only manager to distinguish desired
  // entries from plugins that actually completed native loading.
  void SetDesired(std::string_view id, bool desired, std::optional<size_t> order = std::nullopt);
  void SetActive(std::string_view id, bool active, std::optional<size_t> order = std::nullopt);
  void MarkLoadFailed(std::string_view id, std::string message);
};

// Discover exactly one level of direct-child package directories. Symlinked
// children are skipped and no package code is executed or loaded.
ModCatalog DiscoverModCatalog(const std::filesystem::path& mods_root, std::string_view game_version,
                              uint32_t host_plugin_abi = kModPluginAbiVersion);

}  // namespace rex::system
