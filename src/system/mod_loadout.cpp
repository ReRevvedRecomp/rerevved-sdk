

/**
 * @file        system/mod_loadout.cpp
 * @brief       Profile-local ordered mod loadout
 */

#include <rex/system/mod_loadout.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rex::system {
namespace {

namespace fs = std::filesystem;

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

void AddDiagnostic(std::vector<ModLoadoutDiagnostic>& diagnostics, std::string message, size_t line,
                   const fs::path& path) {
  diagnostics.push_back({line, std::move(message), path});
}

bool IsUnsafeExistingPath(const fs::path& path) {
  std::error_code error;
  auto absolute = fs::absolute(path, error).lexically_normal();
  if (error) {
    return true;
  }

  fs::path current = absolute.root_path();
  for (const auto& component : absolute.relative_path()) {
    current /= component;
    const auto status = fs::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      break;
    }
    if (error) {
      return true;
    }
    if (fs::is_symlink(status)) {
      return true;
    }
  }
  return false;
}

std::string ApplyBytes(std::span<const std::string> ids) {
  std::string bytes;
  for (const auto& id : ids) {
    bytes += id;
    bytes.push_back('\n');
  }
  return bytes;
}

bool AtomicReplace(const fs::path& temporary, const fs::path& target) {
#if defined(_WIN32)
  return MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error;
  fs::rename(temporary, target, error);
  return !error;
#endif
}

}  // namespace

bool ModLoadoutSelection::IsValid() const {
  return diagnostics.empty();
}

ModLoadoutFile ReadModLoadout(const fs::path& profile_root) {
  ModLoadoutFile result;
  result.path = profile_root / fs::path(kModOrderFileName);

  if (profile_root.empty() || IsUnsafeExistingPath(profile_root)) {
    result.exists = true;
    AddDiagnostic(result.diagnostics, "active profile root is invalid", 0, profile_root);
    return result;
  }

  std::error_code error;
  const auto status = fs::symlink_status(result.path, error);
  if (error == std::errc::no_such_file_or_directory) {
    return result;
  }
  if (error) {
    result.exists = true;
    AddDiagnostic(result.diagnostics, "could not inspect mod_order.txt: " + error.message(), 0,
                  result.path);
    return result;
  }
  result.exists = true;
  if (fs::is_symlink(status) || !fs::is_regular_file(status)) {
    AddDiagnostic(result.diagnostics, "mod_order.txt must be a regular file", 0, result.path);
    return result;
  }

  std::ifstream input(result.path, std::ios::binary);
  if (!input) {
    AddDiagnostic(result.diagnostics, "could not read mod_order.txt", 0, result.path);
    return result;
  }

  std::string line;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const std::string id = Trim(line);
    if (id.empty() || id.front() == '#') {
      continue;
    }
    result.entries.push_back({id, line_number});
    if (!IsValidModPackageId(id)) {
      AddDiagnostic(result.diagnostics,
                    "mod_order.txt line " + std::to_string(line_number) +
                        " has invalid package ID '" + id + "'",
                    line_number, result.path);
    }
  }
  if (input.bad()) {
    AddDiagnostic(result.diagnostics, "could not read mod_order.txt", line_number, result.path);
  }

  std::unordered_set<std::string> seen;
  for (const auto& entry : result.entries) {
    if (!seen.insert(entry.id).second) {
      AddDiagnostic(result.diagnostics,
                    "mod_order.txt line " + std::to_string(entry.line) + " repeats package ID '" +
                        entry.id + "'",
                    entry.line, result.path);
    }
  }
  return result;
}

ModLoadoutSelection SelectModLoadout(const ModCatalog& catalog, const ModLoadoutFile& loadout) {
  ModLoadoutSelection result;
  result.requested_ids.reserve(loadout.entries.size());
  result.diagnostics = loadout.diagnostics;

  for (const auto& diagnostic : catalog.diagnostics) {
    if (diagnostic.blocking) {
      result.diagnostics.push_back({0, diagnostic.message, diagnostic.path});
    }
  }

  std::unordered_set<std::string> seen_ids;
  for (const auto& entry : loadout.entries) {
    result.requested_ids.push_back(entry.id);
    if (!IsValidModPackageId(entry.id)) {
      continue;
    }
    if (!seen_ids.insert(entry.id).second) {
      continue;
    }
    const auto* package = catalog.Find(entry.id);
    if (!package) {
      AddDiagnostic(result.diagnostics,
                    "mod_order.txt line " + std::to_string(entry.line) +
                        " refers to missing package '" + entry.id + "'",
                    entry.line, loadout.path);
      continue;
    }
    result.packages.push_back(*package);
    for (const auto& diagnostic : package->diagnostics) {
      if (diagnostic.blocking) {
        result.diagnostics.push_back({entry.line, diagnostic.message, diagnostic.path});
      }
    }
  }
  return result;
}

ModLoadoutSelection ValidateModLoadout(const ModCatalog& catalog, std::span<const std::string> ids,
                                       const fs::path& path) {
  ModLoadoutFile desired;
  desired.path = path;
  desired.entries.reserve(ids.size());
  std::unordered_set<std::string> seen;
  for (size_t index = 0; index < ids.size(); ++index) {
    desired.entries.push_back({ids[index], index + 1});
    if (!IsValidModPackageId(ids[index])) {
      AddDiagnostic(desired.diagnostics,
                    "mod_order.txt line " + std::to_string(index + 1) +
                        " has invalid package ID '" + ids[index] + "'",
                    index + 1, desired.path);
    } else if (!seen.insert(ids[index]).second) {
      AddDiagnostic(desired.diagnostics,
                    "mod_order.txt line " + std::to_string(index + 1) + " repeats package ID '" +
                        ids[index] + "'",
                    index + 1, desired.path);
    }
  }
  return SelectModLoadout(catalog, desired);
}

ModLoadoutApplyResult ApplyModLoadout(const fs::path& profile_root, const ModCatalog& catalog,
                                      std::span<const std::string> ids,
                                      bool replace_invalid_current) {
  ModLoadoutApplyResult result;
  const auto loadout_path = profile_root / fs::path(kModOrderFileName);
  const auto desired = ValidateModLoadout(catalog, ids, loadout_path);
  result.diagnostics = desired.diagnostics;
  if (!desired.IsValid()) {
    result.status = ModLoadoutApplyStatus::kInvalidDesired;
    return result;
  }

  if (profile_root.empty() || IsUnsafeExistingPath(profile_root)) {
    result.status = ModLoadoutApplyStatus::kInvalidProfile;
    AddDiagnostic(result.diagnostics, "active profile root is invalid", 0, profile_root);
    return result;
  }
  const auto current = ReadModLoadout(profile_root);
  const auto current_selection = SelectModLoadout(catalog, current);
  if (current.exists && !current_selection.IsValid() && !replace_invalid_current) {
    result.status = ModLoadoutApplyStatus::kInvalidCurrent;
    result.diagnostics = current_selection.diagnostics;
    return result;
  }

  std::error_code error;
  fs::create_directories(profile_root, error);
  if (error || IsUnsafeExistingPath(profile_root)) {
    result.status = ModLoadoutApplyStatus::kInvalidProfile;
    AddDiagnostic(result.diagnostics, "active profile root is invalid", 0, profile_root);
    return result;
  }

  const auto temporary = profile_root / ".mod_order.txt.tmp";
  if (IsUnsafeExistingPath(temporary)) {
    result.status = ModLoadoutApplyStatus::kInvalidProfile;
    AddDiagnostic(result.diagnostics, "temporary loadout path is invalid", 0, temporary);
    return result;
  }

  const std::string bytes = ApplyBytes(ids);
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      result.status = ModLoadoutApplyStatus::kIoError;
      AddDiagnostic(result.diagnostics, "could not create temporary mod_order.txt", 0, temporary);
      return result;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
      std::error_code cleanup_error;
      fs::remove(temporary, cleanup_error);
      result.status = ModLoadoutApplyStatus::kIoError;
      AddDiagnostic(result.diagnostics, "could not write temporary mod_order.txt", 0, temporary);
      return result;
    }
  }

  if (!AtomicReplace(temporary, loadout_path)) {
    std::error_code cleanup_error;
    fs::remove(temporary, cleanup_error);
    result.status = ModLoadoutApplyStatus::kIoError;
    AddDiagnostic(result.diagnostics, "could not atomically replace mod_order.txt", 0,
                  loadout_path);
    return result;
  }
  result.status = ModLoadoutApplyStatus::kSuccess;
  return result;
}

}  // namespace rex::system
