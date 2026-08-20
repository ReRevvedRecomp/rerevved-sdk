/**
 * @file        mod_dependency_test.cpp
 * @brief       Unit tests for mod manifest and version validation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

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

void WriteMod(const std::filesystem::path& mods_root, std::string_view name,
              std::string_view version = {}, std::string_view requires_list = {},
              std::string_view game_version = {}) {
  auto path = mods_root / name / "mod.toml";
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << "name = \"" << name << "\"\n";
  if (!version.empty()) {
    file << "version = \"" << version << "\"\n";
  }
  if (!requires_list.empty()) {
    file << "requires = \"" << requires_list << "\"\n";
  }
  if (!game_version.empty()) {
    file << "game_version = \"" << game_version << "\"\n";
  }
}

bool ValidateMods(const std::filesystem::path& root, std::string_view enabled,
                  std::string_view game_version = {}) {
  auto mods_root = root / "mods";
  auto mods = rex::system::ResolveModConfiguration(mods_root, enabled);
  return rex::system::ValidateModConfiguration(mods, game_version);
}

}  // namespace

TEST_CASE("mod dependencies accept a satisfied minimum version", "[mod_dependency]") {
  TempDirectory temp("rex_mod_dependency_ok");
  auto mods = temp.path() / "mods";
  WriteMod(mods, "game_api", "1.2.0");
  WriteMod(mods, "inspector", "1.0.0", "game_api >= 1.0.0");

  CHECK(ValidateMods(temp.path(), "game_api,inspector", "1.0.0"));
}

TEST_CASE("mod dependencies reject an older required mod", "[mod_dependency]") {
  TempDirectory temp("rex_mod_dependency_old");
  auto mods = temp.path() / "mods";
  WriteMod(mods, "game_api", "0.9.0");
  WriteMod(mods, "inspector", "1.0.0", "game_api >= 1.0.0");

  CHECK_FALSE(ValidateMods(temp.path(), "game_api,inspector", "1.0.0"));
}

TEST_CASE("missing unordered dependencies diagnose without blocking startup", "[mod_dependency]") {
  TempDirectory temp("rex_mod_dependency_missing");
  auto mods = temp.path() / "mods";
  WriteMod(mods, "inspector", "1.0.0", "game_api");

  CHECK(ValidateMods(temp.path(), "inspector", "1.0.0"));
}

TEST_CASE("mod game version accepts an equal host", "[mod_dependency]") {
  TempDirectory temp("rex_mod_game_version_ok");
  WriteMod(temp.path() / "mods", "inspector", "1.0.0", {}, ">= 1.2.0");

  CHECK(ValidateMods(temp.path(), "inspector", "1.2.0"));
}

TEST_CASE("mod game version rejects an older host", "[mod_dependency]") {
  TempDirectory temp("rex_mod_game_version_old");
  WriteMod(temp.path() / "mods", "inspector", "1.0.0", {}, "1.2.0");

  CHECK_FALSE(ValidateMods(temp.path(), "inspector", "1.1.0"));
}

TEST_CASE("enabled mod names cannot escape the configured root", "[mod_dependency]") {
  TempDirectory temp("rex_mod_path_escape");
  std::filesystem::create_directories(temp.path() / "mods");

  auto mods = rex::system::ResolveModConfiguration(temp.path() / "mods", "..");
  CHECK(mods.empty());
}
