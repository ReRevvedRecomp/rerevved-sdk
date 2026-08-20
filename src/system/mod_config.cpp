/**
 * @file        system/mod_config.cpp
 * @brief       Mod manifest resolution and dependency validation
 */

#include <rex/system/mod_config.h>

#include <algorithm>
#include <charconv>
#include <sstream>
#include <unordered_map>

#include <rex/logging.h>

#include <toml++/toml.hpp>

namespace rex::system {
namespace {

std::vector<std::string> SplitCommaList(std::string_view csv) {
  std::vector<std::string> result;
  std::istringstream stream{std::string(csv)};
  std::string token;
  while (std::getline(stream, token, ',')) {
    auto start = token.find_first_not_of(" \t");
    auto end = token.find_last_not_of(" \t");
    if (start != std::string::npos) {
      result.push_back(token.substr(start, end - start + 1));
    }
  }
  return result;
}

ModRequirement ParseRequirement(std::string_view entry) {
  ModRequirement requirement;
  auto operator_position = entry.find(">=");
  if (operator_position == std::string_view::npos) {
    requirement.name = std::string(entry);
    return requirement;
  }

  auto name = entry.substr(0, operator_position);
  auto name_end = name.find_last_not_of(" \t");
  if (name_end != std::string_view::npos) {
    requirement.name = std::string(name.substr(0, name_end + 1));
  }

  auto version = entry.substr(operator_position + 2);
  auto version_start = version.find_first_not_of(" \t");
  if (version_start != std::string_view::npos) {
    requirement.min_version = std::string(version.substr(version_start));
  }
  return requirement;
}

std::string ParseGameVersionConstraint(std::string_view value) {
  auto start = value.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    return {};
  }
  value.remove_prefix(start);
  if (value.starts_with(">=")) {
    value.remove_prefix(2);
    start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos) {
      return {};
    }
    value.remove_prefix(start);
  }
  auto end = value.find_last_not_of(" \t");
  return end == std::string_view::npos ? std::string() : std::string(value.substr(0, end + 1));
}

bool ParseVersionComponents(std::string_view version, std::vector<uint64_t>& components) {
  components.clear();
  while (!version.empty()) {
    auto separator = version.find('.');
    std::string_view component = version.substr(0, separator);
    if (component.empty()) {
      return false;
    }

    uint64_t value = 0;
    auto [end, error] =
        std::from_chars(component.data(), component.data() + component.size(), value);
    if (error != std::errc() || end != component.data() + component.size()) {
      return false;
    }
    components.push_back(value);

    if (separator == std::string_view::npos) {
      return true;
    }
    version.remove_prefix(separator + 1);
  }
  return false;
}

int CompareVersions(const std::vector<uint64_t>& have, const std::vector<uint64_t>& want) {
  size_t count = std::max(have.size(), want.size());
  for (size_t i = 0; i < count; ++i) {
    uint64_t have_component = i < have.size() ? have[i] : 0;
    uint64_t want_component = i < want.size() ? want[i] : 0;
    if (have_component < want_component) {
      return -1;
    }
    if (have_component > want_component) {
      return 1;
    }
  }
  return 0;
}

ModInfo ParseModInfo(const std::filesystem::path& mod_root) {
  ModInfo info;
  info.mod_root = mod_root;
  info.folder_name = mod_root.filename().string();
  info.display_name = info.folder_name;

  auto icon_path = mod_root / "icon.png";
  if (std::filesystem::is_regular_file(icon_path)) {
    info.icon_path = std::move(icon_path);
  }

  auto manifest_path = mod_root / "mod.toml";
  if (!std::filesystem::is_regular_file(manifest_path)) {
    return info;
  }

  try {
    auto table = toml::parse_file(manifest_path.string());
    info.display_name = table["name"].value_or<std::string>(std::string(info.folder_name));
    info.version = table["version"].value_or<std::string>("");
    info.author = table["author"].value_or<std::string>("");
    info.description = table["description"].value_or<std::string>("");
    info.code = table["code"].value_or<std::string>("");
    for (const auto& entry : SplitCommaList(table["requires"].value_or<std::string>(""))) {
      info.requires_mods.push_back(ParseRequirement(entry));
    }
    info.load_after_mods = SplitCommaList(table["load_after"].value_or<std::string>(""));
    info.conflicts_mods = SplitCommaList(table["conflicts"].value_or<std::string>(""));
    info.min_game_version =
        ParseGameVersionConstraint(table["game_version"].value_or<std::string>(""));
    info.platforms = SplitCommaList(table["platform"].value_or<std::string>(""));
  } catch (const toml::parse_error& error) {
    REXSYS_WARN("Failed to parse {}: {}", manifest_path.string(), error.what());
  }
  return info;
}

}  // namespace

std::vector<ModInfo> ResolveModConfiguration(const std::filesystem::path& mods_root,
                                             std::string_view enabled_mods) {
  std::vector<ModInfo> result;
  if (enabled_mods.empty()) {
    return result;
  }
  if (!std::filesystem::is_directory(mods_root)) {
    REXSYS_WARN("Mods folder does not exist: {}", mods_root.string());
    return result;
  }

  for (const auto& name : SplitCommaList(enabled_mods)) {
    std::filesystem::path relative_name(name);
    if (relative_name.is_absolute() || relative_name.has_parent_path() ||
        relative_name.filename() != relative_name || name == "." || name == "..") {
      REXSYS_ERROR("Invalid mod folder name '{}'; enabled_mods entries must be direct children",
                   name);
      continue;
    }

    auto mod_root = mods_root / relative_name;
    if (!std::filesystem::is_directory(mod_root)) {
      REXSYS_WARN("Mod '{}' was not found at {}; skipping", name, mod_root.string());
      continue;
    }
    result.push_back(ParseModInfo(mod_root));
    REXSYS_INFO("Mod enabled: {} ({})", name, mod_root.string());
  }
  return result;
}

bool ValidateModConfiguration(std::span<const ModInfo> mods, std::string_view game_version) {
  std::unordered_map<std::string, size_t> index_by_name;
  for (size_t i = 0; i < mods.size(); ++i) {
    index_by_name.emplace(mods[i].folder_name, i);
  }

  bool versions_valid = true;
  for (size_t i = 0; i < mods.size(); ++i) {
    const auto& mod = mods[i];
    for (const auto& requirement : mod.requires_mods) {
      if (requirement.name == mod.folder_name) {
        REXSYS_ERROR("Mod '{}' lists itself in 'requires'", mod.folder_name);
        continue;
      }
      auto dependency = index_by_name.find(requirement.name);
      if (dependency == index_by_name.end()) {
        REXSYS_ERROR("Mod '{}' requires '{}'; enable it before '{}'", mod.folder_name,
                     requirement.name, mod.folder_name);
        continue;
      }
      if (dependency->second > i) {
        REXSYS_ERROR("Mod '{}' requires '{}' to load first; move '{}' earlier", mod.folder_name,
                     requirement.name, requirement.name);
        continue;
      }
      if (requirement.min_version.empty()) {
        continue;
      }

      std::vector<uint64_t> wanted;
      std::vector<uint64_t> available;
      const auto& dependency_info = mods[dependency->second];
      if (!ParseVersionComponents(requirement.min_version, wanted)) {
        REXSYS_WARN("Mod '{}' has an invalid version constraint for '{}': {}", mod.folder_name,
                    requirement.name, requirement.min_version);
      } else if (!ParseVersionComponents(dependency_info.version, available)) {
        REXSYS_WARN("Mod '{}' requires '{} >= {}', but '{}' has no valid version", mod.folder_name,
                    requirement.name, requirement.min_version, requirement.name);
      } else if (CompareVersions(available, wanted) < 0) {
        REXSYS_ERROR("Mod '{}' requires '{} >= {}', but version {} is enabled", mod.folder_name,
                     requirement.name, requirement.min_version, dependency_info.version);
        versions_valid = false;
      }
    }

    if (!mod.min_game_version.empty()) {
      std::vector<uint64_t> wanted;
      std::vector<uint64_t> available;
      if (!ParseVersionComponents(mod.min_game_version, wanted)) {
        REXSYS_WARN("Mod '{}' has an invalid game_version constraint: {}", mod.folder_name,
                    mod.min_game_version);
      } else if (!ParseVersionComponents(game_version, available)) {
        REXSYS_WARN("Mod '{}' requires game version {}, but the host did not publish one",
                    mod.folder_name, mod.min_game_version);
      } else if (CompareVersions(available, wanted) < 0) {
        REXSYS_ERROR("Mod '{}' requires game version {}, but the host is {}", mod.folder_name,
                     mod.min_game_version, game_version);
        versions_valid = false;
      }
    }

    for (const auto& other : mod.load_after_mods) {
      auto dependency = index_by_name.find(other);
      if (dependency == index_by_name.end()) {
        REXSYS_WARN("Mod '{}' should load after '{}', which is not enabled", mod.folder_name,
                    other);
      } else if (dependency->second > i) {
        REXSYS_WARN("Mod '{}' should load after '{}'; move '{}' earlier", mod.folder_name, other,
                    other);
      }
    }

    for (const auto& other : mod.conflicts_mods) {
      if (other == mod.folder_name) {
        REXSYS_ERROR("Mod '{}' lists itself in 'conflicts'", mod.folder_name);
      } else if (index_by_name.contains(other)) {
        REXSYS_ERROR("Mod '{}' conflicts with '{}'; disable one of them", mod.folder_name, other);
      }
    }
  }
  return versions_valid;
}

}  // namespace rex::system
