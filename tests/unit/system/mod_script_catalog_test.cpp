#include <catch2/catch_test_macros.hpp>
#include <rex/runtime.h>
#include <rex/system/mod_catalog.h>
#include <rex/system/mod_script.h>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <utility>

using namespace rex::system;
namespace {
struct PackageTree {
  std::filesystem::path root;
  std::string previous_root;
  PackageTree() {
    static std::atomic<unsigned> serial{0};
    root = std::filesystem::temp_directory_path() /
           ("rex_lua_catalog_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(serial++));
    std::filesystem::create_directories(root);
    previous_root = REXCVAR_GET(mods_data_root);
    REXCVAR_SET(mods_data_root, root.string());
  }
  ~PackageTree() {
    REXCVAR_SET(mods_data_root, previous_root);
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  void Write(std::string_view id,
             std::string_view payload =
                 "kind='lua'\nentry='main.lua'\napi='example.rules'\napi_version=1\nrequires=['"
                 "terrain_yield']\n",
             std::string_view extra_mod = "", std::string_view prefix = "manifest_version=2\n") {
    auto package = root / id;
    std::filesystem::create_directories(package);
    std::ofstream(package / "main.lua") << "return {answer=42}";
    std::ofstream manifest(package / "mod.toml");
    manifest << prefix << "[mod]\nid='" << id << "'\nname='Example'\nversion='1.0'\n"
             << extra_mod << "[payload]\n"
             << payload;
  }
};
const std::array<ModScriptContract, 1> kContracts = {
    {{"example.rules", 1, {"terrain_yield", "text"}}}};
ModCatalog Discover(const PackageTree& tree,
                    std::span<const ModScriptContract> contracts = kContracts) {
  return DiscoverModCatalog(tree.root, "1.0.0", kModPluginAbiVersion, contracts);
}
bool Message(const ModPackage& package, std::string_view substring) {
  return std::any_of(package.diagnostics.begin(), package.diagnostics.end(),
                     [&](const auto& diagnostic) {
                       return diagnostic.message.find(substring) != std::string::npos;
                     });
}
struct FakeHost : IModScriptHost {
  rex::Runtime* runtime = nullptr;
  std::vector<std::string>* calls = nullptr;
  bool observed_inactive = true;
  bool prior_committed = true;
  std::span<const ModScriptContract> contracts() const override { return kContracts; }
  ModScriptPublishResult Publish(const ModScriptPublishRequest& request) noexcept override {
    calls->push_back(std::string(request.package_id));
    observed_inactive &= !runtime->mod_catalog().Find(request.package_id)->active;
    if (request.package_id == "bad") {
      prior_committed &= runtime->mod_catalog().Find("first")->active;
      return {false, ModScriptStage::kPublish, "fixture rejection"};
    }
    if (request.package_id == "last") {
      prior_committed &= runtime->mod_catalog().Find("first")->active &&
                         !runtime->mod_catalog().Find("bad")->active;
    }
    return {true};
  }
};
}  // namespace

TEST_CASE("Lua v2 catalog validates payload and advertised contract", "[mod_catalog][mod_script]") {
  PackageTree tree;
  tree.Write("valid");
  auto catalog = Discover(tree);
  REQUIRE(catalog.packages.size() == 1);
  const auto& package = catalog.packages[0];
  REQUIRE(package.status == ModPackageStatus::kReady);
  REQUIRE(package.payload_kind == ModPayloadKind::kLua);
  REQUIRE(package.entry_path == tree.root / "valid/main.lua");
  REQUIRE(package.required_domains == std::vector<std::string>{"terrain_yield"});
  REQUIRE_FALSE(package.HasBlockingError());
  REQUIRE(Discover(tree, {}).packages[0].status == ModPackageStatus::kUnsupportedScriptContract);
  for (const auto& contract : {ModScriptContract{"other.rules", 1, {"terrain_yield"}},
                               ModScriptContract{"example.rules", 2, {"terrain_yield"}},
                               ModScriptContract{"example.rules", 1, {"text"}}}) {
    auto unsupported = Discover(tree, std::span(&contract, 1));
    REQUIRE(unsupported.packages[0].status == ModPackageStatus::kUnsupportedScriptContract);
    REQUIRE(unsupported.packages[0].HasBlockingError());
    if (contract.api == "example.rules" && contract.version == 1)
      REQUIRE(Message(unsupported.packages[0], "terrain_yield"));
  }
}

TEST_CASE("Lua v2 rejects mixed unknown missing and mistyped payload fields",
          "[mod_catalog][mod_script]") {
  PackageTree tree;
  const std::string valid =
      "kind='lua'\nentry='main.lua'\napi='example.rules'\napi_version=1\nrequires=['terrain_yield']"
      "\n";
  std::vector<std::string> invalid = {valid + "other=true\n", valid + "code='native'\n",
                                      valid + "plugin_abi=1\n",
                                      valid + "[payload.native]\ncode='mixed'\n", ""};
  for (const auto field : {"kind='lua'\n", "entry='main.lua'\n", "api='example.rules'\n",
                           "api_version=1\n", "requires=['terrain_yield']\n"}) {
    auto missing = valid;
    missing.erase(missing.find(field), std::string_view(field).size());
    invalid.push_back(std::move(missing));
  }
  for (const auto replacement :
       {"requires=[]", "requires=['terrain_yield','terrain_yield']", "requires=[1]",
        "requires=['bad.name']", "requires='terrain_yield'"}) {
    invalid.push_back(valid.substr(0, valid.find("requires=")) + replacement + "\n");
  }
  for (const auto version : {"0", "-1", "1.0", "4294967296", "'1'"}) {
    auto bad = valid;
    bad.replace(bad.find("api_version=1"), 13, std::string("api_version=") + version + "\n");
    invalid.push_back(std::move(bad));
  }
  for (const auto& payload : invalid) {
    INFO(payload);
    tree.Write("invalid", payload);
    REQUIRE(Discover(tree).Find("invalid")->HasBlockingError());
  }
  for (const auto native : {"code='native'\n", "plugin_abi=1\n"}) {
    tree.Write("invalid", valid, native);
    REQUIRE(Discover(tree).Find("invalid")->HasBlockingError());
  }
  tree.Write("invalid", valid, "", "manifest_version=2\nplugin_abi=1\n");
  REQUIRE(Discover(tree).Find("invalid")->HasBlockingError());
  tree.Write("invalid", valid, "", "manifest_version=2.0\n");
  REQUIRE(Discover(tree).Find("invalid")->HasBlockingError());
  tree.Write("invalid");
  std::filesystem::create_directories(tree.root / "invalid/code");
  REQUIRE(Message(*Discover(tree).Find("invalid"), "code/"));
}

TEST_CASE("Lua v2 and file runner reject unsafe and missing entries", "[mod_catalog][mod_script]") {
  PackageTree tree;
  for (const auto entry :
       {"sub/main.lua", "sub\\main.lua", "../main.lua", "..\\main.lua", "/main.lua", "C:/main.lua",
        "C:main.lua", "main.lua:stream", "main.txt", "missing.lua", "CON.lua", "nul.lua",
        "Aux .lua", "LPT1.lua", "com9..lua"}) {
    INFO(entry);
    const std::string payload =
        "kind='lua'\nentry='" + std::string(entry) +
        "'\napi='example.rules'\napi_version=1\nrequires=['terrain_yield']\n";
    tree.Write("unsafe", payload);
    REQUIRE(Discover(tree).Find("unsafe")->HasBlockingError());
    REQUIRE(RunModScriptFile(tree.root / "unsafe", entry).error.stage == ModScriptStage::kRead);
    REQUIRE_FALSE(RunModScriptFile(tree.root / "unsafe", entry).ok);
  }
  tree.Write("unsafe");
  REQUIRE(RunModScriptFile(tree.root / "unsafe", "main.lua").ok);
  ModScriptLimits limits;
  limits.max_source_bytes = 3;
  REQUIRE(RunModScriptFile(tree.root / "unsafe", "main.lua", limits).error.stage ==
          ModScriptStage::kRead);
}

TEST_CASE("Lua v2 rejects linked entries when links are available", "[mod_catalog][mod_script]") {
  PackageTree tree;
  tree.Write("unsafe");
  std::error_code error;
  std::filesystem::create_symlink(tree.root / "unsafe/main.lua", tree.root / "unsafe/link.lua",
                                  error);
  if (error)
    SKIP("symbolic links unavailable on this host");
  REQUIRE_FALSE(IsSafeModScriptEntry(tree.root / "unsafe", "link.lua"));
  tree.Write("unsafe",
             "kind='lua'\nentry='link.lua'\napi='example.rules'\napi_version=1\nrequires=['terrain_"
             "yield']\n");
  REQUIRE(Discover(tree).Find("unsafe")->HasBlockingError());
}

TEST_CASE("Lua launch dispatch preserves mixed order publication status and rescan",
          "[mod_script]") {
  PackageTree tree;
  for (const auto id : {"first", "native", "bad", "last"})
    tree.Write(id);
  std::vector<std::string> calls;
  calls.reserve(4);
  auto host = std::make_shared<FakeHost>();
  rex::Runtime runtime({}, tree.root / "profile");
  host->runtime = &runtime;
  host->calls = &calls;
  rex::RuntimeConfig config;
  config.tool_mode = true;
  config.mod_script_host = host;
  REQUIRE(runtime.Setup(std::move(config)) == 0);
  runtime.RescanModCatalog();
  auto catalog = Discover(tree);
  std::vector<ModPackage> packages;
  for (const auto id : {"first", "native", "bad", "last"})
    packages.push_back(*catalog.Find(id));
  packages[1].payload_kind = ModPayloadKind::kNative;
  const std::vector<std::string> desired = {"first", "native", "bad", "last"};
  std::filesystem::create_directories(tree.root / "profile");
  std::ofstream(tree.root / "profile/mod_order.txt") << "first\nnative\nbad\nlast\n";
  runtime.RescanModCatalog();
  DispatchModLaunch(runtime, packages, [&](const auto& package) {
    REQUIRE(runtime.mod_catalog().Find("first")->active);
    calls.push_back(package.id);
  });
  REQUIRE(calls == desired);
  REQUIRE(host->observed_inactive);
  REQUIRE(host->prior_committed);
  REQUIRE(runtime.mod_catalog().Find("first")->active);
  REQUIRE(runtime.mod_catalog().Find("last")->active);
  REQUIRE_FALSE(runtime.mod_catalog().Find("bad")->active);
  REQUIRE(Message(*runtime.mod_catalog().Find("bad"), "publish: main.lua: fixture rejection"));
  REQUIRE(runtime.mod_order_ids() == desired);
  runtime.RescanModCatalog();
  REQUIRE(runtime.mod_catalog().Find("first")->active_order == 0);
  REQUIRE(runtime.mod_catalog().Find("last")->active_order == 3);
  REQUIRE(Message(*runtime.mod_catalog().Find("bad"), "publish: main.lua: fixture rejection"));
  REQUIRE_FALSE(runtime.mod_catalog().Find("bad")->HasBlockingError());
  REQUIRE(runtime.mod_order_ids() == desired);
}

TEST_CASE("Lua prepared activation commits without allocation and cancels by destruction",
          "[mod_script]") {
  static_assert(noexcept(std::declval<ModScriptActivation&>().Commit()));
  PackageTree tree;
  tree.Write("active");
  rex::Runtime runtime({}, tree.root / "profile");
  runtime.RescanModCatalog();
  {
    auto cancelled = runtime.PrepareModScriptActivation("active", 2);
    REQUIRE_FALSE(runtime.mod_catalog().Find("active")->active);
  }
  REQUIRE(runtime.active_mod_states().empty());
  auto activation = runtime.PrepareModScriptActivation("active", 2);
  const auto* storage = runtime.active_mod_states().data();
  const auto capacity = runtime.active_mod_states().capacity();
  auto moved = std::move(activation);
  activation.Commit();
  REQUIRE(runtime.active_mod_states().empty());
  moved.Commit();
  moved.Commit();
  REQUIRE(runtime.active_mod_states().data() == storage);
  REQUIRE(runtime.active_mod_states().capacity() == capacity);
  REQUIRE(runtime.active_mod_states().size() == 1);
  REQUIRE(runtime.mod_catalog().Find("active")->active);
  runtime.RescanModCatalog();
  REQUIRE(runtime.mod_catalog().Find("active")->active_order == 2);
}

TEST_CASE("Lua launch failures retain stages and continue to later packages", "[mod_script]") {
  PackageTree tree;
  tree.Write("syntax");
  tree.Write("good");
  tree.Write("missing");
  auto catalog = Discover(tree);
  std::vector<ModPackage> packages = {*catalog.Find("syntax"), *catalog.Find("missing"),
                                      *catalog.Find("good")};
  std::ofstream(tree.root / "syntax/main.lua") << "return {";
  packages[1].entry = "absent.lua";
  std::vector<std::string> calls;
  calls.reserve(1);
  auto host = std::make_shared<FakeHost>();
  rex::Runtime runtime({}, tree.root / "profile");
  host->runtime = &runtime;
  host->calls = &calls;
  rex::RuntimeConfig config;
  config.tool_mode = true;
  config.mod_script_host = host;
  REQUIRE(runtime.Setup(std::move(config)) == 0);
  runtime.RescanModCatalog();
  DispatchModLaunch(runtime, packages, [](const auto&) { FAIL("unexpected native callback"); });
  REQUIRE(calls == std::vector<std::string>{"good"});
  REQUIRE(Message(*runtime.mod_catalog().Find("syntax"), "compile: main.lua:"));
  REQUIRE(Message(*runtime.mod_catalog().Find("missing"), "read: absent.lua:"));
  REQUIRE(runtime.mod_catalog().Find("good")->active);
  REQUIRE_FALSE(std::filesystem::exists(tree.root / "profile/mod_order.txt"));
}

TEST_CASE("Lua dispatch reports missing host without running source", "[mod_script]") {
  PackageTree tree;
  tree.Write("unhosted");
  auto catalog = Discover(tree);
  std::ofstream(tree.root / "unhosted/main.lua") << "while true do end";
  rex::Runtime runtime({}, tree.root / "profile");
  runtime.RescanModCatalog();
  DispatchModLaunch(runtime, catalog.packages,
                    [](const auto&) { FAIL("unexpected native callback"); });
  REQUIRE(Message(*runtime.mod_catalog().Find("unhosted"), "support: main.lua: no script host"));
  REQUIRE(runtime.active_mod_states().empty());
}
