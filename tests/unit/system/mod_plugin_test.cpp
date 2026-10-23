#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include <rex/platform/dynlib.h>
#include <rex/system/mod_plugin.h>

namespace {

std::filesystem::path ModRootFromPlugin(const std::filesystem::path& plugin_path) {
  return plugin_path.parent_path().parent_path().parent_path();
}

}  // namespace

TEST_CASE("mod plugin loader dispatches the inert lifecycle", "[mod_plugin]") {
  std::filesystem::path plugin_path(REX_TEST_MOD_PLUGIN_PATH);
  auto mod_root = ModRootFromPlugin(plugin_path);

  rex::system::ModHostContext context{};
  auto plugin = rex::system::LoadModPlugin(mod_root, "lifecycle", "lifecycle", context);
  REQUIRE(plugin);

  plugin->OnCreateDialogs(nullptr);
  plugin->OnModuleLaunched();
  plugin->OnShutdown();

  rex::platform::DynamicLibrary inspection;
  REQUIRE(inspection.Load(plugin_path, rex::platform::SymbolResolution::kImmediate));
  using CountsFn = void (*)(uint32_t*);
  auto counts_fn = inspection.GetSymbol<CountsFn>("rex_test_mod_counts");
  REQUIRE(counts_fn);

  std::array<uint32_t, 4> counts{};
  counts_fn(counts.data());
  CHECK(counts == std::array<uint32_t, 4>{1, 1, 1, 1});
}

TEST_CASE("mod plugin loader rejects an ABI mismatch", "[mod_plugin]") {
  std::filesystem::path plugin_path(REX_TEST_BAD_MOD_PLUGIN_PATH);
  auto mod_root = ModRootFromPlugin(plugin_path);

  rex::system::ModHostContext context{};
  CHECK_FALSE(rex::system::LoadModPlugin(mod_root, "bad_abi", "bad_abi", context));
}

TEST_CASE("mod plugin loader rejects code paths", "[mod_plugin]") {
  rex::system::ModHostContext context{};
  CHECK_FALSE(rex::system::LoadModPlugin({}, "escape", "../outside", context));
}
