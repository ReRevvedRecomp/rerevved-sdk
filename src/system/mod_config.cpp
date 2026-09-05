/**
 * @file        system/mod_config.cpp
 * @brief       Transitional ordered enabled_mods selection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/mod_config.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace rex::system {
namespace {

std::string Trim(std::string_view text) {
  size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) {
    ++first;
  }
  size_t last = text.size();
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) {
    --last;
  }
  return std::string(text.substr(first, last - first));
}

void AddSelectionError(ModSelection& selection, std::string message,
                       const std::filesystem::path& path = {}) {
  selection.diagnostics.push_back({ModDiagnosticSeverity::kError, true, std::move(message), path});
}

}  // namespace

bool ModSelection::IsValid() const {
  return !std::any_of(diagnostics.begin(), diagnostics.end(),
                      [](const ModDiagnostic& diagnostic) { return diagnostic.blocking; });
}

std::vector<std::string> ParseEnabledModIds(std::string_view enabled_mods) {
  std::vector<std::string> ids;
  size_t start = 0;
  while (start <= enabled_mods.size()) {
    const size_t separator = enabled_mods.find(',', start);
    const size_t end = separator == std::string_view::npos ? enabled_mods.size() : separator;
    std::string id = Trim(enabled_mods.substr(start, end - start));
    if (!id.empty()) {
      ids.push_back(std::move(id));
    }
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return ids;
}

ModSelection SelectEnabledMods(const ModCatalog& catalog, std::string_view enabled_mods) {
  ModSelection selection;
  selection.requested_ids = ParseEnabledModIds(enabled_mods);

  for (const auto& diagnostic : catalog.diagnostics) {
    if (diagnostic.blocking) {
      selection.diagnostics.push_back(diagnostic);
    }
  }

  std::unordered_set<std::string> selected_ids;
  for (const std::string& id : selection.requested_ids) {
    if (!IsValidModPackageId(id)) {
      AddSelectionError(selection, "enabled_mods entry '" + id + "' is not a valid package ID",
                        catalog.mods_root);
      continue;
    }
    if (!selected_ids.insert(id).second) {
      AddSelectionError(selection, "enabled_mods repeats package ID '" + id + "'",
                        catalog.mods_root);
      continue;
    }
    const ModPackage* package = catalog.Find(id);
    if (!package) {
      AddSelectionError(selection, "enabled_mods package '" + id + "' was not found",
                        catalog.mods_root);
      continue;
    }
    selection.packages.push_back(*package);
    if (package->HasBlockingError()) {
      for (const auto& diagnostic : package->diagnostics) {
        if (diagnostic.blocking) {
          selection.diagnostics.push_back(diagnostic);
        }
      }
    }
  }
  return selection;
}

}  // namespace rex::system
