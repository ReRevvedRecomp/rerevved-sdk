/**
 * @file        runtime/runtime.cpp
 * @brief       Runtime subsystem implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/null_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/ppc/context.h>          // PPCFuncMapping
#include <rex/platform/exceptions.h>  // SEH exception support
#include <rex/kernel/crt/heap.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>
#include <rex/system/asset_overlay_catalog.h>
#include <rex/system/asset_overlay_loadout.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_loadout.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "../kernel/xboxkrnl/video_routing.h"

REXCVAR_DEFINE_STRING(game_data_root, "", "Runtime", "Override game data path");
std::string& FLAGS_user_data_root_storage_() {
  static std::string storage;
  return storage;
}
static auto _cvar_reg_user_data_root =
    rex::cvar::FlagRegistrar({"user_data_root",
                              rex::cvar::FlagType::String,
                              "Runtime",
                              "Override user data path",
                              [](std::string_view v) {
                                FLAGS_user_data_root_storage_() = std::string(v);
                                return true;
                              },
                              []() { return FLAGS_user_data_root_storage_(); },
                              [](std::string_view) {},
                              rex::cvar::Lifecycle::kInitOnly,
                              {},
                              "",
                              false},
                             rex::cvar::Audience::kAdvanced, rex::cvar::Persistence::kSessionOnly);
REXCVAR_DEFINE_STRING(update_data_root, "", "Runtime", "Override update data path");
REXCVAR_DEFINE_STRING(cache_root, "", "Runtime", "Override shader cache path");
REXCVAR_DEFINE_STRING(metadata_root, "", "Runtime", "Override metadata path");
REXCVAR_DEFINE_STRING(mods_data_root, "", "Mods", "Mod folder; defaults to <executable>/mods");

namespace rex {

// Static instance for global access
Runtime* Runtime::instance_ = nullptr;

Runtime* Runtime::instance() {
  return instance_;
}

Runtime::Runtime(const std::filesystem::path& game_data_root,
                 const std::filesystem::path& user_data_root,
                 const std::filesystem::path& update_data_root,
                 const std::filesystem::path& cache_root,
                 const std::filesystem::path& metadata_root)
    : Runtime(game_data_root, user_data_root.empty() ? game_data_root : user_data_root,
              user_data_root.empty() ? game_data_root : user_data_root, update_data_root,
              cache_root, metadata_root) {}

Runtime::Runtime(const std::filesystem::path& game_data_root,
                 const std::filesystem::path& base_user_data_root,
                 const std::filesystem::path& active_user_data_root,
                 const std::filesystem::path& update_data_root,
                 const std::filesystem::path& cache_root,
                 const std::filesystem::path& metadata_root)
    : game_data_root_(game_data_root),
      base_user_data_root_(
          base_user_data_root.empty()
              ? (active_user_data_root.empty() ? game_data_root : active_user_data_root)
              : base_user_data_root),
      user_data_root_(active_user_data_root.empty()
                          ? (base_user_data_root.empty() ? game_data_root : base_user_data_root)
                          : active_user_data_root),
      update_data_root_(update_data_root),
      cache_root_(cache_root),
      metadata_root_(metadata_root) {}

Runtime::~Runtime() {
  Shutdown();
}

std::optional<std::filesystem::path> Runtime::FindMetadataPath(
    const std::filesystem::path& relative_path) const {
  if (!metadata_root_.empty()) {
    std::filesystem::path candidate = metadata_root_ / relative_path;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
    return std::nullopt;
  }

  const std::filesystem::path candidates[] = {
      game_data_root_ / "metadata" / relative_path,
      game_data_root_.parent_path() / "metadata" / relative_path,
      game_data_root_ / relative_path,
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::optional<EmbeddedMetadataAsset> Runtime::FindEmbeddedMetadata(
    const std::filesystem::path& relative_path) const {
  return FindEmbeddedMetadataAsset(relative_path);
}

X_STATUS Runtime::Setup(RuntimeConfig config) {
  if (instance_ != nullptr) {
    REXSYS_ERROR("Runtime::Setup() called but global instance already exists");
    return X_STATUS_UNSUCCESSFUL;
  }
  instance_ = this;

  auto fail = [this](X_STATUS status, std::string_view reason) {
    REXSYS_ERROR("Runtime::Setup failed: {}", reason);
    Shutdown();
    return status;
  };

  // Initialize SEH exception support for hardware exception handling
  rex::initialize_seh();

  // Initialize clock
  chrono::Clock::set_guest_tick_frequency(50000000);
  chrono::Clock::set_guest_system_time_base(chrono::Clock::QueryHostSystemTime());
  chrono::Clock::set_guest_time_scalar(1.0);

  // Enable threading affinity configuration
  thread::EnableAffinityConfiguration();

  tool_mode_ = config.tool_mode;
  game_version_ = std::move(config.game_version);
  // A delegate set before Setup must not end up routing exports away from graphics.
  if (detached_video_delegate() && (tool_mode_ || config.graphics)) {
    return fail(X_STATUS_NOT_SUPPORTED, "detached video delegate set with graphics or tool mode");
  }

  // Create memory system first
  memory_ = std::make_unique<memory::Memory>();
  if (!memory_->Initialize()) {
    return fail(X_STATUS_UNSUCCESSFUL, "memory init failed");
  }

  export_resolver_ = std::make_unique<runtime::ExportResolver>();

  function_dispatcher_ =
      std::make_unique<runtime::FunctionDispatcher>(memory_.get(), export_resolver_.get());

  // Create virtual file system
  file_system_ = std::make_unique<rex::filesystem::VirtualFileSystem>();

  // Create kernel state - this sets the global singleton
  kernel_state_ = std::make_unique<system::KernelState>(this);

  // Initialize input from injected config
  if (config.input_factory) {
    input_system_ = config.input_factory(tool_mode_);
    if (input_system_) {
      X_STATUS input_status = input_system_->Setup();
      if (XFAILED(input_status)) {
        REXSYS_WARN("Failed to initialize input system (status {:08X}) - input disabled",
                    input_status);
        input_system_.reset();
      } else {
        REXSYS_DEBUG("Input system initialized");
      }
    }
  }

  // HLE kernel modules and apps.
  if (config.kernel_init) {
    config.kernel_init(this, kernel_state_.get());
  }

  // Initialize the APU (Audio Processing Unit) from injected config
  if (config.audio_factory) {
    audio_system_ = config.audio_factory(function_dispatcher_.get());
    if (audio_system_) {
      X_STATUS audio_status = audio_system_->Setup(kernel_state_.get());
      if (XFAILED(audio_status)) {
        REXSYS_WARN("Failed to initialize audio system (status {:08X}) - audio disabled",
                    audio_status);
        audio_system_.reset();
      } else {
        REXSYS_DEBUG("Audio system initialized");
      }
    }
  }

  mod_script_host_ = std::move(config.mod_script_host);
  // Tool mode analyzes title binaries without starting the host mod lifecycle.
  if (!tool_mode_) {
    ResolveModLoadout();
    ResolveAssetOverlayLoadout();
  }

  // Set up VFS: game_data_root as game:/d:, update_data_root as update:
  if (!SetupVfs()) {
    return fail(X_STATUS_UNSUCCESSFUL, "VFS setup failed");
  }

  // Skip GPU initialization in tool mode (for analysis tools like codegen)
  if (tool_mode_) {
    REXSYS_DEBUG("Runtime initialized in tool mode (no GPU)");
    setup_complete_ = true;
    return X_STATUS_SUCCESS;
  }

  // Initialize GPU from injected config
  if (config.graphics) {
    graphics_system_ = std::move(config.graphics);
    bool with_presentation = (app_context_ != nullptr);
    X_STATUS gpu_status = graphics_system_->Setup(function_dispatcher_.get(), kernel_state_.get(),
                                                  app_context_, with_presentation);
    if (XFAILED(gpu_status)) {
      return fail(gpu_status, "GPU setup failed");
    }
    REXSYS_DEBUG("GPU system initialized (presentation={})", with_presentation);
  } else {
    REXSYS_DEBUG("Runtime initialized without graphics system (native rendering mode)");
  }

  REXSYS_DEBUG("Runtime initialized successfully");
  setup_complete_ = true;
  return X_STATUS_SUCCESS;
}

X_STATUS Runtime::Setup(const rex::PPCImageInfo& image_info, RuntimeConfig config) {
  X_STATUS status = Setup(std::move(config));
  if (status != X_STATUS_SUCCESS) {
    return status;
  }

  codegen_flags_ = image_info.codegen_flags;

  if (!function_dispatcher_->InitializeFunctionTable(image_info.code_base, image_info.code_size,
                                                     image_info.image_base, image_info.image_size,
                                                     /*is_entrypoint=*/true)) {
    REXSYS_ERROR("Failed to initialize function table");
    Shutdown();
    return X_STATUS_UNSUCCESSFUL;
  }

  if (image_info.func_mappings) {
    int count = 0;
    int duplicates = 0;
    int rejected = 0;
    for (int i = 0; image_info.func_mappings[i].guest != 0; ++i) {
      uint32_t guest = static_cast<uint32_t>(image_info.func_mappings[i].guest);
      auto* host = image_info.func_mappings[i].host;
      if (!host) {
        continue;
      }
      if (function_dispatcher_->GetFunction(guest)) {
        REXSYS_WARN("func_mappings: duplicate guest address {:08X}", guest);
        ++duplicates;
      }
      if (!function_dispatcher_->SetFunction(guest, host)) {
        ++rejected;
      } else {
        ++count;
      }
    }
    REXSYS_DEBUG("Registered {} recompiled functions ({} duplicates, {} rejected)", count,
                 duplicates, rejected);
    if (rejected > 0) {
      REXSYS_ERROR("PPCImageInfo registration: {} func_mappings entries rejected", rejected);
      Shutdown();
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  REXSYS_DEBUG("Runtime setup complete (code: {:08X}-{:08X}, image: {:08X}-{:08X})",
               image_info.code_base, image_info.code_base + image_info.code_size,
               image_info.image_base, image_info.image_base + image_info.image_size);

  const auto& flags = image_info.codegen_flags;
  REXSYS_DEBUG(
      "Codegen config: skip_lr={} skip_msr={} ctr={} xer={} cr={} reserved={} "
      "non_argument={} non_volatile={}",
      flags.skip_lr, flags.skip_msr, flags.ctr_as_local, flags.xer_as_local, flags.cr_as_local,
      flags.reserved_as_local, flags.non_argument_as_local, flags.non_volatile_as_local);

  return X_STATUS_SUCCESS;
}

void Runtime::Shutdown() {
  if (!instance_ && !setup_complete_ && !memory_) {
    return;
  }

  if (instance_ == this) {
    instance_ = nullptr;
  }

  if (graphics_system_) {
    graphics_system_->Shutdown();
    graphics_system_.reset();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
    audio_system_.reset();
  }
  if (input_system_) {
    input_system_->Shutdown();
    input_system_.reset();
  }
  // Clear before the kernel goes away; calls already in flight hold their own
  // reference, and the delegate is released outside the lock.
  SetDetachedVideoDelegate(nullptr);
  kernel_state_.reset();
  function_dispatcher_.reset();
  export_resolver_.reset();
  file_system_.reset();
  memory_.reset();
  mod_catalog_ = {};
  mod_script_host_.reset();
  active_mods_info_.clear();
  active_mod_states_.clear();
  failed_mod_messages_.clear();
  mod_order_file_exists_ = false;
  mod_order_ids_.clear();
  mod_loadout_diagnostics_.clear();
  mod_order_file_invalid_ = false;
  restart_required_ = false;
  asset_overlay_catalog_ = {};
  asset_order_file_exists_ = false;
  active_asset_overlays_.clear();
  asset_order_ids_.clear();
  asset_loadout_diagnostics_.clear();
  asset_order_file_invalid_ = false;
  asset_overlay_restart_required_ = false;
  game_version_.clear();

  rex::perf::Profiler::Shutdown();
  setup_complete_ = false;
}

X_STATUS Runtime::SetDetachedVideoDelegate(
    std::shared_ptr<system::IDetachedVideoDelegate> video_delegate) {
  std::shared_ptr<system::IDetachedVideoDelegate> released;
  X_STATUS status;
  {
    std::lock_guard<std::mutex> lock(detached_video_delegate_mutex_);
    status = kernel::xboxkrnl::CheckDetachedVideoDelegateChange(
        tool_mode_, graphics_system_ != nullptr, detached_video_delegate_ != nullptr,
        video_delegate != nullptr);
    if (XSUCCEEDED(status)) {
      released = std::exchange(detached_video_delegate_, std::move(video_delegate));
    }
  }
  // Logged after unlocking so a log sink may query the delegate.
  if (XFAILED(status)) {
    REXSYS_ERROR("Runtime::SetDetachedVideoDelegate rejected (status {:08X})", status);
  }
  return status;
}

std::shared_ptr<system::IDetachedVideoDelegate> Runtime::detached_video_delegate() const {
  std::lock_guard<std::mutex> lock(detached_video_delegate_mutex_);
  return detached_video_delegate_;
}

uint8_t* Runtime::virtual_membase() const {
  return memory_ ? memory_->virtual_membase() : nullptr;
}

namespace {

std::filesystem::path ResolvedModsRoot() {
  std::string configured_root = REXCVAR_GET(mods_data_root);
  return configured_root.empty()
             ? rex::filesystem::GetExecutableFolder() / "mods"
             : std::filesystem::absolute(std::filesystem::path(configured_root));
}

std::filesystem::path ResolvedAssetOverlaysRoot() {
  return rex::filesystem::GetExecutableFolder() / "asset-overrides";
}

}  // namespace

void Runtime::ResolveModLoadout() {
  const auto mods_root = ResolvedModsRoot();
  mod_catalog_ =
      system::DiscoverModCatalog(mods_root, game_version_, system::kModPluginAbiVersion,
                                 mod_script_host_ ? mod_script_host_->contracts()
                                                  : std::span<const system::ModScriptContract>{});

  const auto order_file = system::ReadModLoadout(user_data_root_);
  mod_order_file_exists_ = order_file.exists;
  const auto selection = system::SelectModLoadout(mod_catalog_, order_file);
  mod_loadout_diagnostics_ = selection.diagnostics;
  mod_order_file_invalid_ = mod_order_file_exists_ && !selection.IsValid();
  mod_order_ids_.clear();
  active_mods_info_.clear();
  for (const auto& package : mod_catalog_.packages) {
    mod_catalog_.SetDesired(package.id, false);
  }
  if (selection.IsValid()) {
    mod_order_ids_ = selection.requested_ids;
    active_mods_info_ = selection.packages;
    for (size_t order = 0; order < selection.requested_ids.size(); ++order) {
      mod_catalog_.SetDesired(selection.requested_ids[order], true, order);
    }
    return;
  }

  // A catalog or selection error is shown to F1 and blocks the complete
  // desired set. The game continues with no native plugins to load.
  for (const auto& diagnostic : mod_loadout_diagnostics_) {
    REXSYS_ERROR("Mod selection: {}", diagnostic.message);
  }
}

system::ModLoadoutSelection Runtime::ValidateModLoadout(std::span<const std::string> ids) const {
  return system::ValidateModLoadout(mod_catalog_, ids, user_data_root_ / system::kModOrderFileName);
}

system::ModLoadoutApplyResult Runtime::ApplyModLoadout(std::span<const std::string> ids,
                                                       bool replace_invalid_current) {
  auto result =
      system::ApplyModLoadout(user_data_root_, mod_catalog_, ids, replace_invalid_current);
  if (!result.succeeded()) {
    return result;
  }

  mod_order_file_exists_ = system::ReadModLoadout(user_data_root_).exists;
  mod_order_ids_.assign(ids.begin(), ids.end());
  mod_loadout_diagnostics_.clear();
  mod_order_file_invalid_ = false;
  for (const auto& package : mod_catalog_.packages) {
    mod_catalog_.SetDesired(package.id, false);
  }
  for (size_t order = 0; order < mod_order_ids_.size(); ++order) {
    mod_catalog_.SetDesired(mod_order_ids_[order], true, order);
  }
  restart_required_ = true;
  return result;
}

void Runtime::RescanModCatalog() {
  mod_catalog_ =
      system::DiscoverModCatalog(ResolvedModsRoot(), game_version_, system::kModPluginAbiVersion,
                                 mod_script_host_ ? mod_script_host_->contracts()
                                                  : std::span<const system::ModScriptContract>{});
  const auto order_file = system::ReadModLoadout(user_data_root_);
  mod_order_file_exists_ = order_file.exists;
  const auto persisted = system::SelectModLoadout(mod_catalog_, order_file);
  mod_loadout_diagnostics_ = persisted.diagnostics;
  mod_order_file_invalid_ = mod_order_file_exists_ && !persisted.IsValid();
  if (persisted.IsValid()) {
    mod_order_ids_ = persisted.requested_ids;
  } else {
    mod_order_ids_.clear();
  }
  for (size_t order = 0; order < mod_order_ids_.size(); ++order) {
    mod_catalog_.SetDesired(mod_order_ids_[order], true, order);
  }
  for (const auto& active_mod : active_mod_states_) {
    mod_catalog_.SetActive(active_mod.id, true, active_mod.order);
  }
  for (const auto& [id, message] : failed_mod_messages_) {
    mod_catalog_.MarkLoadFailed(id, message);
  }
}

void Runtime::ResolveAssetOverlayLoadout() {
  asset_overlay_catalog_ = system::DiscoverAssetOverlayCatalog(ResolvedAssetOverlaysRoot());
  const auto order_file =
      system::ReadAssetOverlayLoadout(user_data_root_, ResolvedAssetOverlaysRoot());
  asset_order_file_exists_ = order_file.exists;
  const auto selection = system::SelectAssetOverlayLoadout(asset_overlay_catalog_, order_file);
  asset_loadout_diagnostics_ = selection.diagnostics;
  asset_order_file_invalid_ = asset_order_file_exists_ && !selection.IsValid();
  asset_order_ids_.clear();
  active_asset_overlays_.clear();
  if (selection.IsValid()) {
    asset_order_ids_ = selection.requested_ids;
    active_asset_overlays_ = selection.packages;
    return;
  }
  for (const auto& diagnostic : asset_loadout_diagnostics_) {
    REXSYS_ERROR("Asset overlay selection: {}", diagnostic.message);
  }
}

system::AssetOverlayLoadoutSelection Runtime::ValidateAssetOverlayLoadout(
    std::span<const std::string> ids) const {
  return system::ValidateAssetOverlayLoadout(asset_overlay_catalog_, ids,
                                             user_data_root_ / system::kAssetOverlayOrderFileName);
}

system::AssetOverlayLoadoutApplyResult Runtime::ApplyAssetOverlayLoadout(
    std::span<const std::string> ids, bool replace_invalid_current) {
  auto result = system::ApplyAssetOverlayLoadout(user_data_root_, asset_overlay_catalog_, ids,
                                                 replace_invalid_current);
  if (!result.succeeded()) {
    return result;
  }
  asset_order_file_exists_ =
      system::ReadAssetOverlayLoadout(user_data_root_, ResolvedAssetOverlaysRoot()).exists;
  asset_order_ids_.assign(ids.begin(), ids.end());
  asset_loadout_diagnostics_.clear();
  asset_order_file_invalid_ = false;
  asset_overlay_restart_required_ = true;
  return result;
}

void Runtime::RescanAssetOverlayCatalog() {
  asset_overlay_catalog_ = system::DiscoverAssetOverlayCatalog(ResolvedAssetOverlaysRoot());
  const auto order_file =
      system::ReadAssetOverlayLoadout(user_data_root_, ResolvedAssetOverlaysRoot());
  asset_order_file_exists_ = order_file.exists;
  const auto persisted = system::SelectAssetOverlayLoadout(asset_overlay_catalog_, order_file);
  asset_loadout_diagnostics_ = persisted.diagnostics;
  asset_order_file_invalid_ = asset_order_file_exists_ && !persisted.IsValid();
  if (persisted.IsValid()) {
    asset_order_ids_ = persisted.requested_ids;
  } else {
    asset_order_ids_.clear();
  }
}

bool Runtime::SetupVfs() {
  if (game_data_root_.empty()) {
    REXSYS_WARN("Runtime::SetupVfs: No game_data_root specified, skipping VFS setup");
    return true;
  }

  auto abs_game_root = std::filesystem::absolute(game_data_root_);
  if (!std::filesystem::exists(abs_game_root)) {
    REXSYS_ERROR("Runtime::SetupVfs: game_data_root does not exist: {}", abs_game_root.string());
    return false;
  }

  // Mount game_data_root as \Device\Harddisk0\Partition1
  auto mount_path = "\\Device\\Harddisk0\\Partition1";
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(
      mount_path, abs_game_root, !REXCVAR_GET(allow_game_relative_writes));
  if (!device->Initialize()) {
    REXSYS_ERROR("Runtime::SetupVfs: Failed to initialize host path device");
    return false;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    REXSYS_ERROR("Runtime::SetupVfs: Failed to register host path device");
    return false;
  }
  REXSYS_DEBUG("  Mounted {} at {}", abs_game_root.string(), mount_path);

  // Register symbolic links for game: and D:
  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);
  REXSYS_DEBUG("  Registered symbolic links: game:, d:");

  // Mount update_data_root as update:\ if provided
  if (!update_data_root_.empty()) {
    auto abs_update_root = std::filesystem::absolute(update_data_root_);
    if (std::filesystem::exists(abs_update_root)) {
      auto update_mount = "\\Device\\Harddisk0\\PartitionUpdate";
      auto update_device =
          std::make_unique<rex::filesystem::HostPathDevice>(update_mount, abs_update_root, true);
      if (update_device->Initialize() && file_system_->RegisterDevice(std::move(update_device))) {
        file_system_->RegisterSymbolicLink("update:", update_mount);
        REXSYS_DEBUG("  Mounted {} at update:", abs_update_root.string());
      }
    }
  }

  // Setup NullDevice for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // Using a NullDevice returns success to all IO requests, allowing games
  // to believe cache/raw disk was accessed successfully.
  // NOTE: Must be registered AFTER Partition1 so Partition1 requests don't
  // go to NullDevice (VFS resolves devices in registration order)
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"), std::string("\\Cache1")};
  auto null_device =
      std::make_unique<rex::filesystem::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
    REXSYS_DEBUG("  Registered NullDevice for \\Device\\Harddisk0\\{{Partition0,Cache0,Cache1}}");
  }

  // NOTE: Do NOT register a device for cache: paths
  // Games handle "device not found" gracefully but don't handle actual device
  // errors (like NAME_COLLISION) well. Let cache: fail cleanly.

  return true;
}

X_STATUS Runtime::LoadXexImage(const std::string_view module_path) {
  REXSYS_DEBUG("Loading XEX image: {}", std::string(module_path));

  auto module = system::object_ref<system::UserModule>(new system::UserModule(kernel_state_.get()));
  X_STATUS status = module->LoadFromFile(module_path);
  if (XFAILED(status)) {
    REXSYS_ERROR("Runtime::LoadXexImage: Failed to load module, status {:08X}", status);
    return status;
  }

  kernel_state_->SetExecutableModule(module);
  REXSYS_DEBUG("  XEX image loaded successfully");
  return X_STATUS_SUCCESS;
}

system::object_ref<system::XThread> Runtime::PrepareModuleLaunch() {
  auto executable = kernel_state_->GetExecutableModule();
  if (!executable) {
    REXSYS_ERROR("Runtime::PrepareModuleLaunch: No executable module loaded");
    return nullptr;
  }

  auto thread = kernel_state_->PrepareModuleLaunch(executable);
  if (!thread) {
    REXSYS_ERROR("Runtime::PrepareModuleLaunch: Failed to prepare module");
    return nullptr;
  }

  REXSYS_DEBUG("  Module prepared on thread '{}'", thread->name());
  return thread;
}

system::object_ref<system::XThread> Runtime::LaunchModule() {
  auto thread = PrepareModuleLaunch();
  if (thread) {
    thread->Resume();
    REXSYS_DEBUG("  Module launched on thread '{}'", thread->name());
  }
  return thread;
}

}  // namespace rex
