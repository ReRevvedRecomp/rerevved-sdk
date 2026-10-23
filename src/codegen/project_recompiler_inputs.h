#pragma once

#include <filesystem>
#include <span>
#include <vector>

namespace rex::codegen {
struct RecompilerConfig;

namespace detail {

std::vector<std::filesystem::path> CollectModuleInputs(const RecompilerConfig& cfg,
                                                       const std::filesystem::path& configDir,
                                                       const std::filesystem::path& manifestPath);

std::vector<std::filesystem::path> CollectModuleDependencies(
    const RecompilerConfig& cfg, const std::filesystem::path& configDir,
    std::span<const std::filesystem::path> inputs);

}  // namespace detail
}  // namespace rex::codegen
