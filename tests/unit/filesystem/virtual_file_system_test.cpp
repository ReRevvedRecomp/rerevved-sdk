#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>

using rex::X_STATUS;

namespace {

constexpr std::string_view kGameMount = "\\Device\\Harddisk0\\Partition1";

class ScopedVfsTree {
 public:
  ScopedVfsTree() {
    auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() / ("rexglue_vfs_test_" + std::to_string(suffix));
    REQUIRE(std::filesystem::create_directories(root_ / "game" / "folder"));
    REQUIRE(std::filesystem::create_directories(root_ / "decoy"));
    Write(root_ / "game" / "DLCScenarioData0.xml");
    Write(root_ / "game" / "precedence.txt");
    Write(root_ / "outside.txt");
  }

  ~ScopedVfsTree() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  std::filesystem::path game_root() const { return root_ / "game"; }
  std::filesystem::path decoy_root() const { return root_ / "decoy"; }

 private:
  static void Write(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    stream << "test";
    REQUIRE(stream.good());
  }

  std::filesystem::path root_;
};

void RegisterHostDevice(rex::filesystem::VirtualFileSystem& vfs, std::string_view mount_path,
                        const std::filesystem::path& host_path) {
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(mount_path, host_path, true);
  REQUIRE(device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(device)));
}

void RegisterGameMount(rex::filesystem::VirtualFileSystem& vfs,
                       const std::filesystem::path& host_path) {
  RegisterHostDevice(vfs, kGameMount, host_path);
  REQUIRE(vfs.RegisterSymbolicLink("game:", kGameMount));
  REQUIRE(vfs.RegisterSymbolicLink("d:", kGameMount));
}

}  // namespace

TEST_CASE("VFS rejects rootless DOS-device paths", "[filesystem][vfs]") {
  ScopedVfsTree tree;
  rex::filesystem::VirtualFileSystem vfs;
  RegisterGameMount(vfs, tree.game_root());

  CHECK(vfs.ResolvePath("DLCScenarioData0.xml") == nullptr);
  CHECK(vfs.ResolvePath(".\\DLCScenarioData0.xml") == nullptr);
  CHECK(vfs.ResolvePath("folder\\..\\DLCScenarioData0.xml") == nullptr);
  CHECK(vfs.ResolvePath("folder/../DLCScenarioData0.xml") == nullptr);
  CHECK(vfs.ResolvePath("..\\DLCScenarioData0.xml") == nullptr);
}

TEST_CASE("VFS open requires a device for a rootless path", "[filesystem][vfs]") {
  ScopedVfsTree tree;
  rex::filesystem::VirtualFileSystem vfs;
  RegisterGameMount(vfs, tree.game_root());

  rex::filesystem::File* file = nullptr;
  rex::filesystem::FileAction action{};
  CHECK(vfs.OpenFile(nullptr, "DLCScenarioData0.xml", rex::filesystem::FileDisposition::kOpen,
                     rex::filesystem::FileAccess::kGenericRead, false, true, &file,
                     &action) == X_STATUS_NO_SUCH_FILE);
  CHECK(file == nullptr);
  CHECK(action == rex::filesystem::FileAction::kDoesNotExist);

  REQUIRE(vfs.OpenFile(nullptr, "game:\\DLCScenarioData0.xml",
                       rex::filesystem::FileDisposition::kOpen,
                       rex::filesystem::FileAccess::kGenericRead, false, true, &file,
                       &action) == X_STATUS_SUCCESS);
  REQUIRE(file != nullptr);
  file->Destroy();
}

TEST_CASE("VFS resolves only explicit game device paths", "[filesystem][vfs]") {
  ScopedVfsTree tree;
  rex::filesystem::VirtualFileSystem vfs;
  RegisterGameMount(vfs, tree.game_root());

  auto* game_entry = vfs.ResolvePath("game:\\DLCScenarioData0.xml");
  REQUIRE(game_entry != nullptr);
  CHECK(game_entry->device()->mount_path() == kGameMount);
  CHECK(vfs.ResolvePath("d:/DLCScenarioData0.xml") != nullptr);
  CHECK(vfs.ResolvePath("GAME:\\dlcscenariodata0.XML") != nullptr);
  CHECK(vfs.ResolvePath("game:\\folder\\..\\DLCScenarioData0.xml") != nullptr);
  CHECK(vfs.ResolvePath("\\Device\\Harddisk0\\Partition1\\DLCScenarioData0.xml") != nullptr);
}

TEST_CASE("VFS expands links before device selection and contains traversal", "[filesystem][vfs]") {
  ScopedVfsTree tree;
  rex::filesystem::VirtualFileSystem vfs;
  RegisterHostDevice(vfs, "game:", tree.decoy_root());
  RegisterGameMount(vfs, tree.game_root());

  CHECK(vfs.ResolvePath("game:\\precedence.txt") != nullptr);
  CHECK(vfs.ResolvePath("game:\\..\\outside.txt") == nullptr);
  CHECK(vfs.ResolvePath("game:/folder/../../outside.txt") == nullptr);
  CHECK(vfs.ResolvePath("\\Device\\Harddisk0\\Partition1\\..\\outside.txt") == nullptr);
}
