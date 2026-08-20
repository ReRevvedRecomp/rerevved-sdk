/**
 * @file        system/mod_config.h
 * @brief       Mod manifest resolution and dependency validation
 */

#pragma once

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <rex/system/mod_plugin.h>

namespace rex::system {

std::vector<ModInfo> ResolveModConfiguration(const std::filesystem::path& mods_root,
                                             std::string_view enabled_mods);

// Returns false only for a verified dependency or host version mismatch.
// Missing dependencies, ordering hints, conflicts, and unverifiable versions
// are diagnosed but do not block host startup.
bool ValidateModConfiguration(std::span<const ModInfo> mods, std::string_view game_version);

}  // namespace rex::system
