#include <catch2/catch_test_macros.hpp>
#include <rex/system/mod_script.h>
#include <array>
#include <limits>
#include <string>
#include "mod_script_fixtures.h"

using namespace rex::system;
namespace rex::system {
ModScriptRunResult RunModScriptForTest(std::string_view source, size_t fail_at, size_t& attempts,
                                       size_t& live_bytes, int inject, ModScriptStage& last_stage);
}
namespace {
const ModScriptValue& Field(const ModScriptValue& table, std::string_view name) {
  REQUIRE(table.type == ModScriptValue::Type::kTable);
  for (const auto& entry : table.entries)
    if (entry.key.type == ModScriptValue::Type::kString && entry.key.string == name)
      return entry.value;
  FAIL("missing field " << name);
  return table;
}
void Strings(const ModScriptValue& table,
             std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
  for (const auto& [key, expected] : fields) {
    REQUIRE(Field(table, key).type == ModScriptValue::Type::kString);
    REQUIRE(Field(table, key).string == expected);
  }
}
void Error(std::string_view source, ModScriptStage stage, ModScriptLimits limits = {}) {
  const auto result = RunModScript("main.lua", source, limits);
  INFO(result.error.message);
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error.stage == stage);
}
}  // namespace

TEST_CASE("Lua scripts produce complete owned trees", "[mod_script]") {
  auto hills = RunModScript("main.lua", kHills);
  REQUIRE(hills.ok);
  REQUIRE(hills.value.entries.size() == 2);
  Strings(hills.value, {{"provider_id", "aeshur.hills-production"}});
  const auto& hill_rules = Field(hills.value, "rules");
  REQUIRE(hill_rules.entries.size() == 1);
  REQUIRE(hill_rules.entries[0].key.integer == 1);
  const auto& hill = hill_rules.entries[0].value;
  REQUIRE(hill.entries.size() == 6);
  Strings(hill, {{"id", "hills-production"},
                 {"kind", "terrain_yield"},
                 {"terrain", "hill"},
                 {"component", "production"},
                 {"operation", "add"}});
  REQUIRE(Field(hill, "value").integer == 1);
  auto jaguar = RunModScript("main.lua", kJaguar);
  REQUIRE(jaguar.ok);
  REQUIRE(jaguar.value.entries.size() == 2);
  Strings(jaguar.value, {{"provider_id", "aeshur.jaguar-woodsman"}});
  const auto& rules = Field(jaguar.value, "rules");
  REQUIRE(rules.entries.size() == 3);
  for (size_t i = 0; i < 3; ++i)
    REQUIRE(rules.entries[i].key.integer == i + 1);
  for (size_t i = 0; i < 2; ++i) {
    const auto& rule = rules.entries[i].value;
    REQUIRE(rule.entries.size() == 8);
    Strings(rule,
            {{"id", i == 0 ? "jaguar-warrior-forest-attack" : "jaguar-warrior-forest-defense"},
             {"kind", "unit_combat"},
             {"civilization", "aztec"},
             {"base_unit_type", "warrior"},
             {"identity", "jaguar_warrior"},
             {"terrain", "forest"},
             {"property", i == 0 ? "attack" : "defense"}});
    REQUIRE(Field(rule, "percentage_delta").integer == 50);
  }
  const auto& text = rules.entries[2].value;
  REQUIRE(text.entries.size() == 8);
  Strings(text, {{"id", "jaguar-woodsman-text"},
                 {"kind", "nation_select_text"},
                 {"surface", "unique_unit"},
                 {"civilization", "aztec"},
                 {"base_unit_type", "warrior"},
                 {"identity", "jaguar_warrior"},
                 {"display_form", "unit"},
                 {"text", "Jaguar Warrior - Warrior with +50% Attack and Defense in Forest"}});
}

TEST_CASE("Lua environment exposes only admitted helpers and isolates calls", "[mod_script]") {
  const auto result = RunModScript("main.lua", R"(
    assert(type(assert) == 'function' and type(error) == 'function')
    assert(type(type) == 'function' and type(ipairs) == 'function')
    assert(type(pairs) == 'function' and type(next) == 'function')
    assert(print == nil and string == nil and load == nil and require == nil)
    assert(pcall == nil and xpcall == nil and os == nil and io == nil and debug == nil)
    assert(setmetatable == nil and getmetatable == nil and collectgarbage == nil)
    assert(_G == nil and _VERSION == nil and loadfile == nil and dofile == nil)
    escaped = 1
    local total = 0
    for _, value in ipairs({1, 2, 3}) do total = total + value end
    for key, value in pairs({bonus=4}) do total = total + value end
    return total
  )");
  INFO(result.error.message);
  REQUIRE(result.ok);
  REQUIRE(result.value.integer == 10);
  REQUIRE(RunModScript("main.lua", "assert(escaped == nil); return true").ok);
}

TEST_CASE("Lua errors preserve stages and source lines", "[mod_script]") {
  const auto syntax = RunModScript("main.lua", "local x = 1\nreturn {");
  REQUIRE_FALSE(syntax.ok);
  REQUIRE(syntax.error.stage == ModScriptStage::kCompile);
  REQUIRE(syntax.error.message.find("main.lua:2:") != std::string::npos);
  for (const auto source :
       {"local x = 1\nerror('broken')", "local x = 1\nerror({})", "local x = 1\nerror('broken', 0)",
        "local x = 1\nassert(false, 'broken')"}) {
    const auto result = RunModScript("main.lua", source);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.stage == ModScriptStage::kExecute);
    REQUIRE(result.error.message.find("main.lua:2:") != std::string::npos);
  }
  Error(std::string("\x1bLua", 4), ModScriptStage::kCompile);
  Error("return", ModScriptStage::kDecode);
  Error("return 1, 2", ModScriptStage::kDecode);
}

TEST_CASE("Lua enforces execution, source, memory and owned value limits", "[mod_script]") {
  ModScriptLimits limits;
  limits.max_instructions = 100;
  limits.hook_interval = 10;
  Error("while true do end", ModScriptStage::kBudget, limits);
  Error("local n = 0; for i=1,10000 do n=n+i end; return n", ModScriptStage::kBudget, limits);
  limits = {};
  limits.max_source_bytes = 3;
  Error("return 1", ModScriptStage::kRead, limits);
  limits = {};
  limits.max_memory_bytes = 1;
  Error("return {}", ModScriptStage::kMemory, limits);
  limits.max_memory_bytes = 32768;
  Error("local t={}; for i=1,10000 do t[i]={i,i,i} end; return t", ModScriptStage::kMemory, limits);
  limits = {};
  limits.max_value_depth = 2;
  REQUIRE(RunModScript("main.lua", "return {{1}}", limits).ok);
  Error("return {{{1}}}", ModScriptStage::kDecode, limits);
  limits = {};
  limits.max_value_entries = 2;
  REQUIRE(RunModScript("main.lua", "return {1,2}", limits).ok);
  Error("return {1,{2}}", ModScriptStage::kDecode, limits);
  limits = {};
  limits.max_value_bytes = 3;
  REQUIRE(RunModScript("main.lua", "return {a='bc'}", limits).ok);
  Error("return {ab='cd'}", ModScriptStage::kDecode, limits);
}

TEST_CASE("Lua decoder rejects non-data and retains numeric distinctions", "[mod_script]") {
  for (const auto source :
       {"local t={}; t.self=t; return t", "return function() end", "return {value=function() end}",
        "return 0/0", "return 1/0", "return -1/0", "return {[1.5]=true}", "return {[true]=1}",
        "return {[{}]=1}"}) {
    INFO(source);
    Error(source, ModScriptStage::kDecode);
  }
  for (int inject = 1; inject <= 4; ++inject) {
    size_t attempts = 0, live = 0;
    ModScriptStage stage;
    auto result = RunModScriptForTest("return {}", 0, attempts, live, inject, stage);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.stage == ModScriptStage::kDecode);
    REQUIRE(live == 0);
  }
  auto result = RunModScript(
      "main.lua", "return {[-2]=false,[1]=1.0,z=9223372036854775807,a=-9223372036854775807-1}");
  REQUIRE(result.ok);
  REQUIRE(result.value.entries[0].key.integer == -2);
  REQUIRE(result.value.entries[1].value.type == ModScriptValue::Type::kNumber);
  REQUIRE(result.value.entries[1].value.number == 1.0);
  REQUIRE(result.value.entries[2].key.string == "a");
  REQUIRE(result.value.entries[2].value.integer == std::numeric_limits<int64_t>::min());
  REQUIRE(result.value.entries[3].key.string == "z");
  REQUIRE(result.value.entries[3].value.integer == std::numeric_limits<int64_t>::max());
  REQUIRE(RunModScript("main.lua", "local t={a=1}; return {t,t}").ok);
}

TEST_CASE("Lua allocation denial at every compile, execute and decode allocation closes state",
          "[mod_script]") {
  constexpr std::string_view source = R"(
    local result = { label = 'a nontrivial owned string value', records = {} }
    for i=1,40 do result.records[i] = {name='item-' .. i, value=i*i} end
    return result
  )";
  size_t count = 0, live = 0;
  ModScriptStage stage;
  REQUIRE(RunModScriptForTest(source, 0, count, live, 0, stage).ok);
  REQUIRE(live == 0);
  std::array<bool, 3> reached{};
  for (size_t n = 1; n <= count; ++n) {
    size_t attempts = 0;
    auto result = RunModScriptForTest(source, n, attempts, live, 0, stage);
    INFO("allocation " << n << " of " << count << ": " << result.error.message);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.stage == ModScriptStage::kMemory);
    REQUIRE(live == 0);
    if (stage == ModScriptStage::kCompile)
      reached[0] = true;
    if (stage == ModScriptStage::kExecute)
      reached[1] = true;
    if (stage == ModScriptStage::kDecode)
      reached[2] = true;
  }
  REQUIRE(reached == std::array<bool, 3>{true, true, true});
}
