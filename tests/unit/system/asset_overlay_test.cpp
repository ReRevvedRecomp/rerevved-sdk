/**
 * @file        asset_overlay_test.cpp
 * @brief       Unit tests for separate asset-overlay packages and loadouts
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/asset_overlay.h>
#include <rex/system/asset_overlay_catalog.h>
#include <rex/system/asset_overlay_loadout.h>

namespace {

class TempDirectory {
 public:
  explicit TempDirectory(std::string_view name) {
    static std::atomic<uint64_t> next_id{0};
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
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

void WritePack(const std::filesystem::path& root, std::string_view id,
               std::string_view name = "Asset Pack") {
  std::filesystem::create_directories(root / "assets" / "ui");
  std::ofstream manifest(root / "asset-pack.toml", std::ios::binary);
  manifest << "manifest_version = 1\n[asset_pack]\nid = \"" << id << "\"\nname = \"" << name
           << "\"\nversion = \"1.0\"\n";
  std::ofstream(root / "assets" / "ui" / "logo.dds", std::ios::binary) << id;
}

}  // namespace

TEST_CASE("asset overlay catalog is separate and validates safe nonempty assets",
          "[asset_overlay]") {
  TempDirectory temp("rex_asset_overlay_catalog");
  WritePack(temp.path() / "high", "high");
  WritePack(temp.path() / "low", "low");
  const auto empty = temp.path() / "empty";
  std::filesystem::create_directories(empty / "assets");
  std::ofstream(empty / "asset-pack.toml", std::ios::binary)
      << "manifest_version = 1\n[asset_pack]\nid = \"empty\"\nname = \"Empty\"\n"
         "version = \"1.0\"\n";

  const auto catalog = rex::system::DiscoverAssetOverlayCatalog(temp.path());
  REQUIRE(catalog.packages.size() == 3);
  CHECK(catalog.Find("high")->assets_root.filename() == "assets");
  CHECK(catalog.Find("empty")->assets_root.filename() == "assets");
  CHECK(std::any_of(catalog.Find("empty")->diagnostics.begin(),
                    catalog.Find("empty")->diagnostics.end(),
                    [](const auto& diagnostic) { return diagnostic.blocking; }));
  CHECK_FALSE(std::any_of(catalog.Find("high")->diagnostics.begin(),
                          catalog.Find("high")->diagnostics.end(),
                          [](const auto& diagnostic) { return diagnostic.blocking; }));
  CHECK_FALSE(rex::system::IsValidAssetOverlayPackageId("Upper"));
}

TEST_CASE("asset overlay resolver uses top priority and reports shadowed packs",
          "[asset_overlay]") {
  TempDirectory temp("rex_asset_overlay_resolve");
  WritePack(temp.path() / "high", "high");
  WritePack(temp.path() / "low", "low");
  const auto catalog = rex::system::DiscoverAssetOverlayCatalog(temp.path());
  const std::vector<rex::system::AssetOverlayPackage> packages = {*catalog.Find("high"),
                                                                  *catalog.Find("low")};
  const auto result = rex::system::ResolveAssetOverlay(packages, "ui/logo.dds", 64);
  REQUIRE(result.has_value());
  CHECK(result->package_id == "high");
  CHECK(std::string(result->bytes.begin(), result->bytes.end()) == "high");
  REQUIRE(result->shadowed_package_ids.size() == 1);
  CHECK(result->shadowed_package_ids.front() == "low");
}

TEST_CASE("asset overlay resolver rejects unsafe and oversized winners", "[asset_overlay]") {
  TempDirectory temp("rex_asset_overlay_safety");
  WritePack(temp.path() / "high", "high");
  WritePack(temp.path() / "low", "low");
  const auto catalog = rex::system::DiscoverAssetOverlayCatalog(temp.path());
  const std::vector<rex::system::AssetOverlayPackage> packages = {*catalog.Find("high"),
                                                                  *catalog.Find("low")};
  const auto oversized = rex::system::ResolveAssetOverlay(packages, "ui/logo.dds", 2);
  REQUIRE_FALSE(oversized.has_value());
  CHECK(oversized.error().category == rex::ErrorCategory::Validation);
  const auto missing = rex::system::ResolveAssetOverlay(packages, "ui/missing.dds", 64);
  CHECK(missing.error().category == rex::ErrorCategory::NotFound);
  for (const auto key : {"", "/ui/logo.dds", "../ui/logo.dds", "ui//logo.dds", "ui\\logo.dds",
                         "C:/ui/logo.dds", "ui/./logo.dds"}) {
    const auto invalid = rex::system::ResolveAssetOverlay(packages, key, 64);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().category == rex::ErrorCategory::Validation);
  }
}

TEST_CASE("asset overlay loadout is profile-local and atomic", "[asset_overlay]") {
  TempDirectory temp("rex_asset_overlay_loadout");
  WritePack(temp.path() / "high", "high");
  WritePack(temp.path() / "low", "low");
  const auto catalog = rex::system::DiscoverAssetOverlayCatalog(temp.path());
  const auto first = std::vector<std::string>{"high", "low"};
  REQUIRE(
      rex::system::ApplyAssetOverlayLoadout(temp.path() / "profile", catalog, first).succeeded());
  const auto read = rex::system::ReadAssetOverlayLoadout(temp.path() / "profile");
  REQUIRE(read.entries.size() == 2);
  CHECK(read.entries[0].id == "high");
  CHECK(read.entries[1].id == "low");
  CHECK(rex::system::SelectAssetOverlayLoadout(catalog, read).IsValid());
  CHECK(std::filesystem::exists(temp.path() / "profile" / "asset_order.txt"));

  const auto invalid_profile = temp.path() / "invalid-profile";
  std::filesystem::create_directories(invalid_profile);
  std::ofstream(invalid_profile / "asset_order.txt", std::ios::binary) << "missing\n";
  const auto blocked = rex::system::ApplyAssetOverlayLoadout(invalid_profile, catalog, first);
  CHECK(blocked.status == rex::system::AssetOverlayLoadoutApplyStatus::kInvalidCurrent);
  const auto replaced =
      rex::system::ApplyAssetOverlayLoadout(invalid_profile, catalog, first, true);
  REQUIRE(replaced.succeeded());
}

TEST_CASE("asset overlay loadout falls back to the bundled default order", "[asset_overlay]") {
  TempDirectory temp("rex_asset_overlay_default");
  const auto overlays = temp.path() / "asset-overrides";
  WritePack(overlays / "high", "high");
  WritePack(overlays / "low", "low");
  std::ofstream(overlays / "default_asset_order.txt", std::ios::binary) << "low\nhigh\n";

  const auto catalog = rex::system::DiscoverAssetOverlayCatalog(overlays);
  const auto read = rex::system::ReadAssetOverlayLoadout(temp.path() / "profile", overlays);
  REQUIRE_FALSE(read.exists);
  CHECK(read.uses_bundled_default);
  CHECK(read.source_path == overlays / "default_asset_order.txt");
  REQUIRE(read.entries.size() == 2);
  CHECK(read.entries[0].id == "low");
  CHECK(read.entries[1].id == "high");

  const auto selection = rex::system::SelectAssetOverlayLoadout(catalog, read);
  REQUIRE(selection.IsValid());
  REQUIRE(selection.packages.size() == 2);
  CHECK(selection.packages[0].id == "low");
  CHECK(selection.packages[1].id == "high");
}

TEST_CASE("profile asset overlay order takes precedence over bundled default", "[asset_overlay]") {
  TempDirectory temp("rex_asset_overlay_profile_precedence");
  const auto overlays = temp.path() / "asset-overrides";
  WritePack(overlays / "high", "high");
  WritePack(overlays / "low", "low");
  std::ofstream(overlays / "default_asset_order.txt", std::ios::binary) << "low\nhigh\n";
  const auto profile = temp.path() / "profile";
  std::filesystem::create_directories(profile);
  std::ofstream(profile / "asset_order.txt", std::ios::binary) << "high\n";

  const auto read = rex::system::ReadAssetOverlayLoadout(profile, overlays);
  REQUIRE(read.exists);
  CHECK_FALSE(read.uses_bundled_default);
  CHECK(read.source_path == profile / "asset_order.txt");
  REQUIRE(read.entries.size() == 1);
  CHECK(read.entries.front().id == "high");
}

TEST_CASE("an explicit empty profile asset order disables bundled defaults", "[asset_overlay]") {
  TempDirectory temp("rex_asset_overlay_empty_profile");
  const auto overlays = temp.path() / "asset-overrides";
  WritePack(overlays / "high", "high");
  std::ofstream(overlays / "default_asset_order.txt", std::ios::binary) << "high\n";
  const auto profile = temp.path() / "profile";
  std::filesystem::create_directories(profile);
  std::ofstream(profile / "asset_order.txt", std::ios::binary);

  const auto catalog = rex::system::DiscoverAssetOverlayCatalog(overlays);
  const auto read = rex::system::ReadAssetOverlayLoadout(profile, overlays);
  REQUIRE(read.exists);
  CHECK_FALSE(read.uses_bundled_default);
  CHECK(read.entries.empty());
  const auto selection = rex::system::SelectAssetOverlayLoadout(catalog, read);
  CHECK(selection.IsValid());
  CHECK(selection.packages.empty());
}

TEST_CASE("malformed bundled asset order reports diagnostics", "[asset_overlay]") {
  TempDirectory temp("rex_asset_overlay_bad_default");
  const auto overlays = temp.path() / "asset-overrides";
  WritePack(overlays / "high", "high");
  std::ofstream(overlays / "default_asset_order.txt", std::ios::binary) << "missing\nmissing\n";

  const auto catalog = rex::system::DiscoverAssetOverlayCatalog(overlays);
  const auto read = rex::system::ReadAssetOverlayLoadout(temp.path() / "profile", overlays);
  REQUIRE_FALSE(read.exists);
  REQUIRE(read.uses_bundled_default);
  const auto selection = rex::system::SelectAssetOverlayLoadout(catalog, read);
  CHECK_FALSE(selection.IsValid());
  CHECK(std::any_of(
      selection.diagnostics.begin(), selection.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.message.find("default_asset_order.txt") != std::string::npos;
      }));
}
