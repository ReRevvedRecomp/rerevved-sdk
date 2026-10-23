/**
 * @file        cvar.cpp
 * @brief       Configuration variable system implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <CLI/CLI.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform/env.h>

#include <toml++/toml.hpp>

namespace rex::cvar {

namespace {

std::string EscapeTomlString(std::string_view value);

// Invite codes carry reusable relay credentials, so their values never reach the log.
std::string_view LogValue(std::string_view name, std::string_view value) {
  return name == "system_link_invite" || name == "live_invite" ? "<redacted>" : value;
}

bool TomlNeedsQuotes(const FlagEntry& entry) {
  if (entry.type == FlagType::String)
    return true;
  if (entry.type != FlagType::Uint64)
    return false;
  const auto text = entry.getter();
  uint64_t value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value);
  // TOML integers are signed; the cvar loader also accepts decimal strings.
  return value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
}

bool g_finalized = false;
bool g_lifecycle_override = false;
std::mutex g_mutex;

// Set once cvar::Init has parsed the command line; later registrations are
// from runtime-loaded modules and drain pending values.
std::atomic<bool> g_init_done = false;

// Recursive: FlagRegistrar chain methods re-enter; change callbacks invoked
// from SetFlagByName must not mutate the registry.
std::recursive_mutex& GetRegistryMutex() {
  static std::recursive_mutex m;
  return m;
}

// Flag registry - use functions to avoid static init order issues
std::vector<FlagEntry>& GetRegistryStorage() {
  static std::vector<FlagEntry> registry;
  return registry;
}

std::unordered_map<std::string, size_t>& GetRegistryIndex() {
  static std::unordered_map<std::string, size_t> index;
  return index;
}

// FlagEntry exposes the effective default; retain compiled defaults so tests
// can restore a fresh application startup without changing the public layout.
std::unordered_map<std::string, std::string>& GetCompiledDefaultsStorage() {
  static std::unordered_map<std::string, std::string> defaults;
  return defaults;
}

Source DefaultSource(const FlagEntry& entry) {
  return GetCompiledDefaultsStorage().contains(entry.name) ? Source::kApplicationDefault
                                                           : Source::kDefault;
}

// Values that arrived before their cvar was registered; runtime-loaded
// modules register cvars long after Init/LoadConfig.
struct PendingValues {
  std::optional<std::string> cmdline;
  std::optional<std::string> config;
};

std::unordered_map<std::string, PendingValues>& GetPendingValuesStorage() {
  static std::unordered_map<std::string, PendingValues> pending;
  return pending;
}

// Convert flag name to environment variable: gpu_vsync -> REX_GPU_VSYNC
std::string FlagNameToEnvVar(std::string_view name) {
  std::string result = "REX_";
  for (char c : name) {
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

enum class ApplyResult { kApplied, kSkipped, kRejected };

ApplyResult SetFlagFromSource(std::string_view name, std::string_view value, Source source);
ApplyResult ApplyFlagFromSourceLocked(std::string_view name, std::string_view value, Source source);

int SourceRank(Source source) {
  switch (source) {
    case Source::kDefault:
      return 0;
    case Source::kApplicationDefault:
      return 1;
    case Source::kConfig:
      return 2;
    case Source::kEnvironment:
      return 3;
    case Source::kCommandLine:
      return 4;
    case Source::kRuntime:
      return 5;
  }
  return -1;
}

bool Outranks(Source source, const FlagEntry& entry) {
  return SourceRank(source) >= SourceRank(entry.source);
}

// Unvalidated apply, for the command line and environment paths.
bool ApplyFromSource(FlagEntry& entry, std::string_view value, Source source) {
  if (!Outranks(source, entry) || !entry.setter(value)) {
    return false;
  }
  entry.source = source;
  return true;
}

// Recursively apply TOML values
void ApplyTomlTable(const toml::table& table, const std::string& prefix) {
  for (const auto& [key, value] : table) {
    std::string full_key = prefix.empty() ? std::string(key) : prefix + "_" + std::string(key);

    if (value.is_table()) {
      ApplyTomlTable(*value.as_table(), full_key);
    } else {
      std::string value_str;
      if (value.is_boolean()) {
        value_str = value.as_boolean()->get() ? "true" : "false";
      } else if (value.is_integer()) {
        value_str = std::to_string(value.as_integer()->get());
      } else if (value.is_floating_point()) {
        value_str = std::to_string(value.as_floating_point()->get());
      } else if (value.is_string()) {
        value_str = value.as_string()->get();
      } else {
        REXLOG_WARN("Config: unsupported type for key '{}'", full_key);
        continue;
      }

      if (GetFlagInfo(full_key) == nullptr) {
        std::lock_guard lock(GetRegistryMutex());
        GetPendingValuesStorage()[full_key].config = value_str;
        REXLOG_DEBUG("Config: '{}' deferred (cvar not yet registered)", full_key);
        continue;
      }

      switch (SetFlagFromSource(full_key, value_str, Source::kConfig)) {
        case ApplyResult::kApplied:
          REXLOG_DEBUG("Config: {} = {}", full_key, LogValue(full_key, value_str));
          break;
        case ApplyResult::kSkipped:
          REXLOG_DEBUG("Config: {} ignored, already set by a higher-priority source", full_key);
          break;
        case ApplyResult::kRejected:
          REXLOG_WARN("Config: invalid value for cvar '{}'", full_key);
          break;
      }
    }
  }
}

// todo(tomc): move restart manager to Runtime
std::vector<std::string>& GetPendingRestartStorage() {
  static std::vector<std::string> pending;
  return pending;
}

// Callback storage for change notifications
std::unordered_map<std::string, std::vector<ChangeCallback>>& GetCallbackStorage() {
  static std::unordered_map<std::string, std::vector<ChangeCallback>> callbacks;
  return callbacks;
}

void MarkPendingRestart(std::string_view name) {
  auto& pending = GetPendingRestartStorage();
  std::string name_str(name);
  if (std::find(pending.begin(), pending.end(), name_str) == pending.end()) {
    pending.push_back(name_str);
  }
}

bool ValidateConstraints(const FlagEntry& entry, std::string_view value) {
  const auto& c = entry.constraints;

  // Range validation for numeric types
  if (c.HasRangeConstraint()) {
    double numeric_val = 0;
    if (entry.type == FlagType::String || entry.type == FlagType::Boolean) {
      // These types don't have numeric range constraints
    } else if (entry.type == FlagType::Double) {
      if (!ParseDouble(value, numeric_val))
        return false;
    } else {
      // Integer types
      int64_t int_val = 0;
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), int_val);
      if (ec != std::errc())
        return false;
      numeric_val = static_cast<double>(int_val);
    }

    if (c.min.has_value() && numeric_val < *c.min) {
      REXLOG_WARN("Flag '{}': value {} below min ({})", entry.name, LogValue(entry.name, value),
                  *c.min);
      return false;
    }
    if (c.max.has_value() && numeric_val > *c.max) {
      REXLOG_WARN("Flag '{}': value {} exceeds max ({})", entry.name, LogValue(entry.name, value),
                  *c.max);
      return false;
    }
  }

  // Allowed values validation
  if (c.HasAllowedValues()) {
    bool found = false;
    for (const auto& allowed : c.allowed_values) {
      if (allowed == value) {
        found = true;
        break;
      }
    }
    if (!found) {
      REXLOG_WARN("Flag '{}': '{}' not in allowed values", entry.name, LogValue(entry.name, value));
      return false;
    }
  }

  // Custom validator
  if (c.custom_validator && !c.custom_validator(value)) {
    REXLOG_WARN("Flag '{}': custom validation failed for '{}'", entry.name,
                LogValue(entry.name, value));
    return false;
  }

  return true;
}

}  // namespace

//=============================================================================
// Registry
//=============================================================================

std::vector<FlagEntry>& GetRegistry() {
  return GetRegistryStorage();
}

std::optional<size_t> RegisterFlag(FlagEntry entry) {
  // Legacy ABI callers treat the policy bytes as padding and may leave them
  // uninitialized. Supply explicit policies before replaying startup values.
  return RegisterFlag(std::move(entry), Audience::kAdvanced, Persistence::kConfig);
}

std::optional<size_t> RegisterFlag(FlagEntry entry, Audience audience, Persistence persistence) {
  std::lock_guard lock(GetRegistryMutex());
  (void)GetCallbackStorage();
  (void)GetPendingRestartStorage();
  (void)GetCompiledDefaultsStorage();
  entry.audience = audience;
  entry.persistence = persistence;
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  auto it = index.find(entry.name);
  if (it != index.end()) {
    REXLOG_ERROR("cvar: duplicate registration of '{}'; second registration ignored", entry.name);
    return std::nullopt;
  }
  size_t pos = storage.size();
  index[entry.name] = pos;
  storage.push_back(std::move(entry));

  // Late registration: replay pending values in ascending priority.
  if (g_init_done) {
    FlagEntry& stored = storage[pos];
    auto& pending = GetPendingValuesStorage();
    auto pending_it = pending.find(stored.name);
    if (pending_it != pending.end() && pending_it->second.config) {
      ApplyFlagFromSourceLocked(stored.name, *pending_it->second.config, Source::kConfig);
    }
    auto env_value = rex::platform::env::get(FlagNameToEnvVar(stored.name));
    if (env_value.has_value()) {
      ApplyFromSource(stored, *env_value, Source::kEnvironment);
    }
    if (pending_it != pending.end()) {
      if (pending_it->second.cmdline) {
        ApplyFromSource(stored, *pending_it->second.cmdline, Source::kCommandLine);
      }
      pending.erase(pending_it);
    }
  }
  return pos;
}

bool SetFlagAsApplicationDefault(std::string_view name, std::string_view value) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }
  return ApplyFlagFromSourceLocked(name, value, Source::kApplicationDefault) ==
         ApplyResult::kApplied;
}

void UnregisterFlag(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  std::string key(name);
  auto idx_it = index.find(key);
  if (idx_it == index.end()) {
    return;
  }
  size_t pos = idx_it->second;
  index.erase(idx_it);
  storage.erase(storage.begin() + pos);
  for (auto& [n, i] : index) {
    if (i > pos) {
      --i;
    }
  }
  GetCallbackStorage().erase(key);
  GetCompiledDefaultsStorage().erase(key);
  auto& pending = GetPendingRestartStorage();
  pending.erase(std::remove(pending.begin(), pending.end(), key), pending.end());
}

void FlagRegistrar::apply_(std::function<void(FlagEntry&)> fn) {
  if (owned_name_.empty()) {
    return;
  }
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto it = index.find(owned_name_);
  if (it == index.end()) {
    return;
  }
  fn(GetRegistryStorage()[it->second]);
}

namespace {

ApplyResult ApplyFlagFromSourceLocked(std::string_view name, std::string_view value,
                                      Source source) {
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return ApplyResult::kRejected;
  }

  auto& entry = GetRegistryStorage()[it->second];

  if (source == Source::kConfig && entry.persistence == Persistence::kSessionOnly) {
    REXLOG_DEBUG("Config: {} ignored, flag is session-only", name);
    return ApplyResult::kRejected;
  }

  if (!Outranks(source, entry)) {
    return ApplyResult::kSkipped;
  }

  if (!g_lifecycle_override && entry.lifecycle == Lifecycle::kInitOnly && IsFinalized()) {
    REXLOG_WARN("Cannot modify init-only flag '{}' after initialization", name);
    return ApplyResult::kRejected;
  }

  if (!ValidateConstraints(entry, value)) {
    return ApplyResult::kRejected;
  }

  if (!entry.setter(value)) {
    return ApplyResult::kRejected;
  }
  if (source == Source::kApplicationDefault) {
    GetCompiledDefaultsStorage().try_emplace(entry.name, entry.default_value);
    entry.default_value = entry.getter();
  }
  entry.source = source;

  if (entry.lifecycle == Lifecycle::kRequiresRestart) {
    MarkPendingRestart(name);
  }

  auto& callbacks = GetCallbackStorage();
  auto callback_it = callbacks.find(std::string(name));
  if (callback_it != callbacks.end()) {
    for (const auto& callback : callback_it->second) {
      callback(name, value);
    }
  }

  return ApplyResult::kApplied;
}

ApplyResult SetFlagFromSource(std::string_view name, std::string_view value, Source source) {
  std::lock_guard lock(GetRegistryMutex());
  return ApplyFlagFromSourceLocked(name, value, source);
}

}  // namespace

bool SetFlagByName(std::string_view name, std::string_view value) {
  return SetFlagFromSource(name, value, Source::kRuntime) == ApplyResult::kApplied;
}

bool SetFlagFromCommandLine(std::string_view name, std::string_view value) {
  return SetFlagFromSource(name, value, Source::kCommandLine) != ApplyResult::kRejected;
}

bool InvokeCommand(std::string_view name, std::string_view args) {
  std::function<void(std::string_view)> cb;
  {
    std::lock_guard lock(GetRegistryMutex());
    auto it = GetRegistryIndex().find(std::string(name));
    if (it == GetRegistryIndex().end()) {
      return false;
    }
    const auto& entry = GetRegistryStorage()[it->second];
    if (entry.type != FlagType::Command) {
      return false;
    }
    // Copy the callback out from under the lock; GetFlagInfo pointers are
    // invalidated by registry mutation and a command may touch the registry.
    cb = entry.command_callback;
  }
  if (!cb) {
    return false;
  }
  cb(args);
  return true;
}

std::string GetFlagByName(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return "";
  }

  return GetRegistryStorage()[it->second].getter();
}

Source GetFlagSource(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return Source::kDefault;
  }
  return GetRegistryStorage()[it->second].source;
}

std::vector<std::string> ListFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  result.reserve(GetRegistryStorage().size());
  for (const auto& entry : GetRegistryStorage()) {
    result.push_back(entry.name);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByCategory(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByLifecycle(Lifecycle lc) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.lifecycle == lc) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

const FlagEntry* GetFlagInfo(std::string_view name) {
  // Pointer is invalidated by any subsequent registry call.
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return nullptr;
  }
  return &GetRegistryStorage()[it->second];
}

template <>
bool Query<bool>(std::string_view name) {
  std::string v = GetFlagByName(name);
  return rex::string::from_string<bool>(v, false);
}

template <>
int32_t Query<int32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
int64_t Query<int64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint32_t Query<uint32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint64_t Query<uint64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
double Query<double>(std::string_view name) {
  std::string v = GetFlagByName(name);
  double out = 0.0;
  ParseDouble(v, out);
  return out;
}

template <>
std::string Query<std::string>(std::string_view name) {
  return GetFlagByName(name);
}

std::vector<std::string> GetPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  return GetPendingRestartStorage();
}

void ClearPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  GetPendingRestartStorage().clear();
}

void ResetToDefault(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return;
  }
  auto& entry = GetRegistryStorage()[it->second];
  entry.setter(entry.default_value);
  entry.source = DefaultSource(entry);
}

void ResetAllToDefaults() {
  std::lock_guard lock(GetRegistryMutex());
  for (auto& entry : GetRegistryStorage()) {
    entry.setter(entry.default_value);
    entry.source = DefaultSource(entry);
  }
}

bool HasNonDefaultValue(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }
  const auto& entry = GetRegistryStorage()[it->second];
  return entry.getter() != entry.default_value;
}

std::vector<std::string> ListModifiedFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.getter() != entry.default_value) {
      result.push_back(entry.name);
    }
  }
  return result;
}

std::string SerializeToTOML() {
  std::lock_guard lock(GetRegistryMutex());
  std::string result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.persistence == Persistence::kConfig && entry.getter() != entry.default_value) {
      if (TomlNeedsQuotes(entry)) {
        result += entry.name + " = \"" + EscapeTomlString(entry.getter()) + "\"\n";
      } else {
        result += entry.name + " = " + entry.getter() + "\n";
      }
    }
  }
  return result;
}

std::string SerializeToTOML(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::string result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category && entry.persistence == Persistence::kConfig &&
        entry.getter() != entry.default_value) {
      if (TomlNeedsQuotes(entry)) {
        result += entry.name + " = \"" + EscapeTomlString(entry.getter()) + "\"\n";
      } else {
        result += entry.name + " = " + entry.getter() + "\n";
      }
    }
  }
  return result;
}

void RegisterChangeCallback(std::string_view name, ChangeCallback callback) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage()[std::string(name)].push_back(std::move(callback));
}

void UnregisterChangeCallbacks(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage().erase(std::string(name));
}

//=============================================================================
// Initialization
//=============================================================================

std::vector<std::string> Init(int argc, char** argv) {
  CLI::App app{"", ""};
  app.allow_extras();

  for (auto& entry : GetRegistryStorage()) {
    if (entry.type == FlagType::Boolean) {
      app.add_flag_function(
          "--" + entry.name + ",!--no-" + entry.name,
          [&entry](int64_t count) {
            ApplyFromSource(entry, count > 0 ? "true" : "false", Source::kCommandLine);
          },
          entry.description);
    } else {
      app.add_option_function<std::string>(
          "--" + entry.name,
          [&entry](const std::string& val) { ApplyFromSource(entry, val, Source::kCommandLine); },
          entry.description);
    }
  }

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    // TODO(tomc): dumb workaround for the stupid chicken and its egg.
    //             dont call rex logging funcs here for now.
    fprintf(stderr, "cvar: CLI11  parse error: %s\n", e.what());
  }

  // Stash unrecognized --options for cvars that register later. Supported
  // forms: --name=value, --name (true), --no-name (false); a separated
  // "--name value" pair is ambiguous with a positional, so never consumed.
  std::vector<std::string> positional;
  for (const auto& arg : app.remaining()) {
    std::string_view view(arg);
    if (!view.starts_with("--")) {
      positional.push_back(arg);
      continue;
    }
    view.remove_prefix(2);
    std::string name;
    std::string value = "true";
    if (auto eq = view.find('='); eq != std::string_view::npos) {
      name.assign(view.substr(0, eq));
      value.assign(view.substr(eq + 1));
    } else if (view.starts_with("no-")) {
      name.assign(view.substr(3));
      value = "false";
    } else {
      name.assign(view);
    }
    std::lock_guard lock(GetRegistryMutex());
    GetPendingValuesStorage()[name].cmdline = std::move(value);
  }
  g_init_done = true;
  return positional;
}

void LoadConfig(const std::filesystem::path& config_path) {
  if (!std::filesystem::exists(config_path)) {
    REXLOG_DEBUG("Config file not found: {}", config_path.string());
    return;
  }

  try {
    auto config = toml::parse_file(config_path.string());
    ApplyTomlTable(config, "");
    REXLOG_DEBUG("Loaded config from {}", config_path.string());
  } catch (const toml::parse_error& err) {
    std::string description(err.description());
    // toml++ parser.inl quotes source tokens in these diagnostic forms.
    if (const auto saw = description.find("saw '"); saw != std::string::npos) {
      description.replace(saw + 4, description.size() - saw - 4, "<source text redacted>");
    } else if (description.find("unknown escape sequence '") != std::string::npos ||
               description.find("cannot insert '") != std::string::npos ||
               description.find("cannot redefine existing ") != std::string::npos) {
      const auto first = description.find('\'');
      if (first != std::string::npos)
        description.replace(first, description.size() - first, "<source text redacted>");
    }
    REXLOG_ERROR("Failed to parse config {} at line {}: {}", config_path.string(),
                 err.source().begin.line, description);
  }
}

void ApplyEnvironment() {
  int count = 0;
  for (auto& entry : GetRegistryStorage()) {
    std::string env_name = FlagNameToEnvVar(entry.name);
    auto env_value = rex::platform::env::get(env_name);
    if (!env_value.has_value()) {
      continue;
    }
    if (!Outranks(Source::kEnvironment, entry)) {
      REXLOG_DEBUG("Env: {} ignored, already set on the command line", entry.name);
      continue;
    }
    if (ApplyFromSource(entry, *env_value, Source::kEnvironment)) {
      REXLOG_DEBUG("Env: {} = {} (from {})", entry.name, LogValue(entry.name, *env_value),
                   env_name);
      ++count;
    } else {
      REXLOG_WARN("Env: failed to parse {} = {}", env_name, LogValue(entry.name, *env_value));
    }
  }

  if (count > 0) {
    REXLOG_INFO("Applied {} environment variable override(s)", count);
  }
}

void FinalizeInit() {
  std::lock_guard lock(g_mutex);
  g_finalized = true;
  for (const auto& [name, values] : GetPendingValuesStorage()) {
    (void)values;
    REXLOG_WARN("Config: unknown cvar '{}'", name);
  }
  REXLOG_DEBUG("cvar: initialization finalized");
}

bool IsFinalized() {
  return g_finalized;
}

void SaveConfig(const std::filesystem::path& config_path) {
  std::string content = SerializeToTOML();
  if (content.empty()) {
    REXLOG_DEBUG("SaveConfig: no modified flags to save");
    return;
  }

  try {
    std::ofstream file(config_path);
    if (!file) {
      REXLOG_ERROR("SaveConfig: failed to open {}", config_path.string());
      return;
    }
    file << "# Auto-generated cvar configuration\n";
    file << content;
    REXLOG_INFO("Saved config to {}", config_path.string());
  } catch (const std::exception& e) {
    REXLOG_ERROR("SaveConfig: {}", e.what());
  }
}

namespace {

// Escapes a value for use inside a TOML basic (double-quoted) string.
std::string EscapeTomlString(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (char c : value) {
    switch (c) {
      case '\\':
        result += "\\\\";
        break;
      case '"':
        result += "\\\"";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
          constexpr char hex[] = "0123456789ABCDEF";
          result += "\\u00";
          result += hex[(static_cast<unsigned char>(c) >> 4) & 15];
          result += hex[static_cast<unsigned char>(c) & 15];
        } else {
          result += c;
        }
        break;
    }
  }
  return result;
}

// Parses a bare, quoted or dotted TOML key into its segments. Quoted
// segments with escapes are not supported and yield nullopt.
std::optional<std::vector<std::string>> ParseTomlKeyPath(std::string_view text) {
  std::vector<std::string> parts;
  size_t i = 0;
  auto skip_space = [&] {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
      ++i;
    }
  };
  while (true) {
    skip_space();
    if (i >= text.size()) {
      return std::nullopt;
    }
    if (text[i] == '"' || text[i] == '\'') {
      const char quote = text[i++];
      const size_t close = text.find(quote, i);
      if (close == std::string_view::npos) {
        return std::nullopt;
      }
      std::string part(text.substr(i, close - i));
      if (quote == '"' && part.find('\\') != std::string::npos) {
        return std::nullopt;
      }
      parts.push_back(std::move(part));
      i = close + 1;
    } else {
      const size_t start = i;
      while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) ||
                                 text[i] == '_' || text[i] == '-')) {
        ++i;
      }
      if (i == start) {
        return std::nullopt;
      }
      parts.emplace_back(text.substr(start, i - start));
    }
    skip_space();
    if (i == text.size()) {
      return parts;
    }
    if (text[i] != '.') {
      return std::nullopt;
    }
    ++i;
  }
}

// Flattens a key path the way ApplyTomlTable names cvars.
std::string JoinTomlKeyPath(std::string prefix, const std::vector<std::string>& parts) {
  for (const auto& part : parts) {
    prefix = prefix.empty() ? part : prefix + "_" + part;
  }
  return prefix;
}

// Returns the position of the first `close` outside quotes, or npos.
size_t FindTomlUnquoted(std::string_view line, size_t from, char close) {
  char quote = 0;
  for (size_t i = from; i < line.size(); ++i) {
    const char c = line[i];
    if (quote) {
      if (c == quote) {
        quote = 0;
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == close) {
      return i;
    } else if (c == '#') {
      return std::string_view::npos;
    }
  }
  return std::string_view::npos;
}

struct TomlKeyLine {
  std::string text;  // key as written, for rewriting the line
  std::vector<std::string> path;
};

// Returns the key of a `key = value` line, or nullopt if the line is blank,
// a comment, a table header, or otherwise not a recognizable assignment.
std::optional<TomlKeyLine> ExtractTomlKey(const std::string& line) {
  const auto first = line.find_first_not_of(" \t");
  if (first == std::string::npos || line[first] == '#' || line[first] == '[') {
    return std::nullopt;
  }
  const auto eq = FindTomlUnquoted(line, first, '=');
  if (eq == std::string::npos || eq == first) {
    return std::nullopt;
  }
  const auto last = line.find_last_not_of(" \t", eq - 1);
  std::string text = line.substr(first, last - first + 1);
  auto path = ParseTomlKeyPath(text);
  if (!path) {
    return std::nullopt;
  }
  return TomlKeyLine{std::move(text), std::move(*path)};
}

struct TomlHeader {
  std::vector<std::string> path;
  bool array = false;  // `[[name]]`
};

// Parses a `[table]` or `[[array]]` header line, or returns nullopt.
std::optional<TomlHeader> ParseTomlHeader(const std::string& line) {
  const auto first = line.find_first_not_of(" \t");
  if (first == std::string::npos || line[first] != '[') {
    return std::nullopt;
  }
  const bool array = line.compare(first, 2, "[[") == 0;
  const size_t open = first + (array ? 2 : 1);
  const auto close = FindTomlUnquoted(line, open, ']');
  if (close == std::string::npos) {
    return std::nullopt;
  }
  auto path = ParseTomlKeyPath(std::string_view(line).substr(open, close - open));
  if (!path) {
    return std::nullopt;
  }
  return TomlHeader{std::move(*path), array};
}

// Finds the start of a trailing `#` comment, ignoring `#` inside quoted
// strings. Returns npos if there is no trailing comment.
size_t FindTomlCommentStart(const std::string& line) {
  bool in_string = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
      in_string = !in_string;
    } else if (c == '#' && !in_string) {
      return i;
    }
  }
  return std::string::npos;
}

// Track arrays and multiline strings so continuation lines cannot become headers.
struct TomlLineState {
  size_t depth = 0;
  char quote = 0;
  bool multiline = false;
  bool AtRoot() const { return depth == 0 && !quote; }
  void Scan(std::string_view line) {
    for (size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (quote) {
        if (quote == '"' && c == '\\') {
          ++i;
        } else if (c == quote) {
          if (!multiline) {
            quote = 0;
          } else if (i + 2 < line.size() && line[i + 1] == quote && line[i + 2] == quote) {
            quote = 0;
            i += 2;
            // Up to two more quotes belong to the content before the delimiter.
            for (int extra = 0; extra < 2 && i + 1 < line.size() && line[i + 1] == c; ++extra) {
              ++i;
            }
          }
        }
      } else if (c == '#') {
        break;
      } else if (c == '"' || c == '\'') {
        quote = c;
        multiline = i + 2 < line.size() && line[i + 1] == c && line[i + 2] == c;
        if (multiline)
          i += 2;
      } else if (c == '[') {
        ++depth;
      } else if (c == ']' && depth) {
        --depth;
      }
    }
  }
};

}  // namespace

// Source: birabittoh/rexglue-sdk tag sotn-nightly-20260817-7766f971, ported
// onto the v0.9.0 baseline together with GameDataSelector, which persists the
// resolved game data root through it.
namespace {

bool SameTomlNode(const toml::node& a, const toml::node& b) {
  return toml::node_view<const toml::node>(&a) == toml::node_view<const toml::node>(&b);
}

// True when `after` differs from `before` only in target cvar values, using
// ApplyTomlTable's flattening. Targets may be added at the root but may not
// replace a table or array.
bool SameExceptTargets(const toml::table& before, const toml::table& after,
                       const std::string& prefix, const toml::table& expected,
                       std::unordered_set<std::string>& found) {
  for (const auto& [key, node] : after) {
    const std::string name =
        prefix.empty() ? std::string(key.str()) : prefix + "_" + std::string(key.str());
    const toml::node* old_node = before.get(key.str());
    if (const auto* table = node.as_table()) {
      const auto* old_table = old_node ? old_node->as_table() : nullptr;
      if (!old_table || !SameExceptTargets(*old_table, *table, name, expected, found)) {
        return false;
      }
    } else if (const toml::node* want = expected.get(name)) {
      if (old_node ? !old_node->is_value() : !prefix.empty()) {
        return false;
      }
      if (!node.is_value() || !SameTomlNode(node, *want)) {
        return false;
      }
      found.insert(name);
    } else if (!old_node || !SameTomlNode(*old_node, node)) {
      return false;
    }
  }
  for (const auto& [key, node] : before) {
    (void)node;
    if (!after.contains(key.str())) {
      return false;
    }
  }
  return true;
}

// Refuses a rewrite that would not load back as intended, such as one that
// meets inline tables, escaped keys or duplicate spellings of a cvar. Every
// `required` cvar must be visible to the loader, outside any array.
bool VerifyConfigRewrite(const std::string& before, const std::string& after,
                         const std::string& expected, const std::vector<std::string>& required) {
  try {
    const auto before_table = toml::parse(before);
    const auto after_table = toml::parse(after);
    const auto expected_table = toml::parse(expected);
    std::unordered_set<std::string> found;
    if (!SameExceptTargets(before_table, after_table, {}, expected_table, found)) {
      return false;
    }
    return std::all_of(required.begin(), required.end(),
                       [&found](const std::string& name) { return found.contains(name); });
  } catch (const toml::parse_error&) {
    return false;
  }
}

// Rewrites the named flags' lines and keeps every other line. Missing named
// flags are appended, or only non-default ones when append_only_modified.
// Replaces the file atomically through a sibling temporary file.
bool WriteConfigSubsetLocked(const std::filesystem::path& config_path,
                             const std::vector<std::string>& names, bool append_only_modified) {
  std::unordered_map<std::string, const FlagEntry*> all_entries;
  for (const auto& entry : GetRegistryStorage()) {
    all_entries[entry.name] = &entry;
  }

  std::unordered_map<std::string, const FlagEntry*> to_write;
  for (const auto& name : names) {
    auto it = all_entries.find(name);
    if (it == all_entries.end()) {
      continue;
    }
    if (it->second->persistence == Persistence::kConfig) {
      to_write[name] = it->second;
    }
  }

  auto format_value = [](const FlagEntry& entry) {
    if (TomlNeedsQuotes(entry)) {
      return "\"" + EscapeTomlString(entry.getter()) + "\"";
    }
    return entry.getter();
  };

  // Preserve other cvars verbatim; they may belong to another config surface.
  std::filesystem::path temp_path;
  auto discard_temp = [&temp_path] {
    if (!temp_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(temp_path, ignored);
    }
  };
  try {
    std::vector<std::string> result;
    size_t root_end = 0;
    bool in_table = false;
    std::optional<std::string> table_prefix = std::string();
    TomlLineState state;
    std::string original_text;
    std::unordered_set<std::string> seen;
    std::vector<std::vector<std::string>> array_paths;
    if (std::filesystem::exists(config_path)) {
      // Binary, so a 0x1A byte cannot end the read early on Windows.
      std::ifstream in(config_path, std::ios::binary);
      if (!in) {
        REXLOG_ERROR("SaveConfigSubset: failed to read {}", config_path.string());
        return false;
      }
      std::string line;
      bool skipping_value = false;
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        original_text += line;
        original_text += '\n';
        if (skipping_value) {
          // Drop the rest of a rewritten multiline value.
          state.Scan(line);
          skipping_value = !state.AtRoot();
          continue;
        }
        const bool at_root = state.AtRoot();
        const auto first = line.find_first_not_of(" \t\r");
        if (at_root && first != std::string::npos && line[first] == '[') {
          if (!in_table) {
            root_end = result.size();
            in_table = true;
          }
          // Keys under an array table, or a table nested in one, are not cvars.
          const auto header = ParseTomlHeader(line);
          const auto under_array = [&header](const std::vector<std::string>& array_path) {
            return header->path.size() >= array_path.size() &&
                   std::equal(array_path.begin(), array_path.end(), header->path.begin());
          };
          table_prefix.reset();
          if (header && header->array) {
            array_paths.push_back(header->path);
          } else if (header && std::none_of(array_paths.begin(), array_paths.end(), under_array)) {
            table_prefix = JoinTomlKeyPath({}, header->path);
          }
          state.Scan(line);
          result.push_back(line);
          continue;
        }
        auto key = at_root ? ExtractTomlKey(line) : std::nullopt;
        state.Scan(line);
        if (!key || !table_prefix) {
          result.push_back(line);
          continue;
        }

        auto entry_it = to_write.find(JoinTomlKeyPath(*table_prefix, key->path));
        if (entry_it != to_write.end()) {
          seen.insert(entry_it->first);
        }
        if (entry_it == to_write.end()) {
          result.push_back(line);
          continue;
        }

        std::string new_line =
            line.substr(0, first) + key->text + " = " + format_value(*entry_it->second);
        if (state.AtRoot()) {
          size_t comment_start = FindTomlCommentStart(line);
          if (comment_start != std::string::npos) {
            new_line += "  " + line.substr(comment_start);
          }
        } else {
          skipping_value = true;
        }
        result.push_back(std::move(new_line));
      }
      if (in.bad()) {
        REXLOG_ERROR("SaveConfigSubset: failed to read {}", config_path.string());
        return false;
      }
    } else {
      result.push_back("# Auto-generated cvar configuration");
    }

    if (!in_table)
      root_end = result.size();
    std::vector<std::string> added;
    std::vector<std::string> required;
    std::string expected_text;
    for (const auto& name : names) {
      auto it = to_write.find(name);
      if (it == to_write.end()) {
        continue;
      }
      const std::string assignment = name + " = " + format_value(*it->second);
      expected_text += assignment + "\n";
      if (!append_only_modified || it->second->getter() != it->second->default_value) {
        required.push_back(name);
        if (seen.insert(name).second) {
          added.push_back(assignment);
        }
      }
    }
    result.insert(result.begin() + root_end, added.begin(), added.end());

    std::string updated_text;
    for (const auto& line : result) {
      updated_text += line;
      updated_text += '\n';
    }
    if (!VerifyConfigRewrite(original_text, updated_text, expected_text, required)) {
      REXLOG_ERROR("SaveConfigSubset: {} has a layout that cannot be updated safely",
                   config_path.string());
      return false;
    }

    if (config_path.has_parent_path()) {
      std::filesystem::create_directories(config_path.parent_path());
    }
    temp_path = config_path;
    temp_path += ".tmp";
    {
      std::ofstream file(temp_path, std::ios::trunc);
      if (!file) {
        REXLOG_ERROR("SaveConfigSubset: failed to open {}", temp_path.string());
        discard_temp();
        return false;
      }
      file << updated_text;
      file.close();
      if (!file) {
        REXLOG_ERROR("SaveConfigSubset: failed to write {}", temp_path.string());
        discard_temp();
        return false;
      }
    }
    std::filesystem::rename(temp_path, config_path);
    REXLOG_INFO("Saved config subset to {}", config_path.string());
    return true;
  } catch (const std::exception& e) {
    REXLOG_ERROR("SaveConfigSubset: {}", e.what());
    discard_temp();
    return false;
  }
}

}  // namespace

void SaveConfigSubset(const std::filesystem::path& config_path,
                      const std::vector<std::string>& names) {
  std::lock_guard lock(GetRegistryMutex());
  WriteConfigSubsetLocked(config_path, names, false);
}

bool TrySaveConfig(const std::filesystem::path& config_path) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> names;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.persistence == Persistence::kConfig && entry.type != FlagType::Command &&
        entry.source != Source::kCommandLine && entry.source != Source::kEnvironment) {
      names.push_back(entry.name);
    }
  }
  return WriteConfigSubsetLocked(config_path, names, true);
}

namespace testing {

ScopedLifecycleOverride::ScopedLifecycleOverride() {
  g_lifecycle_override = true;
}

ScopedLifecycleOverride::~ScopedLifecycleOverride() {
  g_lifecycle_override = false;
}

void ResetAllForTesting() {
  std::lock_guard lock(GetRegistryMutex());
  auto& compiled_defaults = GetCompiledDefaultsStorage();
  for (auto& entry : GetRegistryStorage()) {
    if (auto it = compiled_defaults.find(entry.name); it != compiled_defaults.end()) {
      entry.default_value = it->second;
    }
  }
  compiled_defaults.clear();
  ResetAllToDefaults();
  ClearPendingRestartFlags();
  GetPendingValuesStorage().clear();
  g_init_done = false;
  g_finalized = false;
}

}  // namespace testing

}  // namespace rex::cvar
