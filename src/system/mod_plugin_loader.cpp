/**
 * @file        system/mod_plugin_loader.cpp
 * @brief       Host-side loader for mod code-plugin DLLs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/mod_plugin.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

namespace rex::system {
namespace {

std::string ModFileName(std::string_view stem, std::string_view postfix) {
#if REX_PLATFORM_WIN32
  return fmt::format("{}{}.dll", stem, postfix);
#else
  return fmt::format("lib{}{}.so", stem, postfix);
#endif
}

constexpr std::string_view ModPlatformDir() {
#if REX_PLATFORM_WIN32
#if defined(REX_ARCH_ARM64)
  return "windows-arm64";
#else
  return "windows-x64";
#endif
#elif REX_PLATFORM_LINUX
#if defined(REX_ARCH_ARM64)
  return "linux-arm64";
#elif defined(REX_ARCH_AMD64)
  return "linux-x64";
#else
  return "";
#endif
#else
  return "";
#endif
}

std::string MismatchedRuntimeDependency(const std::filesystem::path& path,
                                        std::string_view expected_postfix) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  static constexpr std::string_view kConfigPostfixes[] = {"", "d", "rd"};
  for (std::string_view other_postfix : kConfigPostfixes) {
    if (other_postfix == expected_postfix) {
      continue;
    }
    std::string other_name = ModFileName("rexruntime", other_postfix);
    if (contents.find(other_name) != std::string::npos) {
      return other_name;
    }
  }
  return {};
}

std::vector<platform::DynamicLibrary>& LoadedModPlugins() {
  static std::vector<platform::DynamicLibrary> plugins;
  return plugins;
}

}  // namespace

std::unique_ptr<IModPlugin> LoadModPlugin(const std::filesystem::path& mod_root,
                                          std::string_view mod_name, std::string_view code_stem,
                                          const ModHostContext& context) {
  std::filesystem::path relative_stem(code_stem);
  if (code_stem.empty() || relative_stem.is_absolute() || relative_stem.has_parent_path() ||
      relative_stem.filename() != relative_stem || code_stem == "." || code_stem == "..") {
    REXSYS_ERROR("Mod '{}' declares invalid code stem '{}'; expected a binary name without a path",
                 mod_name, code_stem);
    return nullptr;
  }

  const std::filesystem::path code_dir = mod_root / "code";

  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  std::string_view postfix;
  if (kConfig == "Debug") {
    postfix = "d";
  } else if (kConfig == "RelWithDebInfo") {
    postfix = "rd";
  }

  auto resolve = [&](std::string_view candidate_postfix) {
    std::string_view platform_dir = ModPlatformDir();
    if (!platform_dir.empty()) {
      auto platform_path = code_dir / platform_dir / ModFileName(code_stem, candidate_postfix);
      if (std::filesystem::exists(platform_path)) {
        return platform_path;
      }
    }
    return code_dir / ModFileName(code_stem, candidate_postfix);
  };

  std::string_view resolved_postfix = postfix;
  std::filesystem::path path = resolve(postfix);
  if (!postfix.empty() && !std::filesystem::exists(path)) {
    path = resolve("");
    resolved_postfix = "";
  }
  if (!std::filesystem::exists(path)) {
    REXSYS_ERROR("Mod '{}' declares code '{}' but no plugin binary was found at {}", mod_name,
                 code_stem, path.string());
    return nullptr;
  }

  if (std::string mismatched = MismatchedRuntimeDependency(path, resolved_postfix);
      !mismatched.empty()) {
    REXSYS_ERROR(
        "Mod '{}' was built against a different SDK build configuration (links {}, host is {}); "
        "skipping {}",
        mod_name, mismatched, kConfig, path.string());
    return nullptr;
  }

  platform::DynamicLibrary library;
  if (!library.Load(path, platform::SymbolResolution::kImmediate)) {
    REXSYS_ERROR("Mod '{}' code plugin failed to load: {}", mod_name, path.string());
    return nullptr;
  }

  auto abi_version_fn = library.GetSymbol<ModAbiVersionFn>(kModAbiVersionSymbol);
  auto create_fn = library.GetSymbol<ModCreateFn>(kModCreateSymbol);
  if (!abi_version_fn || !create_fn) {
    REXSYS_ERROR("Mod '{}' is missing required exports {} / {}: {}", mod_name, kModAbiVersionSymbol,
                 kModCreateSymbol, path.string());
    return nullptr;
  }

  uint32_t plugin_abi = abi_version_fn();
  if (plugin_abi != kModPluginAbiVersion) {
    REXSYS_ERROR("Mod '{}' has ABI version {}, host expects {}: {}", mod_name, plugin_abi,
                 kModPluginAbiVersion, path.string());
    return nullptr;
  }

  ModHostContext populated_context = context;
  populated_context.struct_size = sizeof(ModHostContext);
  IModPlugin* plugin = create_fn(kModPluginAbiVersion, &populated_context);
  if (!plugin) {
    REXSYS_ERROR("Mod '{}' factory returned no plugin", mod_name);
    return nullptr;
  }

  LoadedModPlugins().push_back(std::move(library));
  REXSYS_INFO("Mod code plugin '{}' loaded ({})", mod_name, path.filename().string());
  return std::unique_ptr<IModPlugin>(plugin);
}

}  // namespace rex::system
