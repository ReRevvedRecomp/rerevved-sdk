/**
 * @file        system/mod_loadout.h
 * @brief       Profile-local ordered mod loadout
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/system/mod_catalog.h>

namespace rex::system {

inline constexpr std::string_view kModOrderFileName = "mod_order.txt";

struct ModLoadoutEntry {
  std::string id;
  size_t line = 0;
};

struct ModLoadoutDiagnostic {
  size_t line = 0;
  std::string message;
  std::filesystem::path path;
};

struct ModLoadoutFile {
  std::filesystem::path path;
  bool exists = false;
  std::vector<ModLoadoutEntry> entries;
  std::vector<ModLoadoutDiagnostic> diagnostics;
};

struct ModLoadoutSelection {
  std::vector<std::string> requested_ids;
  std::vector<ModPackage> packages;
  std::vector<ModLoadoutDiagnostic> diagnostics;

  bool IsValid() const;
};

enum class ModLoadoutApplyStatus {
  kSuccess,
  kInvalidDesired,
  kInvalidCurrent,
  kInvalidProfile,
  kIoError,
  kLocked,  // The application locked loadout changes. Diagnostics carry its reason.
};

struct ModLoadoutApplyResult {
  ModLoadoutApplyStatus status = ModLoadoutApplyStatus::kIoError;
  std::vector<ModLoadoutDiagnostic> diagnostics;

  bool succeeded() const { return status == ModLoadoutApplyStatus::kSuccess; }
};

/// Reads mod_order.txt for the active profile. A missing file means no mods
/// are enabled; it is not an error.
ModLoadoutFile ReadModLoadout(const std::filesystem::path& profile_root);

/// Validates the parsed file against an already-discovered catalog.
ModLoadoutSelection SelectModLoadout(const ModCatalog& catalog, const ModLoadoutFile& loadout);

/// Validates a staged ordered ID list against an already-discovered catalog.
ModLoadoutSelection ValidateModLoadout(const ModCatalog& catalog, std::span<const std::string> ids,
                                       const std::filesystem::path& path = {});

/// Atomically writes a validated ordered ID list. An invalid current file is
/// replaceable only when the caller has explicitly confirmed it.
ModLoadoutApplyResult ApplyModLoadout(const std::filesystem::path& profile_root,
                                      const ModCatalog& catalog, std::span<const std::string> ids,
                                      bool replace_invalid_current = false);

}  // namespace rex::system
