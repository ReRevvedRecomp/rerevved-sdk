#include <rex/system/mod_script.h>

#include <algorithm>
#include <exception>
#include <new>
#include <bit>
#include <rex/crypto/sha256.h>
#include <utility>

#include <rex/runtime.h>
#include <rex/system/mod_catalog.h>

namespace rex {
namespace {
void AppendWord(std::string& bytes, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    bytes.push_back(static_cast<char>((value >> shift) & 255));
}
void AppendString(std::string& bytes, std::string_view value) {
  AppendWord(bytes, value.size());
  bytes.append(value);
}
void AppendValue(std::string& bytes, const system::ModScriptValue& value) {
  using Type = system::ModScriptValue::Type;
  bytes.push_back(static_cast<char>(value.type));
  switch (value.type) {
    case Type::kBoolean:
      bytes.push_back(value.boolean ? 1 : 0);
      break;
    case Type::kInteger:
      AppendWord(bytes, static_cast<uint64_t>(value.integer));
      break;
    case Type::kNumber:
      AppendWord(bytes, std::bit_cast<uint64_t>(value.number));
      break;
    case Type::kString:
      AppendString(bytes, value.string);
      break;
    case Type::kTable:
      AppendWord(bytes, value.entries.size());
      for (const auto& entry : value.entries) {
        AppendValue(bytes, entry.key);
        AppendValue(bytes, entry.value);
      }
      break;
  }
}
}  // namespace

system::ModLoadoutApplyResult Runtime::ApplyModScriptLoadout(
    std::span<const system::ModPackage> packages, std::span<const std::string> ids, bool persist,
    bool replace_invalid_current) {
  using namespace system;
  ModLoadoutApplyResult failure{ModLoadoutApplyStatus::kInvalidDesired, {}};
  if (persist) {
    if (auto reason = mod_loadout_lock_reason()) {
      return {ModLoadoutApplyStatus::kLocked, {{0, std::move(*reason), {}}}};
    }
  }
  try {
    if (!mod_script_host_ || !mod_script_host_->supports_replacement()) {
      failure.diagnostics.push_back({0, "script host does not support live replacement", {}});
      return failure;
    }
    std::vector<ModScriptValue> values;
    std::vector<ModScriptPublishRequest> requests;
    values.reserve(packages.size());
    requests.reserve(packages.size());
    std::string canonical("rex.mod-script-loadout.v1");
    std::vector<std::string> script_ids;
    std::vector<ActiveModState> states;
    std::vector<ModPackage> info;
    std::vector<std::string> native_active;
    std::vector<std::string> native_desired;
    bool native_changed = false;
    auto catalog = mod_catalog_;
    auto failures = failed_mod_messages_;
    std::vector<std::string> desired_ids(ids.begin(), ids.end());
    for (const auto& state : active_mod_states_) {
      if (std::find(active_mod_script_ids_.begin(), active_mod_script_ids_.end(), state.id) !=
          active_mod_script_ids_.end())
        continue;
      states.push_back(state);
      native_active.push_back(state.id);
      const auto existing = std::find_if(active_mods_info_.begin(), active_mods_info_.end(),
                                         [&](const auto& mod) { return mod.id == state.id; });
      if (existing != active_mods_info_.end())
        info.push_back(*existing);
      else if (const auto* mod = catalog.Find(state.id))
        info.push_back(*mod);
    }
    for (auto& package : catalog.packages) {
      if ((package.payload_kind == ModPayloadKind::kLua &&
           std::find(native_active.begin(), native_active.end(), package.id) ==
               native_active.end()) ||
          std::find(active_mod_script_ids_.begin(), active_mod_script_ids_.end(), package.id) !=
              active_mod_script_ids_.end())
        catalog.SetActive(package.id, false);
      if (persist)
        catalog.SetDesired(package.id, false);
    }
    for (size_t order = 0; order < packages.size(); ++order) {
      const auto& package = packages[order];
      if (persist)
        catalog.SetDesired(package.id, true, order);
      if (package.payload_kind != ModPayloadKind::kLua) {
        const auto active = std::find_if(info.begin(), info.end(),
                                         [&](const auto& mod) { return mod.id == package.id; });
        native_changed |= active == info.end() || active->version != package.version ||
                          active->plugin_path != package.plugin_path;
        native_desired.push_back(package.id);
        continue;
      }
      if (std::find(native_active.begin(), native_active.end(), package.id) !=
          native_active.end()) {
        failure.diagnostics.push_back(
            {0, package.id + ": native payload replacement requires restart", {}});
        return failure;
      }
      auto decoded = RunModScriptFile(package.mod_root, package.entry);
      if (!decoded.ok) {
        failure.diagnostics.push_back({0,
                                       package.id + ": " + ToString(decoded.error.stage) + ": " +
                                           package.entry + ": " + decoded.error.message,
                                       package.entry_path});
        return failure;
      }
      values.push_back(std::move(decoded.value));
      requests.push_back({package.id, package.entry, package.api, package.api_version,
                          package.required_domains, values.back()});
      AppendString(canonical, package.id);
      AppendString(canonical, package.api);
      AppendWord(canonical, package.api_version);
      AppendWord(canonical, package.required_domains.size());
      for (const auto& domain : package.required_domains)
        AppendString(canonical, domain);
      AppendValue(canonical, values.back());
      script_ids.push_back(package.id);
      states.push_back({package.id, order});
      info.push_back(package);
      failures.erase(package.id);
      catalog.SetActive(package.id, true, order);
      if (auto* active = catalog.Find(package.id);
          active && active->status == ModPackageStatus::kLoadFailed) {
        active->status = ModPackageStatus::kReady;
        std::erase_if(active->diagnostics, [](const auto& diagnostic) {
          return !diagnostic.blocking && diagnostic.severity == ModDiagnosticSeverity::kError;
        });
      }
    }
    // All acknowledgement storage and the digest are ready before host publication.
    auto digest = crypto::sha256(canonical);
    ModScriptPublishResult prepared_result;
    auto replacement = mod_script_host_->PrepareReplacement(requests, prepared_result);
    if (!replacement) {
      failure.diagnostics.push_back(
          {0, std::string(ToString(prepared_result.stage)) + ": " + prepared_result.message, {}});
      return failure;
    }
    ModLoadoutApplyResult result{ModLoadoutApplyStatus::kSuccess, {}};
    if (persist) {
      // This check narrows the window between the entry check and the write but cannot close it.
      // The host must enforce the lock at publication.
      if (auto reason = mod_loadout_lock_reason()) {
        return {ModLoadoutApplyStatus::kLocked, {{0, std::move(*reason), {}}}};
      }
      result = system::ApplyModLoadout(user_data_root_, mod_catalog_, ids, replace_invalid_current);
      if (!result.succeeded())
        return result;
    }
    replacement->Commit();
    std::swap(mod_catalog_, catalog);
    active_mod_states_.swap(states);
    active_mods_info_.swap(info);
    failed_mod_messages_.swap(failures);
    active_mod_script_ids_.swap(script_ids);
    active_mod_script_digest_.swap(digest);
    if (persist) {
      mod_order_ids_.swap(desired_ids);
      mod_order_file_exists_ = true;
      mod_order_file_invalid_ = false;
      mod_loadout_diagnostics_.clear();
      restart_required_ = native_changed || native_active != native_desired;
    }
    // Notification follows commit, so observer failures cannot undo publication.
    if (mod_loadout_changed_callback_) {
      try {
        mod_loadout_changed_callback_();
      } catch (...) {}
    }
    return result;
  } catch (...) {
    failure.diagnostics.push_back({0, "loadout preparation failed; active rules unchanged", {}});
    return failure;
  }
}

system::ModScriptActivation Runtime::PrepareModScriptActivation(std::string_view id, size_t order) {
  active_mod_states_.reserve(active_mod_states_.size() + 1);
  return system::ModScriptActivation(*this, std::string(id), order);
}

namespace system {
ModScriptActivation::ModScriptActivation(Runtime& runtime, std::string id, size_t order)
    : runtime_(&runtime), id_(std::move(id)), order_(order) {}
ModScriptActivation::ModScriptActivation(ModScriptActivation&& other) noexcept
    : runtime_(std::exchange(other.runtime_, nullptr)),
      id_(std::move(other.id_)),
      order_(other.order_) {}

void ModScriptActivation::Commit() noexcept {
  if (!runtime_)
    return;
  auto& states = runtime_->active_mod_states_;
  runtime_->mod_catalog_.SetActive(id_, true, order_);
  runtime_->failed_mod_messages_.erase(id_);
  const auto existing = std::find_if(states.begin(), states.end(),
                                     [this](const auto& state) { return state.id == id_; });
  if (existing != states.end())
    existing->order = order_;
  else
    states.push_back({std::move(id_), order_});  // Capacity and owned ID were prepared.
  runtime_ = nullptr;
}

namespace {
// Reporting allocates; if it cannot, the package simply stays inactive and
// dispatch continues with later packages before the guest resumes.
template <typename MakeMessage>
void ReportFailure(Runtime& runtime, std::string_view id, MakeMessage&& make_message) noexcept {
  try {
    runtime.MarkModLoadFailed(id, make_message());
  } catch (...) {}
}
}  // namespace

void DispatchModLaunch(Runtime& runtime, std::span<const ModPackage> packages,
                       const std::function<void(const ModPackage&)>& launch_native) {
  if (runtime.mod_script_host() && runtime.mod_script_host()->supports_replacement()) {
    for (const auto& package : packages) {
      if (package.payload_kind != ModPayloadKind::kNative)
        continue;
      try {
        launch_native(package);
      } catch (...) {
        ReportFailure(runtime, package.id,
                      [] { return std::string("launch: native callback failed"); });
      }
    }
    auto result = runtime.ApplyModScriptLoadout(packages, runtime.mod_order_ids(), false);
    if (!result.succeeded()) {
      for (const auto& package : packages) {
        if (package.payload_kind != ModPayloadKind::kLua)
          continue;
        if (const auto* active = runtime.mod_catalog().Find(package.id); active && active->active)
          continue;
        ReportFailure(runtime, package.id, [&] {
          return result.diagnostics.empty() ? std::string("publish: loadout rejected")
                                            : result.diagnostics.front().message;
        });
      }
    }
    return;
  }
  for (size_t order = 0; order < packages.size(); ++order) {
    const auto& package = packages[order];
    const bool native = package.payload_kind == ModPayloadKind::kNative;
    try {
      if (native) {
        launch_native(package);
        continue;
      }
      auto* host = runtime.mod_script_host();
      if (!host) {
        ReportFailure(runtime, package.id,
                      [&] { return "support: " + package.entry + ": no script host"; });
        continue;
      }
      auto result = RunModScriptFile(package.mod_root, package.entry);
      if (!result.ok) {
        ReportFailure(runtime, package.id, [&] {
          return std::string(ToString(result.error.stage)) + ": " + package.entry + ": " +
                 result.error.message;
        });
        continue;
      }
      auto activation = runtime.PrepareModScriptActivation(package.id, order);
      auto published = host->Publish({package.id, package.entry, package.api, package.api_version,
                                      package.required_domains, result.value});
      if (published.published) {
        activation.Commit();
      } else {
        ReportFailure(runtime, package.id, [&] {
          return std::string(ToString(published.stage)) + ": " + package.entry + ": " +
                 published.message;
        });
      }
    } catch (const std::bad_alloc&) {
      ReportFailure(runtime, package.id, [&] {
        return native ? std::string("launch: out of memory")
                      : "memory: " + package.entry + ": activation preparation failed";
      });
    } catch (const std::exception& error) {
      ReportFailure(runtime, package.id, [&] {
        return (native ? std::string("launch: ") : "publish: " + package.entry + ": ") +
               error.what();
      });
    } catch (...) {
      ReportFailure(runtime, package.id, [&] {
        return std::string(native ? "launch" : "publish") + ": unknown package callback failure";
      });
    }
  }
}
}  // namespace system
}  // namespace rex
