

/**
 * @file        mod_loadout_test.cpp
 * @brief       Unit tests for profile-local ordered mod loadouts
 */

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/mod_loadout.h>

namespace {

namespace fs = std::filesystem;

class TempDirectory {
 public:
  explicit TempDirectory(std::string name) {
    static std::atomic<uint64_t> next_id{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() / (std::move(name) + "_" + std::to_string(stamp) + "_" +
                                         std::to_string(next_id.fetch_add(1)));
    fs::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void WriteFile(const fs::path& path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(output);
}

std::string ReadFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool HasDiagnostic(const rex::system::ModLoadoutSelection& selection, std::string_view needle) {
  return std::any_of(selection.diagnostics.begin(), selection.diagnostics.end(),
                     [needle](const auto& diagnostic) {
                       return diagnostic.message.find(needle) != std::string::npos;
                     });
}

rex::system::ModCatalog Catalog(std::initializer_list<std::string_view> ids) {
  rex::system::ModCatalog catalog;
  for (const auto id : ids) {
    rex::system::ModPackage package;
    package.id = id;
    package.folder_name = id;
    package.status = rex::system::ModPackageStatus::kReady;
    catalog.packages.push_back(std::move(package));
  }
  return catalog;
}

}  // namespace

TEST_CASE("mod_order parser trims IDs and reports exact blocking lines", "[mod_loadout]") {
  TempDirectory temp("rex_mod_order_parse");
  WriteFile(temp.path() / "mod_order.txt",
            "\n  # ignored\n  first  \ninvalid_name\nfirst\n  second\r\n");

  const auto loadout = rex::system::ReadModLoadout(temp.path());
  REQUIRE(loadout.entries.size() == 4);
  CHECK(loadout.entries[0].id == "first");
  CHECK(loadout.entries[0].line == 3);
  CHECK(loadout.entries[1].id == "invalid_name");
  CHECK(loadout.entries[1].line == 4);
  CHECK(loadout.entries[2].line == 5);
  CHECK(loadout.entries[3].id == "second");
  CHECK(loadout.entries[3].line == 6);
  CHECK_FALSE(loadout.diagnostics.empty());
  REQUIRE(loadout.diagnostics.size() == 2);
  CHECK(loadout.diagnostics[0].line == 4);
  CHECK(loadout.diagnostics[1].line == 5);
  CHECK(loadout.diagnostics[0].message.find("invalid_name") != std::string::npos);
  CHECK(loadout.diagnostics[1].message.find("repeats package ID 'first'") != std::string::npos);
}

TEST_CASE("missing and explicit empty loadouts are valid and isolated by profile root",
          "[mod_loadout]") {
  TempDirectory temp("rex_mod_order_profiles");
  const auto default_root = temp.path() / "user";
  const auto named_root = default_root / "profiles" / "alpha";
  CHECK(rex::system::ReadModLoadout(default_root).diagnostics.empty());
  CHECK_FALSE(rex::system::ReadModLoadout(default_root).exists);
  fs::create_directories(named_root);
  WriteFile(named_root / "mod_order.txt", "");
  const auto named = rex::system::ReadModLoadout(named_root);
  CHECK(named.exists);
  CHECK(named.diagnostics.empty());
  CHECK(named.entries.empty());
  CHECK_FALSE(fs::exists(default_root / "mod_order.txt"));
}

TEST_CASE("selection reports every malformed, missing, duplicate, and invalid package",
          "[mod_loadout]") {
  TempDirectory temp("rex_mod_order_selection");
  WriteFile(temp.path() / "mod_order.txt", "missing\ngood\ngood\nbad_name\n");
  auto catalog = Catalog({"good"});
  auto selection = rex::system::SelectModLoadout(catalog, rex::system::ReadModLoadout(temp.path()));
  CHECK_FALSE(selection.IsValid());
  CHECK(selection.requested_ids.size() == 4);
  CHECK(selection.packages.size() == 1);
  REQUIRE(selection.diagnostics.size() == 3);
  CHECK(HasDiagnostic(selection, "missing package 'missing'"));
  CHECK(HasDiagnostic(selection, "repeats package ID 'good'"));
  CHECK(HasDiagnostic(selection, "invalid package ID 'bad_name'"));
}

TEST_CASE("apply writes an atomic ordered file and supports an explicit empty file",
          "[mod_loadout]") {
  TempDirectory temp("rex_mod_order_apply");
  const auto profile = temp.path() / "profiles" / "alpha";
  auto catalog = Catalog({"first", "second"});
  std::vector<std::string> ids = {"second", "first"};

  WriteFile(profile / ".mod_order.txt.tmp", "stale");
  const auto applied = rex::system::ApplyModLoadout(profile, catalog, ids);
  REQUIRE(applied.succeeded());
  CHECK(ReadFile(profile / "mod_order.txt") == "second\nfirst\n");
  CHECK_FALSE(fs::exists(profile / ".mod_order.txt.tmp"));

  ids.clear();
  REQUIRE(rex::system::ApplyModLoadout(profile, catalog, ids).succeeded());
  CHECK(fs::exists(profile / "mod_order.txt"));
  CHECK(ReadFile(profile / "mod_order.txt").empty());
}

TEST_CASE("invalid current file requires explicit replacement confirmation", "[mod_loadout]") {
  TempDirectory temp("rex_mod_order_replace");
  const auto profile = temp.path() / "profiles" / "alpha";
  WriteFile(profile / "mod_order.txt", "bad_name\n");
  auto catalog = Catalog({"good"});
  std::vector<std::string> ids = {"good"};

  const auto blocked = rex::system::ApplyModLoadout(profile, catalog, ids);
  CHECK(blocked.status == rex::system::ModLoadoutApplyStatus::kInvalidCurrent);
  CHECK(ReadFile(profile / "mod_order.txt") == "bad_name\n");

  const auto replaced = rex::system::ApplyModLoadout(profile, catalog, ids, true);
  REQUIRE(replaced.succeeded());
  CHECK(ReadFile(profile / "mod_order.txt") == "good\n");
}

TEST_CASE("current file with a missing package also requires replacement confirmation",
          "[mod_loadout]") {
  TempDirectory temp("rex_mod_order_missing_current");
  const auto profile = temp.path() / "profiles" / "alpha";
  WriteFile(profile / "mod_order.txt", "removed-package\n");
  auto catalog = Catalog({"good"});
  std::vector<std::string> ids = {"good"};

  const auto blocked = rex::system::ApplyModLoadout(profile, catalog, ids);
  CHECK(blocked.status == rex::system::ModLoadoutApplyStatus::kInvalidCurrent);
  CHECK(ReadFile(profile / "mod_order.txt") == "removed-package\n");
}

TEST_CASE("staged IDs and order remain visible when a rescan loses a package", "[mod_loadout]") {
  auto catalog = Catalog({"second"});
  const std::vector<std::string> staged = {"removed-package", "second"};
  const auto selection = rex::system::ValidateModLoadout(catalog, staged);
  REQUIRE(selection.requested_ids.size() == 2);
  CHECK(selection.requested_ids[0] == "removed-package");
  CHECK(selection.requested_ids[1] == "second");
  CHECK(selection.packages.size() == 1);
  CHECK_FALSE(selection.IsValid());
}

TEST_CASE("apply does not mutate the running catalog state", "[mod_loadout]") {
  TempDirectory temp("rex_mod_order_restart");
  auto catalog = Catalog({"good"});
  catalog.packages.front().active = true;
  const std::vector<std::string> staged = {"good"};
  REQUIRE(rex::system::ApplyModLoadout(temp.path(), catalog, staged).succeeded());
  CHECK(catalog.packages.front().active);
}
