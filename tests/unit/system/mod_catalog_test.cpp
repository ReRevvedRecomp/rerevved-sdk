/**
 * @file        mod_catalog_test.cpp
 * @brief       Unit tests for read-only version-1 mod discovery
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rex/platform.h>
#include <rex/system/mod_catalog.h>
#include <rex/system/mod_config.h>

namespace {

class TempDirectory {
 public:
  explicit TempDirectory(std::string_view name) {
    static std::atomic<uint64_t> next_id{0};
    auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (std::string(name) + "_" + std::to_string(suffix) + "_" + std::to_string(next_id++));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

constexpr std::string_view CurrentPlatform() {
#if REX_PLATFORM_WIN32
#if defined(REX_ARCH_ARM64)
  return "windows-arm64";
#else
  return "windows-x64";
#endif
#elif REX_PLATFORM_LINUX
#if defined(REX_ARCH_ARM64)
  return "linux-arm64";
#else
  return "linux-x64";
#endif
#else
#if defined(REX_ARCH_ARM64)
  return "macos-arm64";
#else
  return "macos-x64";
#endif
#endif
}

std::string BinaryName(std::string_view stem) {
  if (CurrentPlatform().starts_with("windows-")) {
    return std::string(stem) + ".dll";
  }
  if (CurrentPlatform().starts_with("macos-")) {
    return "lib" + std::string(stem) + ".dylib";
  }
  return "lib" + std::string(stem) + ".so";
}

#ifndef REXGLUE_BUILD_CONFIG
#define REXGLUE_BUILD_CONFIG "Release"
#endif

constexpr std::string_view CurrentConfigPostfix() {
  constexpr std::string_view config = REXGLUE_BUILD_CONFIG;
  if (config == "Debug") {
    return "d";
  }
  if (config == "RelWithDebInfo") {
    return "rd";
  }
  return "";
}

constexpr std::string_view OtherPlatform() {
  if (CurrentPlatform() == "windows-x64") {
    return "windows-arm64";
  }
  if (CurrentPlatform() == "windows-arm64") {
    return "windows-x64";
  }
  if (CurrentPlatform() == "linux-x64") {
    return "linux-arm64";
  }
  if (CurrentPlatform() == "linux-arm64") {
    return "linux-x64";
  }
  if (CurrentPlatform() == "macos-x64") {
    return "macos-arm64";
  }
  return "macos-x64";
}

std::string BinaryNameForPlatform(std::string_view platform, std::string_view stem) {
  if (platform.starts_with("windows-")) {
    return std::string(stem) + ".dll";
  }
  if (platform.starts_with("macos-")) {
    return "lib" + std::string(stem) + ".dylib";
  }
  return "lib" + std::string(stem) + ".so";
}

std::string BinaryNameWithPostfix(std::string_view stem, std::string_view postfix) {
  return BinaryNameForPlatform(CurrentPlatform(), std::string(stem) + std::string(postfix));
}

void WriteManifest(const std::filesystem::path& package_root, std::string_view id,
                   std::string_view code = "catalog_code", std::string_view extra = {}) {
  std::filesystem::create_directories(package_root / "code" / CurrentPlatform());
  std::ofstream manifest(package_root / "mod.toml", std::ios::binary);
  manifest << "manifest_version = 1\n"
           << extra << "[mod]\nid = \"" << id << "\"\nname = \"Display Name\"\nversion = \"1.2\"\n"
           << "code = \"" << code << "\"\nplugin_abi = 1\n";
  std::ofstream binary(package_root / "code" / CurrentPlatform() / BinaryName(code),
                       std::ios::binary);
  binary << "fixture";
}

bool HasMessage(const rex::system::ModPackage& package, std::string_view needle) {
  return std::any_of(package.diagnostics.begin(), package.diagnostics.end(),
                     [needle](const auto& diagnostic) {
                       return diagnostic.message.find(needle) != std::string::npos;
                     });
}

const rex::system::ModPackage* FindFolder(const rex::system::ModCatalog& catalog,
                                          std::string_view folder) {
  const auto it =
      std::find_if(catalog.packages.begin(), catalog.packages.end(),
                   [folder](const auto& package) { return package.folder_name == folder; });
  return it == catalog.packages.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("version 1 catalog parses metadata and warns on unknown fields", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_valid");
  WriteManifest(temp.path() / "catalog-package", "catalog-package", "catalog_code",
                "unknown_root = true\n");
  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.2.3");

  REQUIRE(catalog.packages.size() == 1);
  const auto& package = catalog.packages.front();
  CHECK_FALSE(package.HasBlockingError());
  CHECK(package.id == "catalog-package");
  CHECK(package.display_name == "Display Name");
  CHECK(package.version == "1.2");
  CHECK(package.plugin_path.filename() == BinaryName("catalog_code"));
  CHECK(HasMessage(package, "unknown manifest field"));
}

TEST_CASE("catalog keeps invalid records visible with blocking diagnostics", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_invalid");
  auto package_root = temp.path() / "folder-name";
  std::filesystem::create_directories(package_root);
  {
    std::ofstream manifest(package_root / "mod.toml", std::ios::binary);
    manifest << "manifest_version = 1\n[mod]\nid = \"different-name\"\nname = \"\"\n"
                "version = \"1.0.0\"\ncode = \"../escape\"\nplugin_abi = 99\n";
  }

  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  REQUIRE(catalog.packages.size() == 1);
  const auto& package = catalog.packages.front();
  CHECK(package.HasBlockingError());
  CHECK(package.status == rex::system::ModPackageStatus::kInvalidManifest);
  CHECK(HasMessage(package, "does not match manifest id"));
  CHECK(HasMessage(package, "major.minor"));
  CHECK(HasMessage(package, "code must be one filename stem"));
}

TEST_CASE("catalog discovery is sorted, one level, and read-only", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_order");
  WriteManifest(temp.path() / "zeta", "zeta", "zeta_code");
  WriteManifest(temp.path() / "alpha", "alpha", "alpha_code");
  std::filesystem::create_directories(temp.path() / "alpha" / "nested");
  std::ofstream nested_manifest(temp.path() / "alpha" / "nested" / "mod.toml", std::ios::binary);
  nested_manifest << "this must not be parsed";
  nested_manifest.close();
  const auto alpha_manifest = temp.path() / "alpha" / "mod.toml";
  const auto alpha_binary =
      temp.path() / "alpha" / "code" / CurrentPlatform() / BinaryName("alpha_code");
  const auto manifest_time_before = std::filesystem::last_write_time(alpha_manifest);
  const auto binary_time_before = std::filesystem::last_write_time(alpha_binary);
  const auto manifest_size_before = std::filesystem::file_size(alpha_manifest);
  const auto binary_size_before = std::filesystem::file_size(alpha_binary);
  const auto before = std::filesystem::last_write_time(temp.path());

  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  const auto after = std::filesystem::last_write_time(temp.path());
  REQUIRE(catalog.packages.size() == 2);
  CHECK(catalog.packages[0].folder_name == "alpha");
  CHECK(catalog.packages[1].folder_name == "zeta");
  CHECK(std::filesystem::exists(temp.path() / "alpha" / "nested" / "mod.toml"));
  CHECK(before == after);
  CHECK(manifest_time_before == std::filesystem::last_write_time(alpha_manifest));
  CHECK(binary_time_before == std::filesystem::last_write_time(alpha_binary));
  CHECK(manifest_size_before == std::filesystem::file_size(alpha_manifest));
  CHECK(binary_size_before == std::filesystem::file_size(alpha_binary));

  std::error_code link_error;
  std::filesystem::create_directory_symlink(temp.path() / "alpha", temp.path() / "linked-alpha",
                                            link_error);
  if (!link_error) {
    auto linked_catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
    CHECK(linked_catalog.packages.size() == 2);
  }
}

TEST_CASE("catalog reports game, ABI, and platform incompatibility", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_compatibility");
  auto game_root = temp.path() / "game-too-new";
  WriteManifest(game_root, "game-too-new", "game_code");
  {
    std::ofstream manifest(game_root / "mod.toml", std::ios::app);
    manifest << "min_game_version = \"2.0.0\"\n";
  }
  auto abi_root = temp.path() / "abi-mismatch";
  WriteManifest(abi_root, "abi-mismatch", "abi_code");
  {
    std::ofstream manifest(abi_root / "mod.toml", std::ios::binary);
    manifest << "manifest_version = 1\n[mod]\nid = \"abi-mismatch\"\nname = \"ABI\"\n"
                "version = \"1.0\"\ncode = \"abi_code\"\nplugin_abi = 9\n";
  }
  auto platform_root = temp.path() / "other-platform";
  const auto other_platform = OtherPlatform();
  std::filesystem::create_directories(platform_root / "code" / other_platform);
  {
    std::ofstream manifest(platform_root / "mod.toml", std::ios::binary);
    manifest << "manifest_version = 1\n[mod]\nid = \"other-platform\"\nname = \"Other\"\n"
                "version = \"1.0\"\ncode = \"other_code\"\nplugin_abi = 1\n";
  }
  {
    std::ofstream other_binary(platform_root / "code" / other_platform /
                                   BinaryNameForPlatform(other_platform, "other_code"),
                               std::ios::binary);
    other_binary << "fixture";
  }
  auto no_payload_root = temp.path() / "no-payload";
  WriteManifest(no_payload_root, "no-payload", "no_payload_code");
  std::filesystem::remove(no_payload_root / "code" / CurrentPlatform() /
                          BinaryName("no_payload_code"));

  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  REQUIRE(catalog.packages.size() == 4);
  CHECK(catalog.Find("game-too-new")->status ==
        rex::system::ModPackageStatus::kIncompatibleGameVersion);
  CHECK(catalog.Find("abi-mismatch")->status == rex::system::ModPackageStatus::kPluginAbiMismatch);
  CHECK(catalog.Find("other-platform")->status ==
        rex::system::ModPackageStatus::kUnsupportedPlatform);
  CHECK(catalog.Find("no-payload")->status == rex::system::ModPackageStatus::kMissingPayload);
}

TEST_CASE("catalog prefers the current qualified loader payload", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_loader_preferred");
  const auto package_root = temp.path() / "preferred-payload";
  WriteManifest(package_root, "preferred-payload", "preferred_code");
  const auto postfix = CurrentConfigPostfix();
  if (!postfix.empty()) {
    std::ofstream current_binary(package_root / "code" / CurrentPlatform() /
                                     BinaryNameWithPostfix("preferred_code", postfix),
                                 std::ios::binary);
    current_binary << "current fixture";
  }

  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  const auto* package = catalog.Find("preferred-payload");
  REQUIRE(package != nullptr);
  CHECK_FALSE(package->HasBlockingError());
  CHECK(package->plugin_path.filename() == BinaryNameWithPostfix("preferred_code", postfix));
}

TEST_CASE("catalog follows the qualified release fallback", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_loader_release_fallback");
  const auto package_root = temp.path() / "release-fallback";
  WriteManifest(package_root, "release-fallback", "fallback_code");

  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  const auto* package = catalog.Find("release-fallback");
  REQUIRE(package != nullptr);
  CHECK_FALSE(package->HasBlockingError());
  CHECK(package->plugin_path.filename() == BinaryName("fallback_code"));
}

TEST_CASE("catalog blocks a flat current postfix that shadows release fallback", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_loader_flat_shadow");
  const auto package_root = temp.path() / "flat-shadow";
  WriteManifest(package_root, "flat-shadow", "shadow_code");
  const auto postfix = CurrentConfigPostfix();
  {
    std::ofstream flat_binary(package_root / "code" / BinaryNameWithPostfix("shadow_code", postfix),
                              std::ios::binary);
    flat_binary << "flat fixture";
  }

  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  const auto* package = catalog.Find("flat-shadow");
  REQUIRE(package != nullptr);
  if (postfix.empty()) {
    CHECK_FALSE(package->HasBlockingError());
    CHECK(package->plugin_path.filename() == BinaryName("shadow_code"));
  } else {
    CHECK(package->HasBlockingError());
    CHECK(package->plugin_path.empty());
    CHECK(HasMessage(*package, "noncanonical path"));
  }
}

TEST_CASE("qualified current nonregular payload blocks release fallback", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_loader_nonregular");
  const auto package_root = temp.path() / "nonregular-payload";
  WriteManifest(package_root, "nonregular-payload", "nonregular_code");
  const auto qualified_path = package_root / "code" / CurrentPlatform() /
                              BinaryNameWithPostfix("nonregular_code", CurrentConfigPostfix());
  std::error_code path_error;
  std::filesystem::remove(qualified_path, path_error);
  std::filesystem::create_directory(qualified_path, path_error);
  REQUIRE_FALSE(path_error);

  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  const auto* package = catalog.Find("nonregular-payload");
  REQUIRE(package != nullptr);
  CHECK(package->HasBlockingError());
  CHECK(package->plugin_path.empty());
}

TEST_CASE("qualified current linked payload blocks release fallback", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_loader_linked");
  const auto package_root = temp.path() / "linked-payload";
  WriteManifest(package_root, "linked-payload", "linked_code");
  const auto qualified_path = package_root / "code" / CurrentPlatform() /
                              BinaryNameWithPostfix("linked_code", CurrentConfigPostfix());
  const auto target_path = temp.path() / BinaryNameWithPostfix("linked_code", "target");
  {
    std::ofstream target(target_path, std::ios::binary);
    target << "linked fixture";
  }
  std::error_code link_error;
  std::filesystem::remove(qualified_path, link_error);
  std::filesystem::create_symlink(target_path, qualified_path, link_error);
  if (link_error) {
    SKIP("symbolic links are unavailable on this host");
  }

  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  const auto* package = catalog.Find("linked-payload");
  REQUIRE(package != nullptr);
  CHECK(package->HasBlockingError());
  CHECK(package->plugin_path.empty());
}

TEST_CASE("invalid folder identity cannot shadow a valid package", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_shadow");
  WriteManifest(temp.path() / "alias", "real-id", "alias_code");
  WriteManifest(temp.path() / "real-id", "real-id", "real_code");
  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");

  const auto* package = catalog.Find("real-id");
  REQUIRE(package != nullptr);
  CHECK(package->folder_name == "real-id");
  CHECK_FALSE(package->HasBlockingError());
}

TEST_CASE("enabled_mods selection preserves exact order", "[mod_catalog]") {
  TempDirectory temp("rex_mod_selection_order");
  WriteManifest(temp.path() / "first", "first", "first_code");
  WriteManifest(temp.path() / "second", "second", "second_code");
  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");

  auto selection = rex::system::SelectEnabledMods(catalog, " second, first ");
  REQUIRE(selection.IsValid());
  REQUIRE(selection.packages.size() == 2);
  CHECK(selection.packages[0].id == "second");
  CHECK(selection.packages[1].id == "first");
}

TEST_CASE("enabled_mods selection reports every blocking entry", "[mod_catalog]") {
  TempDirectory temp("rex_mod_selection_errors");
  WriteManifest(temp.path() / "good", "good", "good_code");
  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");

  auto selection = rex::system::SelectEnabledMods(catalog, "missing,good,good,bad_name");
  CHECK_FALSE(selection.IsValid());
  CHECK(selection.packages.size() == 1);
  REQUIRE(selection.diagnostics.size() == 3);
  CHECK(selection.diagnostics[0].message.find("missing") != std::string::npos);
  CHECK(selection.diagnostics[1].message.find("repeats") != std::string::npos);
  CHECK(selection.diagnostics[2].message.find("bad_name") != std::string::npos);
}

TEST_CASE("catalog blocking diagnostics fail closed for valid requested packages",
          "[mod_catalog]") {
  TempDirectory temp("rex_mod_selection_catalog_error");
  WriteManifest(temp.path() / "good", "good", "good_code");
  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  catalog.diagnostics.push_back({rex::system::ModDiagnosticSeverity::kError, true,
                                 "catalog enumeration failed", catalog.mods_root});

  auto selection = rex::system::SelectEnabledMods(catalog, "good");
  CHECK_FALSE(selection.IsValid());
  REQUIRE(selection.packages.size() == 1);
  REQUIRE(selection.diagnostics.size() == 1);
  CHECK(selection.diagnostics.front().message == "catalog enumeration failed");
}

TEST_CASE("empty enabled_mods selects no packages", "[mod_catalog]") {
  TempDirectory temp("rex_mod_selection_empty");
  WriteManifest(temp.path() / "installed", "installed", "installed_code");
  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");

  auto selection = rex::system::SelectEnabledMods(catalog, "  , , ");
  CHECK(selection.IsValid());
  CHECK(selection.packages.empty());
}

TEST_CASE("package ID and strict version grammar are enforced", "[mod_catalog]") {
  CHECK(rex::system::IsValidModPackageId("a"));
  CHECK(rex::system::IsValidModPackageId("abc-123"));
  CHECK(rex::system::IsValidModPackageId(std::string(63, 'a')));
  CHECK_FALSE(rex::system::IsValidModPackageId(""));
  CHECK_FALSE(rex::system::IsValidModPackageId("-leading"));
  CHECK_FALSE(rex::system::IsValidModPackageId("trailing-"));
  CHECK_FALSE(rex::system::IsValidModPackageId("double--hyphen"));
  CHECK_FALSE(rex::system::IsValidModPackageId("Upper"));
  CHECK_FALSE(rex::system::IsValidModPackageId("under_score"));
  CHECK_FALSE(rex::system::IsValidModPackageId(std::string(64, 'a')));

  TempDirectory temp("rex_mod_catalog_grammar");
  WriteManifest(temp.path() / "valid-version", "valid-version", "valid_version_code");
  const std::array<std::string_view, 10> invalid_versions = {
      "1", "1.0.0", "1..2", ".0", "1.", "1.0-alpha", "", "+1.0", "-1.0", "1.0.2.3"};
  for (size_t index = 0; index < invalid_versions.size(); ++index) {
    const std::string id = "bad-version-" + std::to_string(index);
    const auto package_root = temp.path() / id;
    WriteManifest(package_root, id, "bad_code_" + std::to_string(index));
    std::ofstream manifest(package_root / "mod.toml", std::ios::binary);
    manifest << "manifest_version = 1\n[mod]\nid = \"" << id << "\"\nname = \"Bad\"\nversion = \""
             << invalid_versions[index] << "\"\ncode = \"bad_code_" << index
             << "\"\nplugin_abi = 1\n";
  }
  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  REQUIRE(catalog.packages.size() == invalid_versions.size() + 1);
  const auto* valid_package = catalog.Find("valid-version");
  REQUIRE(valid_package != nullptr);
  CHECK_FALSE(valid_package->HasBlockingError());
  CHECK(valid_package->version == "1.2");
  for (const auto& package : catalog.packages) {
    if (package.id == "valid-version") {
      continue;
    }
    CHECK(package.HasBlockingError());
    CHECK(package.status == rex::system::ModPackageStatus::kInvalidManifest);
    if (package.version.empty()) {
      CHECK(HasMessage(package, "must be nonempty"));
    } else {
      CHECK(HasMessage(package, "major.minor"));
    }
  }
}

TEST_CASE("manifest version and ABI fields require their declared types", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_field_types");
  const std::array<std::pair<std::string_view, std::string_view>, 4> manifests = {
      std::pair<std::string_view, std::string_view>{"bad-manifest-version",
                                                    "manifest_version = 1.0\n"},
      std::pair<std::string_view, std::string_view>{"bad-plugin-abi", "manifest_version = 1\n"},
      std::pair<std::string_view, std::string_view>{"missing-version", "manifest_version = 1\n"},
      std::pair<std::string_view, std::string_view>{"missing-plugin-abi",
                                                    "manifest_version = 1\n"}};
  for (const auto& [id, prefix] : manifests) {
    const auto package_root = temp.path() / id;
    WriteManifest(package_root, id, std::string(id) + "_code");
    std::ofstream manifest(package_root / "mod.toml", std::ios::binary);
    manifest << prefix << "[mod]\nid = \"" << id << "\"\nname = \"Bad\"\n";
    if (id != "missing-version") {
      manifest << "version = \"1.0\"\n";
    }
    manifest << "code = \"" << id << "_code\"\n";
    if (id == "bad-plugin-abi") {
      manifest << "plugin_abi = 1.0\n";
    } else if (id != "missing-plugin-abi") {
      manifest << "plugin_abi = 1\n";
    }
  }
  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  REQUIRE(catalog.packages.size() == manifests.size());
  for (const auto& package : catalog.packages) {
    CHECK(package.HasBlockingError());
    CHECK(package.status == rex::system::ModPackageStatus::kInvalidManifest);
  }
}

TEST_CASE("disabled invalid packages stay visible but selected invalid packages block",
          "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_disabled_invalid");
  const auto package_root = temp.path() / "invalid-package";
  std::filesystem::create_directories(package_root);
  {
    std::ofstream manifest(package_root / "mod.toml", std::ios::binary);
    manifest << "manifest_version = 1\n[mod]\nid = \"invalid-package\"\nname = \"Invalid\"\n"
                "version = \"1.0.0\"\ncode = \"invalid_code\"\nplugin_abi = 1\n";
  }
  WriteManifest(temp.path() / "valid-package", "valid-package", "valid_code");
  auto catalog = rex::system::DiscoverModCatalog(temp.path(), "1.0.0");
  REQUIRE(catalog.packages.size() == 2);
  REQUIRE(catalog.Find("invalid-package") != nullptr);
  CHECK(catalog.Find("invalid-package")->HasBlockingError());
  CHECK(catalog.Find("invalid-package")->status == rex::system::ModPackageStatus::kInvalidManifest);
  CHECK(rex::system::SelectEnabledMods(catalog, "").IsValid());
  CHECK(rex::system::SelectEnabledMods(catalog, "valid-package").IsValid());
  CHECK_FALSE(rex::system::SelectEnabledMods(catalog, "invalid-package").IsValid());
}

TEST_CASE("catalog rejects linked package paths without traversing them", "[mod_catalog]") {
  TempDirectory temp("rex_mod_catalog_links");
  const auto mods_root = temp.path() / "mods";
  const auto target_root = temp.path() / "targets";
  std::filesystem::create_directories(mods_root);
  std::filesystem::create_directories(target_root);
  const auto target_manifest = target_root / "target-mod.toml";
  {
    std::ofstream manifest(target_manifest, std::ios::binary);
    manifest << "manifest_version = 1\n[mod]\nid = \"linked-manifest\"\nname = \"Linked\"\n"
                "version = \"1.0\"\ncode = \"linked_code\"\nplugin_abi = 1\n";
  }

  WriteManifest(mods_root / "linked-manifest", "linked-manifest", "linked_code");
  std::error_code link_error;
  std::filesystem::remove(mods_root / "linked-manifest" / "mod.toml", link_error);
  std::filesystem::create_symlink(target_manifest, mods_root / "linked-manifest" / "mod.toml",
                                  link_error);
  if (link_error) {
    SKIP("symbolic links are unavailable on this host");
  }

  WriteManifest(mods_root / "linked-code", "linked-code", "linked_code");
  const auto target_code = target_root / "code-target";
  std::filesystem::create_directories(target_code / CurrentPlatform());
  std::ofstream(target_code / CurrentPlatform() / BinaryName("linked_code")) << "fixture";
  std::filesystem::remove_all(mods_root / "linked-code" / "code");
  std::filesystem::create_directory_symlink(target_code, mods_root / "linked-code" / "code",
                                            link_error);
  if (link_error) {
    SKIP("directory symbolic links are unavailable on this host");
  }

  WriteManifest(mods_root / "linked-platform", "linked-platform", "linked_code");
  const auto target_platform = target_root / "platform-target";
  std::filesystem::create_directories(target_platform);
  std::ofstream(target_platform / BinaryName("linked_code")) << "fixture";
  std::filesystem::remove_all(mods_root / "linked-platform" / "code" / CurrentPlatform());
  std::filesystem::create_directory_symlink(
      target_platform, mods_root / "linked-platform" / "code" / CurrentPlatform(), link_error);
  if (link_error) {
    SKIP("directory symbolic links are unavailable on this host");
  }

  WriteManifest(mods_root / "linked-binary", "linked-binary", "linked_code");
  const auto target_binary = target_root / "binary-target";
  std::ofstream(target_binary, std::ios::binary) << "fixture";
  std::filesystem::remove(mods_root / "linked-binary" / "code" / CurrentPlatform() /
                          BinaryName("linked_code"));
  std::filesystem::create_symlink(
      target_binary,
      mods_root / "linked-binary" / "code" / CurrentPlatform() / BinaryName("linked_code"),
      link_error);
  if (link_error) {
    SKIP("file symbolic links are unavailable on this host");
  }

  auto catalog = rex::system::DiscoverModCatalog(mods_root, "1.0.0");
  REQUIRE(catalog.packages.size() == 4);
  const auto* manifest_link = FindFolder(catalog, "linked-manifest");
  REQUIRE(manifest_link != nullptr);
  CHECK(manifest_link->status == rex::system::ModPackageStatus::kInvalidManifest);
  CHECK(catalog.Find("linked-code")->status == rex::system::ModPackageStatus::kMissingPayload);
  CHECK(catalog.Find("linked-platform")->status == rex::system::ModPackageStatus::kMissingPayload);
  CHECK(catalog.Find("linked-binary")->status == rex::system::ModPackageStatus::kMissingPayload);

  std::filesystem::create_directory_symlink(mods_root, temp.path() / "mods-link", link_error);
  if (link_error) {
    SKIP("root directory symbolic links are unavailable on this host");
  }
  CHECK(rex::system::DiscoverModCatalog(temp.path() / "mods-link", "1.0.0").packages.empty());
}
