#include <rex/system/mod_script.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace rex::system {
namespace {
static_assert(sizeof(lua_Integer) == sizeof(int64_t));
static_assert(sizeof(lua_Number) == sizeof(double));

struct Context {
  const ModScriptLimits& limits;
  size_t used = 0;
  bool memory_failed = false;
  bool budget_failed = false;
  uint64_t instructions = 0;
  ModScriptStage stage = ModScriptStage::kCompile;
  std::string_view source;
  const char* name = nullptr;
  ModScriptValue value;
  size_t entries = 0;
  size_t bytes = 0;
  std::vector<const void*> ancestors;
#ifdef REX_MOD_SCRIPT_TESTING
  size_t fail_at = 0;
  size_t attempts = 0;
  int inject = 0;
#endif
};

bool DenyAllocation(Context& context) {
#ifdef REX_MOD_SCRIPT_TESTING
  ++context.attempts;
  if (context.fail_at && context.attempts >= context.fail_at)
    context.memory_failed = true;
#endif
  return context.memory_failed;
}

void* Allocate(void* ud, void* pointer, size_t old_size, size_t new_size) {
  auto& context = *static_cast<Context*>(ud);
  if (!pointer)
    old_size = 0;  // Lua uses a type tag for fresh allocations.
  if (!new_size) {
    context.used -= old_size;
    std::free(pointer);
    return nullptr;
  }
  if (new_size > old_size &&
      (DenyAllocation(context) ||
       new_size - old_size > context.limits.max_memory_bytes - context.used)) {
    context.memory_failed = true;
    return nullptr;
  }
  void* result = std::realloc(pointer, new_size);
  if (result)
    context.used = context.used - old_size + new_size;
  else
    context.memory_failed = true;
  return result;
}

Context& GetContext(lua_State* state) {
  void* ud = nullptr;
  lua_getallocf(state, &ud);
  return *static_cast<Context*>(ud);
}

void CountInstructions(lua_State* state, lua_Debug*) {
  auto& context = GetContext(state);
  const auto interval = context.limits.hook_interval;
  if (context.instructions >= context.limits.max_instructions ||
      interval > context.limits.max_instructions - context.instructions) {
    context.budget_failed = true;
    luaL_error(state, "instruction budget exhausted");
  }
  context.instructions += interval;
}

void NativeAllocation(Context& context) {
  if (DenyAllocation(context))
    throw std::bad_alloc();
}

ModScriptValue Decode(lua_State* state, int index, Context& context, uint32_t depth, bool key) {
  using Type = ModScriptValue::Type;
  ModScriptValue value;
  index = lua_absindex(state, index);
  const int type = lua_type(state, index);
  if (key && type != LUA_TSTRING && !(type == LUA_TNUMBER && lua_isinteger(state, index)))
    throw std::runtime_error("table keys must be integers or strings");
  switch (type) {
    case LUA_TBOOLEAN:
      value.type = Type::kBoolean;
      value.boolean = lua_toboolean(state, index) != 0;
      break;
    case LUA_TNUMBER:
      if (lua_isinteger(state, index)) {
        value.type = Type::kInteger;
        value.integer = lua_tointeger(state, index);
      } else {
        value.type = Type::kNumber;
        value.number = lua_tonumber(state, index);
        if (!std::isfinite(value.number))
          throw std::runtime_error("nonfinite number");
      }
      break;
    case LUA_TSTRING: {
      value.type = Type::kString;
      size_t length = 0;
      const char* bytes = lua_tolstring(state, index, &length);
      if (length > context.limits.max_value_bytes - context.bytes)
        throw std::runtime_error("value byte limit exceeded");
      context.bytes += length;
      NativeAllocation(context);
      value.string.assign(bytes, length);
      break;
    }
    case LUA_TTABLE: {
      if (depth > context.limits.max_value_depth)
        throw std::runtime_error("value depth limit exceeded");
      if (!lua_checkstack(state, 4))
        throw std::bad_alloc();
      if (lua_getmetatable(state, index))
        throw std::runtime_error("table has a metatable");
      const void* identity = lua_topointer(state, index);
      if (std::find(context.ancestors.begin(), context.ancestors.end(), identity) !=
          context.ancestors.end())
        throw std::runtime_error("table cycle");
      NativeAllocation(context);
      context.ancestors.push_back(identity);
      value.type = Type::kTable;
      lua_pushnil(state);
      while (lua_next(state, index)) {
        if (context.entries == context.limits.max_value_entries)
          throw std::runtime_error("value entry limit exceeded");
        ++context.entries;
        auto owned_key = Decode(state, -2, context, depth, true);
        auto owned_value = Decode(state, -1, context, depth + 1, false);
        NativeAllocation(context);
        value.entries.push_back({std::move(owned_key), std::move(owned_value)});
        lua_pop(state, 1);
      }
      context.ancestors.pop_back();
      std::sort(value.entries.begin(), value.entries.end(), [](const auto& a, const auto& b) {
        if (a.key.type != b.key.type)
          return a.key.type == Type::kInteger;
        if (a.key.type == Type::kInteger)
          return a.key.integer < b.key.integer;
        return std::lexicographical_compare(a.key.string.begin(), a.key.string.end(),
                                            b.key.string.begin(), b.key.string.end(),
                                            [](unsigned char x, unsigned char y) { return x < y; });
      });
      break;
    }
    default:
      throw std::runtime_error("unsupported value type: " + std::string(lua_typename(state, type)));
  }
  return value;
}

int ErrorMessage(lua_State* state) {
  const bool is_string = lua_type(state, 1) == LUA_TSTRING;
  lua_Debug frame{};
  // The failing Lua caller is above error() or another C helper.
  for (int level = 1; lua_getstack(state, level, &frame); ++level) {
    lua_getinfo(state, "Sl", &frame);
    if (frame.currentline > 0) {
      if (is_string) {
        // error(message, 0) and assert(false, message) omit the position.
        const std::string_view message = lua_tostring(state, 1);
        const std::string_view source = frame.short_src;
        if (message.starts_with(source) && message.substr(source.size()).starts_with(":"))
          return 1;
        lua_pushfstring(state, "%s:%d: %s", frame.short_src, frame.currentline,
                        lua_tostring(state, 1));
        return 1;
      }
      lua_pushfstring(state, "%s:%d: non-string error (%s)", frame.short_src, frame.currentline,
                      lua_typename(state, lua_type(state, 1)));
      return 1;
    }
  }
  if (is_string)
    return 1;
  lua_pushfstring(state, "non-string error (%s)", lua_typename(state, lua_type(state, 1)));
  return 1;
}

int RunProtected(lua_State* state) {
  auto& context = GetContext(state);
  // Only copy admitted helpers. No original global table is reachable by the chunk.
  luaopen_base(state);
  lua_newtable(state);
  for (const char* helper : {"assert", "error", "type", "ipairs", "pairs", "next"}) {
    lua_getfield(state, -2, helper);
    lua_setfield(state, -2, helper);
  }
  lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  lua_pop(state, 1);
  lua_pushcfunction(state, ErrorMessage);
  const int handler = lua_gettop(state);
  const int compile =
      luaL_loadbufferx(state, context.source.data(), context.source.size(), context.name, "t");
  if (compile != LUA_OK)
    return lua_error(state);
  context.stage = ModScriptStage::kExecute;
  lua_sethook(state, CountInstructions, LUA_MASKCOUNT,
              static_cast<int>(context.limits.hook_interval));
  const int execute = lua_pcall(state, 0, LUA_MULTRET, handler);
  lua_sethook(state, nullptr, 0, 0);
  if (execute != LUA_OK)
    return lua_error(state);
  context.stage = ModScriptStage::kDecode;
  if (lua_gettop(state) != handler + 1)
    return luaL_error(state, "entry must return exactly one value");
#ifdef REX_MOD_SCRIPT_TESTING
  if (context.inject == 1) {
    lua_newtable(state);
    lua_setmetatable(state, -2);
  }
  if (context.inject == 2) {
    lua_pop(state, 1);
    lua_newuserdatauv(state, 1, 0);
  }
  if (context.inject == 3) {
    lua_pop(state, 1);
    lua_newthread(state);
  }
  if (context.inject == 4) {
    lua_pop(state, 1);
    lua_pushlightuserdata(state, &context);
  }
#endif
  // Lua is built as C++; its private exception unwinds owned decoder frames.
  // Translate native allocation errors here before Lua's foreign-exception handler.
  try {
    context.value = Decode(state, -1, context, 1, false);
  } catch (const std::bad_alloc&) {
    context.memory_failed = true;
    return luaL_error(state, "owned value allocation failed");
  } catch (const std::runtime_error& error) {
    return luaL_error(state, "%s", error.what());
  }
  return 0;
}

ModScriptRunResult Failure(ModScriptStage stage, std::string message) {
  ModScriptRunResult result;
  result.error = {stage, std::move(message)};
  return result;
}

ModScriptRunResult Run(Context& context, std::string_view chunk_name) {
  if (context.source.size() > context.limits.max_source_bytes)
    return Failure(ModScriptStage::kRead, "source size limit exceeded");
  if (!context.limits.hook_interval || context.limits.hook_interval > INT_MAX)
    return Failure(ModScriptStage::kBudget, "hook interval must be between 1 and INT_MAX");
  try {
    const std::string name = "@" + std::string(chunk_name);
    context.name = name.c_str();
    std::unique_ptr<lua_State, decltype(&lua_close)> state(lua_newstate(Allocate, &context, 0),
                                                           lua_close);
    if (!state)
      return Failure(ModScriptStage::kMemory, "Lua state allocation failed");
    // A zero-upvalue C function uses no allocation; the initial stack has room.
    lua_pushcfunction(state.get(), RunProtected);
    const int status = lua_pcall(state.get(), 0, 0, 0);
    if (context.memory_failed || status == LUA_ERRMEM)
      return Failure(ModScriptStage::kMemory, "allocation limit or allocation failure");
    if (context.budget_failed)
      return Failure(ModScriptStage::kBudget, "instruction budget exhausted");
    if (status != LUA_OK) {
      size_t length = 0;
      const char* message = lua_type(state.get(), -1) == LUA_TSTRING
                                ? lua_tolstring(state.get(), -1, &length)
                                : nullptr;
      return Failure(context.stage,
                     message ? std::string(message, length) : "non-string Lua error");
    }
    ModScriptRunResult result;
    result.value = std::move(context.value);
    result.ok = true;
    return result;
  } catch (const std::bad_alloc&) {
    return Failure(ModScriptStage::kMemory, "native allocation failed");
  }
}
// Windows maps these stems to devices regardless of extension, case, or
// trailing dots and spaces, so every host rejects them.
bool IsReservedDeviceStem(std::string_view stem) {
  while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
    stem.remove_suffix(1);
  std::string upper(stem);
  for (auto& c : upper)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL")
    return true;
  return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
         upper[3] >= '1' && upper[3] <= '9';
}

}  // namespace

const char* ToString(ModScriptStage stage) {
  switch (stage) {
    case ModScriptStage::kRead:
      return "read";
    case ModScriptStage::kCompile:
      return "compile";
    case ModScriptStage::kExecute:
      return "execute";
    case ModScriptStage::kBudget:
      return "budget";
    case ModScriptStage::kMemory:
      return "memory";
    case ModScriptStage::kDecode:
      return "decode";
    case ModScriptStage::kSchema:
      return "schema";
    case ModScriptStage::kSupport:
      return "support";
    case ModScriptStage::kPublish:
      return "publish";
  }
  return "unknown";
}

ModScriptRunResult RunModScript(std::string_view name, std::string_view source,
                                const ModScriptLimits& limits) {
  Context context{limits};
  context.source = source;
  return Run(context, name);
}

bool IsSafeModScriptEntry(const std::filesystem::path& root, std::string_view entry) {
  if (entry.size() <= 4 || !entry.ends_with(".lua") || entry.find_first_of("/\\:") != entry.npos ||
      entry.find('\0') != entry.npos || entry == "." || entry == ".." ||
      IsReservedDeviceStem(entry.substr(0, entry.size() - 4)))
    return false;
  const auto path = root / std::string(entry);
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(root, error);
  if (error || !std::filesystem::is_directory(root_status) ||
      std::filesystem::is_symlink(root_status))
    return false;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status))
    return false;
#ifdef _WIN32
  const DWORD root_attributes = GetFileAttributesW(root.c_str());
  if (root_attributes == INVALID_FILE_ATTRIBUTES ||
      (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    return false;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    return false;
#endif
  return true;
}

ModScriptRunResult RunModScriptFile(const std::filesystem::path& root, std::string_view entry,
                                    const ModScriptLimits& limits) {
  try {
    if (!IsSafeModScriptEntry(root, entry))
      return Failure(ModScriptStage::kRead, "unsafe or missing Lua entry");
    std::ifstream stream(root / std::string(entry), std::ios::binary);
    if (!stream)
      return Failure(ModScriptStage::kRead, "cannot open Lua entry");
    std::string source;
    char buffer[4096];
    while (stream) {
      stream.read(buffer, sizeof(buffer));
      const auto count = static_cast<size_t>(stream.gcount());
      if (count > limits.max_source_bytes - source.size())
        return Failure(ModScriptStage::kRead, "source size limit exceeded");
      source.append(buffer, count);
    }
    if (!stream.eof())
      return Failure(ModScriptStage::kRead, "cannot read Lua entry");
    return RunModScript(entry, source, limits);
  } catch (const std::bad_alloc&) {
    return Failure(ModScriptStage::kMemory, "source allocation failed");
  } catch (const std::filesystem::filesystem_error& error) {
    return Failure(ModScriptStage::kRead, error.what());
  }
}

#ifdef REX_MOD_SCRIPT_TESTING
// Linked only into the standalone test executable, never the runtime library.
ModScriptRunResult RunModScriptForTest(std::string_view source, size_t fail_at, size_t& attempts,
                                       size_t& live_bytes, int inject, ModScriptStage& last_stage) {
  ModScriptLimits limits;
  Context context{limits};
  context.source = source;
  context.fail_at = fail_at;
  context.inject = inject;
  auto result = Run(context, "main.lua");
  attempts = context.attempts;
  live_bytes = context.used;
  last_stage = context.stage;
  return result;
}
#endif
}  // namespace rex::system
