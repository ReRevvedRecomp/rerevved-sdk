

/**
 * @file        system/asset_overlay.cpp
 * @brief       Ordered asset-overlay file resolution
 */

#include <rex/system/asset_overlay.h>

#include <fstream>
#include <limits>
#include <utility>
#include <system_error>

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

struct ParsedKey {
  std::vector<std::string> components;
};

rex::Result<ParsedKey> ParseKey(std::string_view logical_key) {
  if (logical_key.empty()) {
    return rex::Err<ParsedKey>(rex::ErrorCategory::Validation, "asset key must be nonempty");
  }
  ParsedKey parsed;
  size_t start = 0;
  while (start <= logical_key.size()) {
    const auto separator = logical_key.find('/', start);
    const auto end = separator == std::string_view::npos ? logical_key.size() : separator;
    const auto component = logical_key.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") {
      return rex::Err<ParsedKey>(rex::ErrorCategory::Validation,
                                 "asset key contains an unsafe path segment");
    }
    for (const unsigned char character : component) {
      if (character < 0x20 || character >= 0x7F || character == '\\' || character == ':') {
        return rex::Err<ParsedKey>(rex::ErrorCategory::Validation,
                                   "asset key must use ASCII forward-slash path syntax");
      }
    }
    parsed.components.emplace_back(component);
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return rex::Ok(std::move(parsed));
}

enum class PathState { kMissing, kDirectory, kRegular, kUnsafe, kOther, kError };

PathState Inspect(const fs::path& path, std::error_code& error) {
  error.clear();
  const auto status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
    return PathState::kMissing;
  }
  if (error) {
    return PathState::kError;
  }
  if (fs::is_symlink(status) || IsWindowsReparsePoint(path)) {
    return PathState::kUnsafe;
  }
  if (fs::is_directory(status)) {
    return PathState::kDirectory;
  }
  if (fs::is_regular_file(status)) {
    return PathState::kRegular;
  }
  return PathState::kOther;
}

fs::path CandidatePath(const AssetOverlayPackage& package, const ParsedKey& key) {
  auto result = package.package_root / "assets";
  for (const auto& component : key.components) {
    result /= component;
  }
  return result;
}

PathState InspectCandidate(const AssetOverlayPackage& package, const ParsedKey& key,
                           std::error_code& error) {
  auto current = package.package_root / "assets";
  auto state = Inspect(current, error);
  if (state != PathState::kDirectory) {
    return state;
  }
  for (size_t index = 0; index < key.components.size(); ++index) {
    current /= key.components[index];
    state = Inspect(current, error);
    if (state == PathState::kMissing || state == PathState::kUnsafe || state == PathState::kError) {
      return state;
    }
    if (index + 1 != key.components.size() && state != PathState::kDirectory) {
      return PathState::kOther;
    }
  }
  return state;
}

rex::Result<AssetOverlayResolution> ReadWinner(const AssetOverlayPackage& package,
                                               const ParsedKey& key,
                                               std::span<const AssetOverlayPackage> packages,
                                               size_t package_index, size_t max_bytes) {
  const auto candidate = CandidatePath(package, key);
  std::error_code error;
  auto state = InspectCandidate(package, key, error);
  if (state == PathState::kUnsafe || state == PathState::kOther) {
    return rex::Err<AssetOverlayResolution>(rex::ErrorCategory::Validation,
                                            "winning asset path is unsafe or not a file");
  }
  if (state == PathState::kError) {
    return rex::Err<AssetOverlayResolution>(rex::ErrorCategory::IO,
                                            "could not inspect winning asset: " + error.message());
  }
  if (state != PathState::kRegular) {
    return rex::Err<AssetOverlayResolution>(rex::ErrorCategory::NotFound,
                                            "winning asset disappeared during resolution");
  }
  const auto size = fs::file_size(candidate, error);
  if (error) {
    return rex::Err<AssetOverlayResolution>(
        rex::ErrorCategory::IO, "could not inspect winning asset size: " + error.message());
  }
  if (size > max_bytes || size > std::numeric_limits<size_t>::max()) {
    return rex::Err<AssetOverlayResolution>(rex::ErrorCategory::Validation,
                                            "winning asset exceeds the requested byte limit");
  }

  AssetOverlayResolution result;
  result.package_id = package.id;
  result.asset_path = candidate;
  result.bytes.resize(static_cast<size_t>(size));
  std::ifstream input(candidate, std::ios::binary);
  if (!input) {
    return rex::Err<AssetOverlayResolution>(rex::ErrorCategory::IO,
                                            "could not open winning asset for reading");
  }
  if (!result.bytes.empty()) {
    input.read(reinterpret_cast<char*>(result.bytes.data()),
               static_cast<std::streamsize>(result.bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(result.bytes.size())) {
      return rex::Err<AssetOverlayResolution>(rex::ErrorCategory::IO,
                                              "could not read winning asset completely");
    }
  }
  for (size_t index = package_index + 1; index < packages.size(); ++index) {
    std::error_code shadow_error;
    if (InspectCandidate(packages[index], key, shadow_error) == PathState::kRegular) {
      result.shadowed_package_ids.push_back(packages[index].id);
    }
  }
  return rex::Ok(std::move(result));
}

}  // namespace

rex::Result<AssetOverlayResolution> ResolveAssetOverlay(
    std::span<const AssetOverlayPackage> selected_packages, std::string_view logical_key,
    size_t max_bytes) {
  auto parsed = ParseKey(logical_key);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  for (size_t index = 0; index < selected_packages.size(); ++index) {
    std::error_code error;
    const auto state = InspectCandidate(selected_packages[index], *parsed, error);
    if (state == PathState::kMissing) {
      continue;
    }
    if (state == PathState::kError) {
      return rex::Err<AssetOverlayResolution>(
          rex::ErrorCategory::IO, "could not inspect asset overlay path: " + error.message());
    }
    if (state != PathState::kRegular) {
      return rex::Err<AssetOverlayResolution>(rex::ErrorCategory::Validation,
                                              "winning asset path is unsafe or not a regular file");
    }
    return ReadWinner(selected_packages[index], *parsed, selected_packages, index, max_bytes);
  }
  return rex::Err<AssetOverlayResolution>(rex::ErrorCategory::NotFound,
                                          "asset was not found in selected overlays");
}

}  // namespace rex::system
