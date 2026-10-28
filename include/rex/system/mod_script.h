#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rex {
class Runtime;
namespace system {

inline constexpr uint32_t kModManifestVersionScript = 2;
enum class ModPayloadKind : uint8_t { kNative, kLua };

// The decoded data owns its storage and remains valid after the VM closes.
// Integer keys sort before string keys; strings sort by byte value.
struct ModScriptValue {
  enum class Type : uint8_t { kBoolean, kInteger, kNumber, kString, kTable };
  struct Entry;
  Type type = Type::kTable;
  bool boolean = false;
  int64_t integer = 0;
  double number = 0;
  std::string string;
  std::vector<Entry> entries;
};
struct ModScriptValue::Entry {
  ModScriptValue key;
  ModScriptValue value;
};

struct ModScriptLimits {
  size_t max_source_bytes = 256 * 1024;
  size_t max_memory_bytes = 8 * 1024 * 1024;
  uint64_t max_instructions = 1'000'000;
  uint32_t hook_interval = 1000;
  uint32_t max_value_depth = 8;
  size_t max_value_entries = 4096;
  size_t max_value_bytes = 2 * 1024 * 1024;
};
enum class ModScriptStage : uint8_t {
  kRead,
  kCompile,
  kExecute,
  kBudget,
  kMemory,
  kDecode,
  kSchema,
  kSupport,
  kPublish,
};
const char* ToString(ModScriptStage stage);
struct ModScriptError {
  ModScriptStage stage = ModScriptStage::kRead;
  std::string message;
};
struct ModScriptRunResult {
  bool ok = false;
  ModScriptValue value;
  ModScriptError error;
};

// Each call owns a fresh isolated state and closes it before returning.
ModScriptRunResult RunModScript(std::string_view chunk_name, std::string_view source,
                                const ModScriptLimits& limits = {});
// Checks the direct-child filename and regular, non-linked file without executing it.
bool IsSafeModScriptEntry(const std::filesystem::path& mod_root, std::string_view entry);
ModScriptRunResult RunModScriptFile(const std::filesystem::path& mod_root, std::string_view entry,
                                    const ModScriptLimits& limits = {});
struct ModScriptContract {
  std::string api;
  uint32_t version = 0;
  std::vector<std::string> domains;
};

// UI-thread transaction. Prepare before publication; do not mutate or rescan the
// Runtime's mod state until Commit or destruction. Destruction cancels preparation.
class ModScriptActivation {
 public:
  ModScriptActivation(const ModScriptActivation&) = delete;
  ModScriptActivation& operator=(const ModScriptActivation&) = delete;
  ModScriptActivation(ModScriptActivation&& other) noexcept;
  void Commit() noexcept;

 private:
  friend class rex::Runtime;
  ModScriptActivation(Runtime& runtime, std::string id, size_t order);
  Runtime* runtime_;
  std::string id_;
  size_t order_;
};

struct ModScriptPublishRequest {
  std::string_view package_id;
  std::string_view entry;
  std::string_view api;
  uint32_t api_version;
  std::span<const std::string> declared_domains;
  const ModScriptValue& value;
};
struct ModScriptPublishResult {
  bool published = false;
  ModScriptStage stage = ModScriptStage::kSchema;
  std::string message;
};
// Prepared replacement on the UI thread owns validation, storage, and publication locks.
// Commit cannot fail or allocate. Destroying it without Commit leaves rules intact.
class ModScriptReplacement {
 public:
  virtual ~ModScriptReplacement() = default;
  virtual void Commit() noexcept = 0;
};

class IModScriptHost {
 public:
  virtual ~IModScriptHost() = default;
  virtual std::span<const ModScriptContract> contracts() const = 0;
  // Validate and atomically publish the whole bundle. After committing, return
  // success without allocating. Descriptors remain immutable after registration.
  virtual ModScriptPublishResult Publish(const ModScriptPublishRequest& request) noexcept = 0;
  // Replaces exactly this host's prior Lua-owned loadout, including removals.
  // Empty requests unload all. Hosts default to publication only at startup.
  virtual bool supports_replacement() const noexcept { return false; }
  virtual std::unique_ptr<ModScriptReplacement> PrepareReplacement(
      std::span<const ModScriptPublishRequest>, ModScriptPublishResult& result) noexcept {
    result.stage = ModScriptStage::kSupport;
    return nullptr;
  }
};

struct ModPackage;
// Runs on the UI thread before guest resume. Native callback receives only native
// packages; failures affect one package and never alter the desired loadout.
void DispatchModLaunch(Runtime& runtime, std::span<const ModPackage> packages,
                       const std::function<void(const ModPackage&)>& launch_native);

}  // namespace system
}  // namespace rex
