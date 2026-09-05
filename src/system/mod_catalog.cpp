/**
 * @file        system/mod_catalog.cpp
 * @brief       Version-1 native mod package catalog
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/mod_catalog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include <toml++/toml.hpp>

#include <rex/platform.h>

namespace rex::system {
namespace {

#ifndef REXGLUE_BUILD_CONFIG
#define REXGLUE_BUILD_CONFIG "Release"
#endif

struct ParsedVersion {
  uint64_t major = 0;
  uint64_t minor = 0;
  uint64_t patch = 0;
};

bool ParseVersion(std::string_view text, ParsedVersion& result) {
  std::array<uint64_t*, 3> components{&result.major, &result.minor, &result.patch};
  for (size_t index = 0; index < components.size(); ++index) {
    const size_t end = text.find('.');
    const std::string_view component = text.substr(0, end);
    if (component.empty()) {
      return false;
    }
    auto [last, error] =
        std::from_chars(component.data(), component.data() + component.size(), *components[index]);
    if (error != std::errc() || last != component.data() + component.size()) {
      return false;
    }
    if (end == std::string_view::npos) {
      return index == components.size() - 1;
    }
    text.remove_prefix(end + 1);
  }
  return false;
}

bool IsPackageVersion(std::string_view text) {
  const size_t first_separator = text.find('.');
  if (first_separator == std::string_view::npos ||
      text.find('.', first_separator + 1) != std::string_view::npos) {
    return false;
  }

  const std::array<std::string_view, 2> components = {text.substr(0, first_separator),
                                                      text.substr(first_separator + 1)};
  for (const std::string_view component : components) {
    if (component.empty()) {
      return false;
    }
    uint64_t value = 0;
    auto [last, error] =
        std::from_chars(component.data(), component.data() + component.size(), value);
    if (error != std::errc() || last != component.data() + component.size()) {
      return false;
    }
  }
  return true;
}

int CompareVersions(const ParsedVersion& left, const ParsedVersion& right) {
  if (left.major != right.major) {
    return left.major < right.major ? -1 : 1;
  }
  if (left.minor != right.minor) {
    return left.minor < right.minor ? -1 : 1;
  }
  if (left.patch != right.patch) {
    return left.patch < right.patch ? -1 : 1;
  }
  return 0;
}

bool IsCodeStem(std::string_view code) {
  if (code.empty() || code == "." || code == ".." || code.find('/') != std::string_view::npos ||
      code.find('\\') != std::string_view::npos) {
    return false;
  }
  const std::filesystem::path path(code);
  return !path.is_absolute() && !path.has_parent_path() && path.filename() == path &&
         path.extension().empty();
}

constexpr std::string_view RuntimePlatform() {
#if REX_PLATFORM_WIN32
#if defined(REX_ARCH_ARM64)
  return "windows-arm64";
#else
  return "windows-x64";
#endif
#elif REX_PLATFORM_LINUX
#if defined(REX_ARCH_ARM64)
  return "linux-arm64";
#else
  return "linux-x64";
#endif
#elif REX_PLATFORM_MAC
#if defined(REX_ARCH_ARM64)
  return "macos-arm64";
#else
  return "macos-x64";
#endif
#else
  return "";
#endif
}

constexpr std::array<std::string_view, 6> kRuntimePlatforms = {
    "windows-x64", "windows-arm64", "linux-x64", "linux-arm64", "macos-x64", "macos-arm64"};

bool IsRuntimePlatform(std::string_view platform) {
  return std::find(kRuntimePlatforms.begin(), kRuntimePlatforms.end(), platform) !=
         kRuntimePlatforms.end();
}

std::string BinaryNameForPlatform(std::string_view platform, std::string_view stem,
                                  std::string_view postfix) {
  std::string prefix = platform.starts_with("windows-") ? "" : "lib";
  std::string extension;
  if (platform.starts_with("windows-")) {
    extension = ".dll";
  } else if (platform.starts_with("macos-")) {
    extension = ".dylib";
  } else {
    extension = ".so";
  }
  return prefix + std::string(stem) + std::string(postfix) + extension;
}

std::string_view ConfigPostfix() {
  constexpr std::string_view config = REXGLUE_BUILD_CONFIG;
  if (config == "Debug") {
    return "d";
  }
  if (config == "RelWithDebInfo") {
    return "rd";
  }
  return "";
}

const toml::node* FindNode(const toml::table& table, std::string_view key) {
  auto it = table.find(key);
  return it == table.end() ? nullptr : &it->second;
}

void AddDiagnostic(ModPackage& package, ModDiagnosticSeverity severity, bool blocking,
                   std::string message, const std::filesystem::path& path = {}) {
  package.diagnostics.push_back({severity, blocking, std::move(message), path});
}

void AddCatalogDiagnostic(ModCatalog& catalog, ModDiagnosticSeverity severity, bool blocking,
                          std::string message, const std::filesystem::path& path = {}) {
  catalog.diagnostics.push_back({severity, blocking, std::move(message), path});
}

bool ReadRequiredString(const toml::table& table, std::string_view key, std::string& result,
                        ModPackage& package) {
  const auto* node = FindNode(table, key);
  if (!node) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "missing required [mod]." + std::string(key));
    return false;
  }
  auto value = node->value<std::string>();
  if (!value) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "[mod]." + std::string(key) + " must be a string");
    return false;
  }
  result = std::move(*value);
  if (result.empty()) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "[mod]." + std::string(key) + " must be nonempty");
    return false;
  }
  return true;
}

bool ReadOptionalString(const toml::table& table, std::string_view key, std::string& result,
                        ModPackage& package) {
  const auto* node = FindNode(table, key);
  if (!node) {
    return true;
  }
  auto value = node->value<std::string>();
  if (!value) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "[mod]." + std::string(key) + " must be a string");
    return false;
  }
  result = std::move(*value);
  return true;
}

ModPackage ParsePackage(const std::filesystem::path& mod_root, std::string_view game_version,
                        uint32_t host_plugin_abi) {
  ModPackage package;
  package.mod_root = mod_root;
  package.folder_name = mod_root.filename().string();
  package.display_name = package.folder_name;
  package.runtime_platform = RuntimePlatform();

  std::error_code error;
  const auto icon_path = mod_root / "icon.png";
  const auto icon_status = std::filesystem::symlink_status(icon_path, error);
  if (!error && !std::filesystem::is_symlink(icon_status) &&
      std::filesystem::is_regular_file(icon_status)) {
    package.icon_path = icon_path;
  }

  const auto manifest_path = mod_root / "mod.toml";
  const auto manifest_status = std::filesystem::symlink_status(manifest_path, error);
  if (error || std::filesystem::is_symlink(manifest_status) ||
      !std::filesystem::is_regular_file(manifest_status)) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true, "missing mod.toml manifest",
                  manifest_path);
    return package;
  }

  toml::table table;
  try {
    table = toml::parse_file(manifest_path.string());
  } catch (const toml::parse_error& parse_error) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "failed to parse mod.toml: " + std::string(parse_error.what()), manifest_path);
    return package;
  }

  for (const auto& [key, value] : table) {
    if (key != "manifest_version" && key != "mod") {
      AddDiagnostic(package, ModDiagnosticSeverity::kWarning, false,
                    "unknown manifest field '" + std::string(key) + "'", manifest_path);
    }
  }

  const auto* manifest_version = FindNode(table, "manifest_version");
  if (!manifest_version) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true, "missing required manifest_version",
                  manifest_path);
  } else if (manifest_version->is_integer() && manifest_version->as_integer()->get() == 1) {
    package.manifest_version = kModManifestVersion;
  } else {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "manifest_version must be integer 1", manifest_path);
  }

  const auto* mod_node = FindNode(table, "mod");
  const auto* mod_table = mod_node ? mod_node->as_table() : nullptr;
  if (!mod_table) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true, "missing required [mod] table",
                  manifest_path);
    return package;
  }

  constexpr std::array<std::string_view, 8> known_mod_fields = {
      "id", "name", "version", "author", "description", "min_game_version", "code", "plugin_abi"};
  for (const auto& [key, value] : *mod_table) {
    if (std::find(known_mod_fields.begin(), known_mod_fields.end(), key) ==
        known_mod_fields.end()) {
      AddDiagnostic(package, ModDiagnosticSeverity::kWarning, false,
                    "unknown [mod] field '" + std::string(key) + "'", manifest_path);
    }
  }

  bool valid = package.manifest_version == kModManifestVersion;
  valid = ReadRequiredString(*mod_table, "id", package.id, package) && valid;
  valid = ReadRequiredString(*mod_table, "name", package.display_name, package) && valid;
  valid = ReadRequiredString(*mod_table, "version", package.version, package) && valid;
  valid = ReadRequiredString(*mod_table, "code", package.code, package) && valid;
  valid = ReadOptionalString(*mod_table, "author", package.author, package) && valid;
  valid = ReadOptionalString(*mod_table, "description", package.description, package) && valid;
  const auto* min_game_version = FindNode(*mod_table, "min_game_version");
  bool min_game_version_is_string = true;
  if (min_game_version) {
    auto value = min_game_version->value<std::string>();
    if (!value) {
      AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                    "[mod].min_game_version must be a string");
      valid = false;
      min_game_version_is_string = false;
    } else {
      package.min_game_version = std::move(*value);
    }
  }

  const auto* plugin_abi = FindNode(*mod_table, "plugin_abi");
  if (!plugin_abi) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "missing required [mod].plugin_abi");
    valid = false;
  } else if (plugin_abi->is_integer() && plugin_abi->as_integer()->get() >= 0 &&
             static_cast<uint64_t>(plugin_abi->as_integer()->get()) <=
                 std::numeric_limits<uint32_t>::max()) {
    package.plugin_abi = static_cast<uint32_t>(plugin_abi->as_integer()->get());
  } else {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "[mod].plugin_abi must be a non-negative integer");
    valid = false;
  }

  if (!package.id.empty() && !IsValidModPackageId(package.id)) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "package id '" + package.id +
                      "' does not match the required lowercase ID "
                      "grammar");
    valid = false;
  }
  if (!package.id.empty() && package.id != package.folder_name) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "folder name '" + package.folder_name + "' does not match manifest id '" +
                      package.id + "'");
    valid = false;
  }

  ParsedVersion parsed_version;
  if (!package.version.empty() && !IsPackageVersion(package.version)) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "package version must use major.minor numeric syntax");
    valid = false;
  }
  if (min_game_version && min_game_version_is_string && !package.min_game_version.empty() &&
      !ParseVersion(package.min_game_version, parsed_version)) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "min_game_version must use major.minor.patch numeric syntax");
    valid = false;
  } else if (min_game_version && min_game_version_is_string && package.min_game_version.empty()) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "[mod].min_game_version must be nonempty");
    valid = false;
  }
  if (!package.code.empty() && !IsCodeStem(package.code)) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "code must be one filename stem without a path or extension");
    valid = false;
  }

  if (!valid) {
    package.status = ModPackageStatus::kInvalidManifest;
    return package;
  }

  package.game_compatible = true;
  if (min_game_version) {
    ParsedVersion minimum;
    ParsedVersion host;
    if (!ParseVersion(package.min_game_version, minimum) || !ParseVersion(game_version, host)) {
      package.game_compatible = false;
      AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                    "host game version is unavailable or invalid for min_game_version");
    } else if (CompareVersions(host, minimum) < 0) {
      package.game_compatible = false;
      AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                    "host game version " + std::string(game_version) +
                        " is below required min_game_version " + package.min_game_version);
    }
  }

  package.plugin_abi_compatible = package.plugin_abi == host_plugin_abi;
  if (!package.plugin_abi_compatible) {
    AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                  "plugin ABI " + std::to_string(package.plugin_abi) + " does not match host ABI " +
                      std::to_string(host_plugin_abi));
  }

  const auto code_root = mod_root / "code";
  const auto code_status = std::filesystem::symlink_status(code_root, error);
  if (!error && std::filesystem::is_directory(code_status) &&
      !std::filesystem::is_symlink(code_status)) {
    std::error_code iteration_error;
    std::filesystem::directory_iterator platform_iterator(code_root, iteration_error);
    const std::filesystem::directory_iterator end;
    while (!iteration_error && platform_iterator != end) {
      const auto platform_entry = *platform_iterator;
      std::error_code status_error;
      const auto platform_status = platform_entry.symlink_status(status_error);
      if (!status_error && !std::filesystem::is_symlink(platform_status) &&
          std::filesystem::is_directory(platform_status)) {
        const auto platform_name = platform_entry.path().filename().generic_string();
        if (IsRuntimePlatform(platform_name)) {
          const std::array<std::string_view, 2> postfixes = {ConfigPostfix(), ""};
          for (const auto postfix : postfixes) {
            const auto candidate =
                platform_entry.path() / BinaryNameForPlatform(platform_name, package.code, postfix);
            std::error_code candidate_error;
            const auto candidate_status =
                std::filesystem::symlink_status(candidate, candidate_error);
            if (!candidate_error && !std::filesystem::is_symlink(candidate_status) &&
                std::filesystem::is_regular_file(candidate_status)) {
              package.available_platforms.push_back(platform_name);
              break;
            }
          }
        }
      }
      platform_iterator.increment(iteration_error);
    }
    if (iteration_error) {
      AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                    "failed to enumerate native payload platforms: " + iteration_error.message(),
                    code_root);
    }
  }
  std::sort(package.available_platforms.begin(), package.available_platforms.end());
  package.available_platforms.erase(
      std::unique(package.available_platforms.begin(), package.available_platforms.end()),
      package.available_platforms.end());

  package.platform_compatible = false;
  if (!package.runtime_platform.empty() &&
      std::find(package.available_platforms.begin(), package.available_platforms.end(),
                package.runtime_platform) != package.available_platforms.end()) {
    const auto platform_root = code_root / package.runtime_platform;
    const auto config_postfix = ConfigPostfix();
    const auto qualified_current =
        platform_root /
        BinaryNameForPlatform(package.runtime_platform, package.code, config_postfix);
    const auto qualified_release =
        platform_root / BinaryNameForPlatform(package.runtime_platform, package.code, "");

    auto resolve_loader_path = [&](std::string_view postfix, std::error_code& resolve_error) {
      const auto qualified =
          platform_root / BinaryNameForPlatform(package.runtime_platform, package.code, postfix);
      if (std::filesystem::exists(qualified, resolve_error) || resolve_error) {
        return qualified;
      }
      const auto flat =
          code_root / BinaryNameForPlatform(package.runtime_platform, package.code, postfix);
      std::filesystem::exists(flat, resolve_error);
      return flat;
    };

    std::error_code resolve_error;
    auto loader_path = resolve_loader_path(config_postfix, resolve_error);
    bool loader_path_exists = false;
    if (!resolve_error) {
      loader_path_exists = std::filesystem::exists(loader_path, resolve_error);
      if (!resolve_error && !config_postfix.empty() && !loader_path_exists) {
        loader_path = resolve_loader_path("", resolve_error);
        if (!resolve_error) {
          loader_path_exists = std::filesystem::exists(loader_path, resolve_error);
        }
      }
    }

    if (resolve_error) {
      AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                    "failed to resolve native payload for loader: " + resolve_error.message(),
                    code_root);
    } else if (loader_path_exists &&
               (loader_path == qualified_current || loader_path == qualified_release)) {
      std::error_code candidate_error;
      const auto candidate_status = std::filesystem::symlink_status(loader_path, candidate_error);
      if (!candidate_error && !std::filesystem::is_symlink(candidate_status) &&
          std::filesystem::is_regular_file(candidate_status)) {
        package.plugin_path = loader_path;
        package.platform_compatible = true;
      } else {
        AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                      "loader-selected native payload is not a regular non-linked file",
                      loader_path);
      }
    } else if (loader_path_exists) {
      AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                    "loader resolves native payload to noncanonical path; qualified payload is "
                    "required",
                    loader_path);
    }
  }
  if (!package.platform_compatible) {
    if (std::find(package.available_platforms.begin(), package.available_platforms.end(),
                  package.runtime_platform) == package.available_platforms.end()) {
      AddDiagnostic(
          package, ModDiagnosticSeverity::kError, true,
          "no native payload is available for runtime platform '" + package.runtime_platform + "'");
    } else {
      AddDiagnostic(package, ModDiagnosticSeverity::kError, true,
                    "native payload for '" + package.code + "' is missing on runtime platform '" +
                        package.runtime_platform + "'");
    }
  }

  if (!package.game_compatible) {
    package.status = ModPackageStatus::kIncompatibleGameVersion;
  } else if (!package.plugin_abi_compatible) {
    package.status = ModPackageStatus::kPluginAbiMismatch;
  } else if (!package.platform_compatible) {
    package.status = package.available_platforms.empty() ? ModPackageStatus::kMissingPayload
                                                         : ModPackageStatus::kUnsupportedPlatform;
  } else {
    package.status = ModPackageStatus::kReady;
  }
  return package;
}

}  // namespace

bool IsValidModPackageId(std::string_view id) {
  if (id.empty() || id.size() > 63 || id.front() == '-' || id.back() == '-') {
    return false;
  }
  bool previous_hyphen = false;
  for (unsigned char character : id) {
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

bool ModPackage::HasBlockingError() const {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [](const ModDiagnostic& diagnostic) { return diagnostic.blocking; });
}

std::string ModPackage::StatusText() const {
  switch (status) {
    case ModPackageStatus::kInvalidManifest:
      return "invalid manifest";
    case ModPackageStatus::kIncompatibleGameVersion:
      return "incompatible game version";
    case ModPackageStatus::kUnsupportedPlatform:
      return "unsupported platform";
    case ModPackageStatus::kMissingPayload:
      return "missing native payload";
    case ModPackageStatus::kPluginAbiMismatch:
      return "plugin ABI mismatch";
    case ModPackageStatus::kReady:
      return active ? "active" : (desired ? "enabled" : "disabled");
    case ModPackageStatus::kLoadFailed:
      return "native load failed";
  }
  return "unknown";
}

const ModPackage* ModCatalog::Find(std::string_view id) const {
  auto it = std::find_if(packages.begin(), packages.end(), [id](const ModPackage& package) {
    return package.folder_name == id && package.id == id;
  });
  return it == packages.end() ? nullptr : &*it;
}

ModPackage* ModCatalog::Find(std::string_view id) {
  auto it = std::find_if(packages.begin(), packages.end(), [id](const ModPackage& package) {
    return package.folder_name == id && package.id == id;
  });
  return it == packages.end() ? nullptr : &*it;
}

void ModCatalog::SetDesired(std::string_view id, bool desired_state, std::optional<size_t> order) {
  if (auto* package = Find(id)) {
    package->desired = desired_state;
    package->desired_order = desired_state ? order : std::nullopt;
  }
}

void ModCatalog::SetActive(std::string_view id, bool active_state, std::optional<size_t> order) {
  if (auto* package = Find(id)) {
    package->active = active_state;
    package->active_order = active_state ? order : std::nullopt;
  }
}

void ModCatalog::MarkLoadFailed(std::string_view id, std::string message) {
  if (auto* package = Find(id)) {
    package->status = ModPackageStatus::kLoadFailed;
    package->active = false;
    package->active_order.reset();
    AddDiagnostic(*package, ModDiagnosticSeverity::kError, false, std::move(message));
  }
}

ModCatalog DiscoverModCatalog(const std::filesystem::path& mods_root, std::string_view game_version,
                              uint32_t host_plugin_abi) {
  ModCatalog catalog;
  catalog.mods_root = mods_root;

  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(mods_root, error);
  if (error || std::filesystem::is_symlink(root_status) ||
      !std::filesystem::is_directory(root_status)) {
    AddCatalogDiagnostic(catalog, ModDiagnosticSeverity::kWarning, false,
                         "mods root does not exist or is not a directory", mods_root);
    return catalog;
  }

  std::vector<std::filesystem::directory_entry> entries;
  std::error_code iteration_error;
  std::filesystem::directory_iterator iterator(mods_root, iteration_error);
  const std::filesystem::directory_iterator end;
  while (!iteration_error && iterator != end) {
    const auto entry = *iterator;
    std::error_code status_error;
    const auto status = entry.symlink_status(status_error);
    if (!status_error && !std::filesystem::is_symlink(status) &&
        std::filesystem::is_directory(status)) {
      entries.push_back(entry);
    }
    iterator.increment(iteration_error);
  }
  if (iteration_error) {
    AddCatalogDiagnostic(catalog, ModDiagnosticSeverity::kError, true,
                         "failed to enumerate mods root: " + iteration_error.message(), mods_root);
  }
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    return left.path().filename().generic_string() < right.path().filename().generic_string();
  });
  catalog.packages.reserve(entries.size());
  for (const auto& entry : entries) {
    catalog.packages.push_back(ParsePackage(entry.path(), game_version, host_plugin_abi));
  }
  return catalog;
}

}  // namespace rex::system
