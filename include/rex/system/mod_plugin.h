/**
 * @file        system/mod_plugin.h
 * @brief       Mod code-plugin ABI and host-side loader
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Generalizes the GPU plugin ABI so a mod folder can ship a
 *              native plugin that participates in the host lifecycle.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#if defined(_WIN32)
#define REX_MOD_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REX_MOD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace rex {
class Runtime;
namespace ui {
class WindowedAppContext;
class Window;
class ImGuiDrawer;
}  // namespace ui
namespace input {
class InputSystem;
}  // namespace input
}  // namespace rex

namespace rex::system {

inline constexpr uint32_t kModPluginAbiVersion = 1;
inline constexpr const char* kModCreateSymbol = "rex_mod_create";
inline constexpr const char* kModAbiVersionSymbol = "rex_mod_abi_version";

// Borrowed pointers remain valid for the plugin lifetime. Add fields only at
// the end and bump kModPluginAbiVersion whenever this contract changes.
struct ModHostContext {
  uint32_t struct_size = 0;
  Runtime* runtime = nullptr;
  ui::WindowedAppContext* app_context = nullptr;
  ui::Window* window = nullptr;
  input::InputSystem* input_system = nullptr;
  const char* mod_root = nullptr;
  const char* mod_name = nullptr;
};

class IModPlugin {
 public:
  virtual ~IModPlugin() = default;

  virtual void OnCreateDialogs(ui::ImGuiDrawer* drawer) { (void)drawer; }
  virtual void OnModuleLaunched() {}
  virtual void OnShutdown() {}
};

using ModAbiVersionFn = uint32_t (*)();
using ModCreateFn = IModPlugin* (*)(uint32_t abi_version, const ModHostContext* context);

// Loads a native plugin from code/<platform>/, with a legacy flat code/
// fallback. Failures are logged with the owning mod name. The loader's ABI and
// lifecycle are independent of catalog metadata.
std::unique_ptr<IModPlugin> LoadModPlugin(const std::filesystem::path& mod_root,
                                          std::string_view mod_name, std::string_view code_stem,
                                          const ModHostContext& context);

}  // namespace rex::system
