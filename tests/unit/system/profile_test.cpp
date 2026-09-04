#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/runtime.h>
#include <rex/system/profile.h>
#include <rex/system/xam/content_manager.h>

namespace {

namespace fs = std::filesystem;

class TempDirectory {
 public:
  explicit TempDirectory(std::string_view name) {
    static std::atomic<uint64_t> next_id{0};
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() / (std::string(name) + "_" + std::to_string(suffix) + "_" +
                                         std::to_string(next_id.fetch_add(1)));
    fs::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void WriteFile(const fs::path& path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  REQUIRE(output);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(output);
}

std::string ReadFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

fs::path PackageRoot(const fs::path& root, std::string_view type) {
  return root / "B13EBABEBABEBABE" / "1234ABCD" / fs::path(type);
}

}  // namespace

TEST_CASE("profile IDs accept only canonical lowercase names", "[profile]") {
  const std::vector<std::pair<std::string_view, bool>> cases = {
      {"alpha", true},
      {"a0", true},
      {"a_profile-2", true},
      {"", false},
      {"default", false},
      {"Alpha", false},
      {"1alpha", true},
      {"-alpha", false},
      {"/alpha", false},
      {"C:/alpha", false},
      {"alpha.beta", false},
      {"alpha/beta", false},
      {"alpha\\beta", false},
      {"alpha:beta", false},
      {"alpha.", false},
      {"alpha ", false},
      {"con", false},
      {"prn", false},
      {"com1", false},
      {"lpt9", false},
      {"abcdefghijklmnopqrstuvwxyzabcdefg", false},
  };
  for (const auto& [id, valid] : cases) {
    CHECK(rex::system::IsValidProfileId(id) == valid);
  }
}

TEST_CASE("profile resolution is confined and does not create paths", "[profile]") {
  TempDirectory temp("rex_profile_resolve");
  const auto base = temp.path() / "user";
  const std::vector<std::pair<std::string_view, bool>> cases = {
      {"", true}, {"default", true}, {"alpha", true}, {"Alpha", false}, {"../alpha", false}};

  for (const auto& [id, valid] : cases) {
    const auto resolved = rex::system::ResolveProfile(base, id);
    REQUIRE(static_cast<bool>(resolved) == valid);
    if (!resolved) {
      continue;
    }
    if (id.empty() || id == "default") {
      CHECK(resolved->active_root == base);
      CHECK(resolved->is_default());
    } else {
      CHECK(resolved->active_root == base / "profiles" / fs::path(id));
      CHECK_FALSE(resolved->is_default());
    }
  }
  CHECK_FALSE(fs::exists(base));
}

TEST_CASE("profile roots preserve legacy equality and named separation", "[profile]") {
  TempDirectory temp("rex_profile_roots");
  const auto base = temp.path() / "user";

  const auto default_profile = rex::system::ResolveProfile(base, "default");
  const auto alpha = rex::system::ResolveProfile(base, "alpha");
  const auto beta = rex::system::ResolveProfile(base, "beta");
  REQUIRE(default_profile);
  REQUIRE(alpha);
  REQUIRE(beta);
  CHECK(default_profile->base_root == default_profile->active_root);
  CHECK(alpha->active_root == base / "profiles" / "alpha");
  CHECK(beta->active_root == base / "profiles" / "beta");
  CHECK(alpha->active_root != beta->active_root);

  rex::Runtime legacy_runtime(temp.path() / "game", base);
  CHECK(legacy_runtime.base_user_data_root() == base);
  CHECK(legacy_runtime.active_user_data_root() == base);

  rex::Runtime named_runtime(temp.path() / "game", base, alpha->active_root, {}, {}, {});
  CHECK(named_runtime.base_user_data_root() == base);
  CHECK(named_runtime.active_user_data_root() == alpha->active_root);
  CHECK_FALSE(fs::exists(base));
}

TEST_CASE("profile content roots share only marketplace packages and headers", "[profile]") {
  TempDirectory temp("rex_profile_content_roots");
  const auto base = temp.path() / "base";
  const auto active = base / "profiles" / "alpha";
  constexpr uint32_t title_id = 0x1234ABCD;
  const auto title = std::string("1234ABCD");
  const auto baseline_xuid = std::string("B13EBABEBABEBABE");
  const auto zero_xuid = std::string("0000000000000000");

  rex::system::xam::ContentManager manager(nullptr, base, active);

  rex::system::xam::XCONTENT_AGGREGATE_DATA marketplace{};
  marketplace.content_type = rex::system::XContentType::kMarketplaceContent;
  marketplace.title_id = title_id;
  marketplace.xuid = rex::system::kBaselineProfileXuid;
  marketplace.set_file_name("shared-dlc");

  rex::system::xam::XCONTENT_AGGREGATE_DATA saved_game{};
  saved_game.content_type = rex::system::XContentType::kSavedGame;
  saved_game.title_id = title_id;
  saved_game.xuid = rex::system::kBaselineProfileXuid;
  saved_game.set_file_name("profile-save");

  const auto marketplace_package = base / zero_xuid / title / "00000002" / "shared-dlc";
  const auto saved_package = active / baseline_xuid / title / "00000001" / "profile-save";
  fs::create_directories(marketplace_package);
  fs::create_directories(saved_package);
  fs::create_directories(active / zero_xuid / title / "00000002" / "shared-dlc");
  fs::create_directories(base / baseline_xuid / title / "00000001" / "profile-save");

  CHECK(manager
            .ListContent(1, rex::system::kBaselineProfileXuid,
                         rex::system::XContentType::kMarketplaceContent, title_id)
            .empty());
  const auto shared_marketplace =
      manager.ListContent(1, 0, rex::system::XContentType::kMarketplaceContent, title_id);
  CHECK(shared_marketplace.size() == 1);

  const auto profile_saves = manager.ListContent(1, rex::system::kBaselineProfileXuid,
                                                 rex::system::XContentType::kSavedGame, title_id);
  CHECK(profile_saves.size() == 1);

  CHECK(manager.ContentExists(rex::system::kBaselineProfileXuid, marketplace));
  CHECK(manager.ContentExists(rex::system::kBaselineProfileXuid, saved_game));

  fs::remove_all(marketplace_package);
  fs::remove_all(saved_package);
  CHECK_FALSE(manager.ContentExists(rex::system::kBaselineProfileXuid, marketplace));
  CHECK_FALSE(manager.ContentExists(rex::system::kBaselineProfileXuid, saved_game));

  REQUIRE(manager.WriteContentHeaderFile(rex::system::kBaselineProfileXuid, marketplace) ==
          rex::X_RESULT(0));
  REQUIRE(manager.WriteContentHeaderFile(rex::system::kBaselineProfileXuid, saved_game) ==
          rex::X_RESULT(0));

  CHECK(fs::exists(base / zero_xuid / title / "Headers" / "00000002" / "shared-dlc.header"));
  CHECK_FALSE(
      fs::exists(active / zero_xuid / title / "Headers" / "00000002" / "shared-dlc.header"));
  CHECK(
      fs::exists(active / baseline_xuid / title / "Headers" / "00000001" / "profile-save.header"));
  CHECK_FALSE(
      fs::exists(base / baseline_xuid / title / "Headers" / "00000001" / "profile-save.header"));
}

TEST_CASE("profile copy rejects invalid specifications", "[profile]") {
  TempDirectory temp("rex_profile_invalid_spec");
  const auto base = temp.path() / "user";
  WriteFile(base / "rerevved.toml", "source remains\n");

  const auto resolved = rex::system::ResolveProfile(base, "alpha");
  REQUIRE(resolved);
  const std::vector<rex::system::ProfileCopySpecification> cases = {
      {fs::path{}, 0x1234ABCD},
      {fs::path("../rerevved.toml"), 0x1234ABCD},
      {temp.path() / "absolute.toml", 0x1234ABCD},
      {fs::path("C:/rerevved.toml"), 0x1234ABCD},
      {fs::path("rerevved.toml"), 0},
  };
  for (const auto& specification : cases) {
    CHECK(rex::system::CopyFromDefault(*resolved, specification) ==
          rex::system::ProfileCopyResult::kInvalidSpecification);
    CHECK_FALSE(fs::exists(resolved->active_root));
  }
  CHECK(ReadFile(base / "rerevved.toml") == "source remains\n");
}

TEST_CASE("profile copy publishes the exact SDK allowlist once", "[profile]") {
  TempDirectory temp("rex_profile_copy");
  const auto base = temp.path() / "user";
  const auto title = std::string("1234ABCD");

  WriteFile(base / "rerevved.toml", "fullscreen = false\n");
  WriteFile(PackageRoot(base, "00000001") / "save-one" / "content.bin", "save bytes");
  WriteFile(base / "B13EBABEBABEBABE" / title / "Headers" / "00000001" / "save-one.header",
            "header bytes");
  WriteFile(PackageRoot(base, "00000002") / "market-only" / "content.bin", "market bytes");
  WriteFile(base / "B13EBABEBABEBABE" / title / "Headers" / "00000002" / "market-only.header",
            "market header");
  WriteFile(base / "0000000000000000" / title / "00000002" / "shared-dlc" / "content.bin",
            "shared marketplace bytes");
  WriteFile(base / "0000000000000000" / title / "Headers" / "00000002" / "shared-dlc.header",
            "shared marketplace header");
  WriteFile(base / title / "profile" / "User" / "settings.bin", "profile bytes");
  WriteFile(base / "achievements" / (title + ".toml"), "achievement bytes");
  WriteFile(base / "mod-loadout.toml", "mod-loadout bytes");

  // These sentinels exercise the non-recursive allowlist boundary.
  WriteFile(base / "unrelated.sentinel", "do not copy");
  WriteFile(base / "logs" / "runtime.log", "do not copy");
  WriteFile(base / "cache" / "shader.bin", "do not copy");
  WriteFile(base / "mods" / "provider" / "mod.toml", "do not copy");
  WriteFile(base / "profiles" / "existing" / "sentinel", "do not copy");
  WriteFile(base / "metadata" / "title.toml", "do not copy");

  const auto resolved = rex::system::ResolveProfile(base, "alpha");
  REQUIRE(resolved);
  const rex::system::ProfileCopySpecification specification{fs::path("rerevved.toml"), 0x1234ABCD};
  CHECK(rex::system::CopyFromDefault(*resolved, specification) ==
        rex::system::ProfileCopyResult::kSuccess);

  const auto target = resolved->active_root;
  CHECK(ReadFile(target / "rerevved.toml") == "fullscreen = false\n");
  CHECK(ReadFile(target / "B13EBABEBABEBABE" / title / "00000001" / "save-one" / "content.bin") ==
        "save bytes");
  CHECK(ReadFile(target / "B13EBABEBABEBABE" / title / "Headers" / "00000001" /
                 "save-one.header") == "header bytes");
  CHECK(ReadFile(target / title / "profile" / "User" / "settings.bin") == "profile bytes");
  CHECK(ReadFile(target / "achievements" / (title + ".toml")) == "achievement bytes");
  CHECK(ReadFile(target / "mod-loadout.toml") == "mod-loadout bytes");
  CHECK_FALSE(fs::exists(target / "B13EBABEBABEBABE" / title / "00000002"));
  CHECK_FALSE(fs::exists(target / "0000000000000000"));
  CHECK_FALSE(fs::exists(target / "unrelated.sentinel"));
  CHECK_FALSE(fs::exists(target / "logs"));
  CHECK_FALSE(fs::exists(target / "cache"));
  CHECK_FALSE(fs::exists(target / "mods"));
  CHECK_FALSE(fs::exists(target / "profiles"));
  CHECK_FALSE(fs::exists(target / "metadata"));

  // Repeating the request never overwrites an already published target.
  CHECK(rex::system::CopyFromDefault(*resolved, specification) ==
        rex::system::ProfileCopyResult::kTargetExists);
  CHECK(ReadFile(base / "rerevved.toml") == "fullscreen = false\n");
  CHECK(ReadFile(base / "unrelated.sentinel") == "do not copy");
}

TEST_CASE("profile copy failures leave source and target unchanged", "[profile]") {
  TempDirectory temp("rex_profile_copy_failure");
  const auto base = temp.path() / "user";
  WriteFile(base / "rerevved.toml", "source remains\n");

  const auto resolved = rex::system::ResolveProfile(base, "beta");
  REQUIRE(resolved);
  const rex::system::ProfileCopySpecification specification{fs::path("rerevved.toml"), 0x1234ABCD};

  fs::create_directories(resolved->active_root);
  WriteFile(resolved->active_root / "sentinel", "existing target");
  CHECK(rex::system::CopyFromDefault(*resolved, specification) ==
        rex::system::ProfileCopyResult::kTargetExists);
  CHECK(ReadFile(base / "rerevved.toml") == "source remains\n");
  CHECK(ReadFile(resolved->active_root / "sentinel") == "existing target");
}

TEST_CASE("profile copy rejects destination collisions without publication", "[profile]") {
  TempDirectory temp("rex_profile_copy_collision");
  const auto base = temp.path() / "user";
  const auto title = std::string("1234ABCD");
  const auto colliding_path = fs::path("achievements") / (title + ".toml");
  WriteFile(base / colliding_path, "single source\n");

  const auto resolved = rex::system::ResolveProfile(base, "alpha");
  REQUIRE(resolved);
  const rex::system::ProfileCopySpecification specification{colliding_path, 0x1234ABCD};
  CHECK(rex::system::CopyFromDefault(*resolved, specification) ==
        rex::system::ProfileCopyResult::kDestinationCollision);
  CHECK_FALSE(fs::exists(resolved->active_root));
  CHECK(ReadFile(base / colliding_path) == "single source\n");
}

TEST_CASE("profile resolution refuses a linked profiles root", "[profile]") {
  TempDirectory temp("rex_profile_target_link");
  const auto base = temp.path() / "user";
  const auto outside = temp.path() / "outside";
  fs::create_directories(base);
  fs::create_directories(outside);

  std::error_code ec;
  fs::create_directory_symlink(outside, base / "profiles", ec);
  if (ec) {
    INFO("profile-root link test skipped because this host cannot create symlinks: "
         << ec.message());
    SUCCEED("symlink creation unavailable");
    return;
  }

  CHECK_FALSE(rex::system::ResolveProfile(base, "alpha"));
  CHECK_FALSE(fs::exists(outside / "alpha"));
}

TEST_CASE("profile copy refuses an unsafe source link", "[profile]") {
  TempDirectory temp("rex_profile_source_link");
  const auto base = temp.path() / "user";
  WriteFile(base / "real.toml", "source remains\n");

  std::error_code ec;
  fs::create_symlink(base / "real.toml", base / "rerevved.toml", ec);
  if (ec) {
    INFO("source-link test skipped because this host cannot create symlinks: " << ec.message());
    SUCCEED("symlink creation unavailable");
    return;
  }

  const auto resolved = rex::system::ResolveProfile(base, "alpha");
  REQUIRE(resolved);
  const rex::system::ProfileCopySpecification specification{fs::path("rerevved.toml"), 0x1234ABCD};
  CHECK(rex::system::CopyFromDefault(*resolved, specification) ==
        rex::system::ProfileCopyResult::kUnsafeSource);
  CHECK_FALSE(fs::exists(resolved->active_root));
  CHECK(ReadFile(base / "real.toml") == "source remains\n");
}
