

/**
 * @file        system/asset_overlay_loadout.cpp
 * @brief       Profile-local ordered asset-overlay loadout
 */

#include <rex/system/asset_overlay_loadout.h>

#include <cctype>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <utility>

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

void AddDiagnostic(std::vector<AssetOverlayLoadoutDiagnostic>& diagnostics, std::string message,
                   size_t line, const fs::path& path) {
  diagnostics.push_back({line, std::move(message), path});
}

bool IsUnsafeExistingPath(const fs::path& path) {
  std::error_code error;
  const auto absolute = fs::absolute(path, error).lexically_normal();
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
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return true;
    }
#endif
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

namespace {

AssetOverlayLoadoutFile ReadOrderFile(const fs::path& profile_path, const fs::path& source_path,
                                      bool profile_exists, bool bundled_default) {
  AssetOverlayLoadoutFile result;
  result.path = profile_path;
  result.source_path = source_path;
  result.exists = profile_exists;
  result.uses_bundled_default = bundled_default;
  const auto file_name =
      bundled_default ? kAssetOverlayDefaultOrderFileName : kAssetOverlayOrderFileName;
  const std::string file_name_text(file_name);

  if (IsUnsafeExistingPath(source_path)) {
    AddDiagnostic(result.diagnostics, file_name_text + " must be a regular file", 0, source_path);
    return result;
  }

  std::error_code error;
  const auto status = fs::symlink_status(source_path, error);
  if (error == std::errc::no_such_file_or_directory) {
    return result;
  }
  if (error) {
    AddDiagnostic(result.diagnostics,
                  "could not inspect " + file_name_text + ": " + error.message(), 0, source_path);
    return result;
  }
  if (fs::is_symlink(status) || !fs::is_regular_file(status)) {
    AddDiagnostic(result.diagnostics, file_name_text + " must be a regular file", 0, source_path);
    return result;
  }

  std::ifstream input(source_path, std::ios::binary);
  if (!input) {
    AddDiagnostic(result.diagnostics, "could not read " + file_name_text, 0, source_path);
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
    if (!IsValidAssetOverlayPackageId(id)) {
      AddDiagnostic(result.diagnostics,
                    file_name_text + " line " + std::to_string(line_number) +
                        " has invalid package ID '" + id + "'",
                    line_number, source_path);
    }
  }
  if (input.bad()) {
    AddDiagnostic(result.diagnostics, "could not read " + file_name_text, line_number, source_path);
  }

  std::unordered_set<std::string> seen;
  for (const auto& entry : result.entries) {
    if (!seen.insert(entry.id).second) {
      AddDiagnostic(result.diagnostics,
                    file_name_text + " line " + std::to_string(entry.line) +
                        " repeats package ID '" + entry.id + "'",
                    entry.line, source_path);
    }
  }
  return result;
}

}  // namespace

AssetOverlayLoadoutFile ReadAssetOverlayLoadout(const fs::path& profile_root) {
  return ReadAssetOverlayLoadout(profile_root, {});
}

AssetOverlayLoadoutFile ReadAssetOverlayLoadout(const fs::path& profile_root,
                                                const fs::path& bundled_root) {
  const auto profile_path = profile_root / fs::path(kAssetOverlayOrderFileName);
  if (profile_root.empty() || IsUnsafeExistingPath(profile_root)) {
    AssetOverlayLoadoutFile result;
    result.path = profile_path;
    result.source_path = profile_path;
    result.exists = true;
    AddDiagnostic(result.diagnostics, "active profile root is invalid", 0, profile_root);
    return result;
  }

  std::error_code error;
  (void)fs::symlink_status(profile_path, error);
  if (!error) {
    return ReadOrderFile(profile_path, profile_path, true, false);
  }
  if (error != std::errc::no_such_file_or_directory) {
    return ReadOrderFile(profile_path, profile_path, true, false);
  }
  if (bundled_root.empty()) {
    AssetOverlayLoadoutFile result;
    result.path = profile_path;
    result.source_path = profile_path;
    return result;
  }

  const auto default_path = bundled_root / fs::path(kAssetOverlayDefaultOrderFileName);
  std::error_code root_error;
  const auto root_status = fs::symlink_status(bundled_root, root_error);
  if (root_error == std::errc::no_such_file_or_directory) {
    AssetOverlayLoadoutFile result;
    result.path = profile_path;
    result.source_path = default_path;
    return result;
  }
  if (root_error || fs::is_symlink(root_status) || !fs::is_directory(root_status)) {
    AssetOverlayLoadoutFile result;
    result.path = profile_path;
    result.source_path = default_path;
    result.uses_bundled_default = true;
    AddDiagnostic(result.diagnostics, "bundled asset overlay root is invalid", 0, bundled_root);
    return result;
  }
  return ReadOrderFile(profile_path, default_path, false, true);
}

AssetOverlayLoadoutSelection SelectAssetOverlayLoadout(const AssetOverlayCatalog& catalog,
                                                       const AssetOverlayLoadoutFile& loadout) {
  AssetOverlayLoadoutSelection result;
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
    if (!IsValidAssetOverlayPackageId(entry.id)) {
      continue;
    }
    if (!seen_ids.insert(entry.id).second) {
      continue;
    }
    const auto* package = catalog.Find(entry.id);
    if (!package) {
      const auto file_name = loadout.uses_bundled_default ? kAssetOverlayDefaultOrderFileName
                                                          : kAssetOverlayOrderFileName;
      const auto& diagnostic_path =
          loadout.source_path.empty() ? loadout.path : loadout.source_path;
      AddDiagnostic(result.diagnostics,
                    std::string(file_name) + " line " + std::to_string(entry.line) +
                        " refers to missing package '" + entry.id + "'",
                    entry.line, diagnostic_path);
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

AssetOverlayLoadoutSelection ValidateAssetOverlayLoadout(const AssetOverlayCatalog& catalog,
                                                         std::span<const std::string> ids,
                                                         const fs::path& path) {
  AssetOverlayLoadoutFile desired;
  desired.path = path;
  desired.entries.reserve(ids.size());
  std::unordered_set<std::string> seen;
  for (size_t index = 0; index < ids.size(); ++index) {
    desired.entries.push_back({ids[index], index + 1});
    if (!IsValidAssetOverlayPackageId(ids[index])) {
      AddDiagnostic(desired.diagnostics,
                    "asset_order.txt line " + std::to_string(index + 1) +
                        " has invalid package ID '" + ids[index] + "'",
                    index + 1, desired.path);
    } else if (!seen.insert(ids[index]).second) {
      AddDiagnostic(desired.diagnostics,
                    "asset_order.txt line " + std::to_string(index + 1) + " repeats package ID '" +
                        ids[index] + "'",
                    index + 1, desired.path);
    }
  }
  return SelectAssetOverlayLoadout(catalog, desired);
}

AssetOverlayLoadoutApplyResult ApplyAssetOverlayLoadout(const fs::path& profile_root,
                                                        const AssetOverlayCatalog& catalog,
                                                        std::span<const std::string> ids,
                                                        bool replace_invalid_current) {
  AssetOverlayLoadoutApplyResult result;
  const auto loadout_path = profile_root / fs::path(kAssetOverlayOrderFileName);
  const auto desired = ValidateAssetOverlayLoadout(catalog, ids, loadout_path);
  result.diagnostics = desired.diagnostics;
  if (!desired.IsValid()) {
    result.status = AssetOverlayLoadoutApplyStatus::kInvalidDesired;
    return result;
  }

  if (profile_root.empty() || IsUnsafeExistingPath(profile_root)) {
    result.status = AssetOverlayLoadoutApplyStatus::kInvalidProfile;
    AddDiagnostic(result.diagnostics, "active profile root is invalid", 0, profile_root);
    return result;
  }
  const auto current = ReadAssetOverlayLoadout(profile_root);
  const auto current_selection = SelectAssetOverlayLoadout(catalog, current);
  if (current.exists && !current_selection.IsValid() && !replace_invalid_current) {
    result.status = AssetOverlayLoadoutApplyStatus::kInvalidCurrent;
    result.diagnostics = current_selection.diagnostics;
    return result;
  }

  std::error_code error;
  fs::create_directories(profile_root, error);
  if (error || IsUnsafeExistingPath(profile_root)) {
    result.status = AssetOverlayLoadoutApplyStatus::kInvalidProfile;
    AddDiagnostic(result.diagnostics, "active profile root is invalid", 0, profile_root);
    return result;
  }

  const auto temporary = profile_root / ".asset_order.txt.tmp";
  if (IsUnsafeExistingPath(temporary)) {
    result.status = AssetOverlayLoadoutApplyStatus::kInvalidProfile;
    AddDiagnostic(result.diagnostics, "temporary loadout path is invalid", 0, temporary);
    return result;
  }

  const std::string bytes = ApplyBytes(ids);
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      result.status = AssetOverlayLoadoutApplyStatus::kIoError;
      AddDiagnostic(result.diagnostics, "could not create temporary asset_order.txt", 0, temporary);
      return result;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
      std::error_code cleanup_error;
      fs::remove(temporary, cleanup_error);
      result.status = AssetOverlayLoadoutApplyStatus::kIoError;
      AddDiagnostic(result.diagnostics, "could not write temporary asset_order.txt", 0, temporary);
      return result;
    }
  }

  if (!AtomicReplace(temporary, loadout_path)) {
    std::error_code cleanup_error;
    fs::remove(temporary, cleanup_error);
    result.status = AssetOverlayLoadoutApplyStatus::kIoError;
    AddDiagnostic(result.diagnostics, "could not atomically replace asset_order.txt", 0,
                  loadout_path);
    return result;
  }
  result.status = AssetOverlayLoadoutApplyStatus::kSuccess;
  return result;
}

}  // namespace rex::system
