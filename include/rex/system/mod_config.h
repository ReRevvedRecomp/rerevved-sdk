/**
 * @file        system/mod_config.h
 * @brief       Transitional ordered enabled_mods selection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rex/system/mod_catalog.h>

namespace rex::system {

struct ModSelection {
  std::vector<std::string> requested_ids;
  std::vector<ModPackage> packages;
  std::vector<ModDiagnostic> diagnostics;

  bool IsValid() const;
};

// enabled_mods remains a comma-separated transition cvar until profile-local
// mod_order.txt is adopted. Empty entries are ignored; every effective entry
// must be one exact package ID and selection preserves line order.
std::vector<std::string> ParseEnabledModIds(std::string_view enabled_mods);

// Select records from an already-discovered catalog without parsing manifests
// or touching the filesystem. A missing, repeated, or invalid selected record
// is a blocking selection error and the order is never inferred or sorted.
ModSelection SelectEnabledMods(const ModCatalog& catalog, std::string_view enabled_mods);

}  // namespace rex::system
