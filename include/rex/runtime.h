/**
 * @file        runtime.h
 * @brief       Runtime subsystem entry point
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 * @remarks     Based on Xenia emulator.h/cc
 */

#pragma once

#include <filesystem>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/embedded_metadata.h>
#include <rex/filesystem/vfs.h>
#include <rex/memory.h>
#include <rex/system/export_resolver.h>
#include <rex/system/asset_overlay_catalog.h>
#include <rex/system/asset_overlay_loadout.h>
#include <rex/system/interfaces/audio.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/interfaces/input.h>
#include <rex/system/interfaces/video_delegate.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_catalog.h>
#include <rex/system/mod_loadout.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/xobject.h>  // object_ref

// Forward declaration for function mapping (defined in rex/ppc/context.h)
struct PPCFuncMapping;

#include <rex/image_info.h>

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(std::string, update_data_root);
REXCVAR_DECLARE(std::string, cache_root);
REXCVAR_DECLARE(std::string, metadata_root);
REXCVAR_DECLARE(std::string, mods_data_root);

namespace rex {

// Forward declarations
namespace runtime {
class FunctionDispatcher;
class ExportResolver;
}  // namespace runtime
namespace system {
class KernelState;
class XThread;
}  // namespace system
namespace ui {
class WindowedAppContext;
class Window;
class ImGuiDrawer;
}  // namespace ui

/// Configuration for Runtime subsystem injection.
/// Graphics and audio backends are provided by the caller, keeping the runtime
/// library decoupled from concrete backend implementations.
/// Audio uses a factory because AudioSystem requires a FunctionDispatcher* at
/// construction time, which is only available during Setup().
struct RuntimeConfig {
  std::unique_ptr<system::IGraphicsSystem> graphics;
  // GPU emulation plugin loaded by ReXApp when `graphics` is empty
  // (e.g. "xenos"); empty means no GPU emulation.
  std::string gpu_plugin;
  std::function<std::unique_ptr<system::IAudioSystem>(runtime::FunctionDispatcher*)> audio_factory;
  std::function<std::unique_ptr<system::IInputSystem>(bool tool_mode)> input_factory;
  std::function<void(Runtime*, system::KernelState*)> kernel_init;
  bool tool_mode = false;
  std::string game_version;
  std::shared_ptr<system::IModScriptHost> mod_script_host;
};

/// Helper macros for populating RuntimeConfig with concrete backends.
/// Usage:
///   rex::RuntimeConfig config;
///   config.graphics      = REX_GRAPHICS_BACKEND(MyCustomGraphicsSystem);
///   config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
#define REX_GRAPHICS_BACKEND(Type) std::make_unique<Type>()
#define REX_AUDIO_BACKEND(Type)                                                                 \
  [](::rex::runtime::FunctionDispatcher* _fd) -> std::unique_ptr<::rex::system::IAudioSystem> { \
    return Type::Create(_fd);                                                                   \
  }
#define REX_INPUT_BACKEND(SetupFunc)                                    \
  [](bool _tool_mode) -> std::unique_ptr<::rex::system::IInputSystem> { \
    return SetupFunc(_tool_mode);                                       \
  }

/**
 * Runtime class - the main entry point for recompiled applications.
 *
 * Owns all subsystems:
 * - Memory: Virtual address space for guest code
 * - VFS: Virtual file system
 * - KernelState: Kernel objects, threading, etc.
 */
class Runtime {
 public:
  explicit Runtime(const std::filesystem::path& game_data_root,
                   const std::filesystem::path& user_data_root = {},
                   const std::filesystem::path& update_data_root = {},
                   const std::filesystem::path& cache_root = {},
                   const std::filesystem::path& metadata_root = {});
  // Constructs a runtime with distinct shared base (B) and active (P) roots
  // so marketplace content can be shared. The five-argument constructor uses
  // the same root for both.
  Runtime(const std::filesystem::path& game_data_root,
          const std::filesystem::path& base_user_data_root,
          const std::filesystem::path& active_user_data_root,
          const std::filesystem::path& update_data_root, const std::filesystem::path& cache_root,
          const std::filesystem::path& metadata_root);
  ~Runtime();

  // Non-copyable
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Global instance accessor - set after Setup() is called
  static Runtime* instance();

  // Subsystem accessors
  memory::Memory* memory() const { return memory_.get(); }
  rex::filesystem::VirtualFileSystem* file_system() const { return file_system_.get(); }
  system::KernelState* kernel_state() const { return kernel_state_.get(); }
  system::IGraphicsSystem* graphics_system() const { return graphics_system_.get(); }
  system::IAudioSystem* audio_system() const { return audio_system_.get(); }
  system::IInputSystem* input_system() const { return input_system_.get(); }

  // Installs the title's guest video services for a Runtime without graphics;
  // see IDetachedVideoDelegate. Call after Setup, never concurrently with
  // Setup or Shutdown. Returns X_STATUS_NOT_SUPPORTED when a graphics system
  // is set or in tool mode, and X_STATUS_OBJECT_NAME_COLLISION when replacing
  // one delegate with another; Setup fails the same way for a delegate set
  // earlier. nullptr always clears. Shutdown clears it before the kernel is
  // released.
  X_STATUS SetDetachedVideoDelegate(std::shared_ptr<system::IDetachedVideoDelegate> video_delegate);
  // Thread-safe copy; exports keep it alive for the duration of a call.
  std::shared_ptr<system::IDetachedVideoDelegate> detached_video_delegate() const;

  // FunctionDispatcher for guest function dispatch and interrupt execution
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_.get(); }
  // Export resolver - used for variable import resolution in guest memory
  runtime::ExportResolver* export_resolver() const { return export_resolver_.get(); }

  // Path accessors
  const std::filesystem::path& game_data_root() const { return game_data_root_; }
  const std::filesystem::path& base_user_data_root() const { return base_user_data_root_; }
  // The active profile root (P).
  const std::filesystem::path& user_data_root() const { return user_data_root_; }
  const std::filesystem::path& active_user_data_root() const { return user_data_root_; }
  const std::filesystem::path& update_data_root() const { return update_data_root_; }
  const std::filesystem::path& cache_root() const { return cache_root_; }
  const std::filesystem::path& metadata_root() const { return metadata_root_; }
  const std::string& game_version() const { return game_version_; }

  // The installed package catalog and the active native set selected from the
  // profile-local mod_order.txt. Native loading is performed by ReXApp.
  struct ActiveModState {
    std::string id;
    size_t order = 0;
  };

  const system::ModCatalog& mod_catalog() const { return mod_catalog_; }
  system::IModScriptHost* mod_script_host() const { return mod_script_host_.get(); }
  system::ModScriptActivation PrepareModScriptActivation(std::string_view id, size_t order);
  const std::vector<system::ModPackage>& active_mods_info() const { return active_mods_info_; }
  const std::vector<ActiveModState>& active_mod_states() const { return active_mod_states_; }
  const std::vector<std::string>& mod_order_ids() const { return mod_order_ids_; }
  const std::vector<system::ModLoadoutDiagnostic>& mod_loadout_diagnostics() const {
    return mod_loadout_diagnostics_;
  }
  bool mod_order_file_exists() const { return mod_order_file_exists_; }
  bool mod_order_file_invalid() const { return mod_order_file_invalid_; }
  bool restart_required() const { return restart_required_; }
  system::ModLoadoutSelection ValidateModLoadout(std::span<const std::string> ids) const;
  system::ModLoadoutApplyResult ApplyModLoadout(std::span<const std::string> ids,
                                                bool replace_invalid_current = false);
  void RescanModCatalog();
  void MarkModActive(std::string_view id, bool active, std::optional<size_t> order = std::nullopt) {
    mod_catalog_.SetActive(id, active, order);
    if (active) {
      const std::string key(id);
      failed_mod_messages_.erase(key);
      auto active_it =
          std::find_if(active_mod_states_.begin(), active_mod_states_.end(),
                       [&key](const ActiveModState& state) { return state.id == key; });
      if (active_it == active_mod_states_.end()) {
        active_mod_states_.push_back({key, order.value_or(active_mod_states_.size())});
      } else if (order) {
        active_it->order = *order;
      }
    } else {
      active_mod_states_.erase(
          std::remove_if(active_mod_states_.begin(), active_mod_states_.end(),
                         [id](const ActiveModState& state) { return state.id == id; }),
          active_mod_states_.end());
    }
  }
  void MarkModLoadFailed(std::string_view id, std::string message) {
    failed_mod_messages_[std::string(id)] = message;
    mod_catalog_.MarkLoadFailed(id, std::move(message));
    active_mod_states_.erase(
        std::remove_if(active_mod_states_.begin(), active_mod_states_.end(),
                       [id](const ActiveModState& state) { return state.id == id; }),
        active_mod_states_.end());
  }

  const system::AssetOverlayCatalog& asset_overlay_catalog() const {
    return asset_overlay_catalog_;
  }
  const std::vector<system::AssetOverlayPackage>& active_asset_overlays() const {
    return active_asset_overlays_;
  }
  const std::vector<std::string>& asset_order_ids() const { return asset_order_ids_; }
  const std::vector<system::AssetOverlayLoadoutDiagnostic>& asset_loadout_diagnostics() const {
    return asset_loadout_diagnostics_;
  }
  bool asset_order_file_exists() const { return asset_order_file_exists_; }
  bool asset_order_file_invalid() const { return asset_order_file_invalid_; }
  bool asset_overlay_restart_required() const { return asset_overlay_restart_required_; }
  system::AssetOverlayLoadoutSelection ValidateAssetOverlayLoadout(
      std::span<const std::string> ids) const;
  system::AssetOverlayLoadoutApplyResult ApplyAssetOverlayLoadout(
      std::span<const std::string> ids, bool replace_invalid_current = false);
  void RescanAssetOverlayCatalog();
  // Finds a metadata file or directory. An explicit metadata_root disables
  // legacy discovery; otherwise existing project layouts remain supported.
  std::optional<std::filesystem::path> FindMetadataPath(
      const std::filesystem::path& relative_path) const;
  std::optional<EmbeddedMetadataAsset> FindEmbeddedMetadata(
      const std::filesystem::path& relative_path) const;

  // Set the app context for presentation (call before Setup)
  void set_app_context(ui::WindowedAppContext* context) { app_context_ = context; }
  ui::WindowedAppContext* app_context() const { return app_context_; }

  // UI accessors for dialog system
  void set_display_window(ui::Window* window) { display_window_ = window; }
  ui::Window* display_window() const { return display_window_; }
  void set_imgui_drawer(ui::ImGuiDrawer* drawer) { imgui_drawer_ = drawer; }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_; }

  // Setup the runtime environment
  // config.tool_mode: If true, skips GPU initialization (for analysis tools)
  X_STATUS Setup(RuntimeConfig config = {});

  // rexglue - initializes per-module function dispatch table
  X_STATUS Setup(const PPCImageInfo& image_info, RuntimeConfig config = {});

  const PPCCodegenFlags& codegen_flags() const { return codegen_flags_; }

  // Check if running in tool mode (no GPU)
  bool is_tool_mode() const { return tool_mode_; }

  void Shutdown();

  // Load XEX image into guest memory
  X_STATUS LoadXexImage(const std::string_view module_path);

  // Launch XEX module and return main thread
  // Call after LoadXexImage to start execution
  system::object_ref<system::XThread> LaunchModule();

  // Prepare module launch: creates suspended main thread without resuming.
  // Call thread->Resume() after any pre-launch hooks.
  system::object_ref<system::XThread> PrepareModuleLaunch();

  // Access the memory base pointer for recompiled code
  uint8_t* virtual_membase() const;

 private:
  // Set up VFS: mounts game_data_root as game:/d:, update_data_root as update:
  bool SetupVfs();
  void ResolveModLoadout();
  void ResolveAssetOverlayLoadout();

  std::filesystem::path game_data_root_;
  std::filesystem::path base_user_data_root_;
  std::filesystem::path user_data_root_;
  std::filesystem::path update_data_root_;
  std::filesystem::path cache_root_;
  std::filesystem::path metadata_root_;
  std::string game_version_;
  system::ModCatalog mod_catalog_;
  friend class system::ModScriptActivation;
  std::shared_ptr<system::IModScriptHost> mod_script_host_;
  std::vector<system::ModPackage> active_mods_info_;
  std::vector<ActiveModState> active_mod_states_;
  std::unordered_map<std::string, std::string> failed_mod_messages_;
  std::vector<std::string> mod_order_ids_;
  std::vector<system::ModLoadoutDiagnostic> mod_loadout_diagnostics_;
  bool mod_order_file_exists_ = false;
  bool mod_order_file_invalid_ = false;
  bool restart_required_ = false;
  system::AssetOverlayCatalog asset_overlay_catalog_;
  std::vector<system::AssetOverlayPackage> active_asset_overlays_;
  std::vector<std::string> asset_order_ids_;
  std::vector<system::AssetOverlayLoadoutDiagnostic> asset_loadout_diagnostics_;
  bool asset_order_file_exists_ = false;
  bool asset_order_file_invalid_ = false;
  bool asset_overlay_restart_required_ = false;

  ui::WindowedAppContext* app_context_ = nullptr;
  ui::Window* display_window_ = nullptr;
  ui::ImGuiDrawer* imgui_drawer_ = nullptr;
  bool tool_mode_ = false;
  PPCCodegenFlags codegen_flags_{};
  bool setup_complete_ = false;

  std::unique_ptr<memory::Memory> memory_;
  std::unique_ptr<runtime::FunctionDispatcher> function_dispatcher_;
  std::unique_ptr<rex::filesystem::VirtualFileSystem> file_system_;
  std::unique_ptr<system::KernelState> kernel_state_;
  std::unique_ptr<system::IGraphicsSystem> graphics_system_;
  std::unique_ptr<system::IAudioSystem> audio_system_;
  std::unique_ptr<system::IInputSystem> input_system_;
  std::unique_ptr<runtime::ExportResolver> export_resolver_;
  mutable std::mutex detached_video_delegate_mutex_;
  std::shared_ptr<system::IDetachedVideoDelegate> detached_video_delegate_;

  static Runtime* instance_;
};

}  // namespace rex
