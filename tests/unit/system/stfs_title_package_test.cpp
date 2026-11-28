#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/stfs_xbox.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/system/xam/content_manager.h>

namespace {

namespace fs = std::filesystem;
namespace rfs = rex::filesystem;
using rex::X_RESULT;
using rex::X_STATUS;

constexpr uint32_t kTitleId = 0x1234ABCD;
constexpr size_t kBlock = 0x1000;
constexpr size_t kHeaderSize = sizeof(rfs::StfsHeader);
constexpr size_t kDataBase = (kHeaderSize + kBlock - 1) & ~(kBlock - 1);

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

struct PackageOptions {
  rfs::XContentPackageType magic = rfs::XContentPackageType::kLive;
  rex::system::XContentType content_type = rex::system::XContentType::kMarketplaceContent;
  rfs::XContentVolumeType volume_type = rfs::XContentVolumeType::kStfs;
  uint32_t title_id = kTitleId;
  uint16_t second_parent = 0xFFFF;
};

struct PackageFile {
  std::string name;
  std::string bytes;
};

// Read-only STFS layout with one level-0 hash table: physical block 0 holds the
// hash table and logical block n is physical block n + 1.
std::vector<uint8_t> BuildPackage(std::span<const PackageFile> files,
                                  const PackageOptions& options = {}) {
  uint32_t next_block = 1;  // block 0 is the file table
  std::vector<uint32_t> starts;
  std::vector<uint32_t> counts;
  for (const auto& file : files) {
    const auto count = static_cast<uint32_t>((file.bytes.size() + kBlock - 1) / kBlock);
    starts.push_back(next_block);
    counts.push_back(count);
    next_block += count;
  }
  const uint32_t total_blocks = next_block;
  std::vector<uint8_t> bytes(kDataBase + (size_t(total_blocks) + 1) * kBlock, 0);

  auto* header = reinterpret_cast<rfs::StfsHeader*>(bytes.data());
  header->header.magic = options.magic;
  header->header.header_size = static_cast<uint32_t>(kHeaderSize);
  auto& metadata = header->metadata;
  metadata.content_type = options.content_type;
  metadata.metadata_version = 2;
  metadata.volume_type = options.volume_type;
  metadata.execution_info.title_id = options.title_id;
  auto& stfs = metadata.volume_descriptor.stfs;
  stfs.descriptor_length = sizeof(rfs::StfsVolumeDescriptor);
  stfs.flags.bits.read_only_format = 1;
  stfs.file_table_block_count = 1;
  stfs.set_file_table_block_number(0);
  stfs.total_block_count = total_blocks;

  auto block_offset = [](uint32_t logical) { return kDataBase + (size_t(logical) + 1) * kBlock; };
  auto* table = reinterpret_cast<rfs::StfsHashTable*>(bytes.data() + kDataBase);
  table->entries[0].set_level0_next_block(0xFFFFFF);

  auto* directory = reinterpret_cast<rfs::StfsDirectoryBlock*>(bytes.data() + block_offset(0));
  for (size_t i = 0; i < files.size(); ++i) {
    auto& entry = directory->entries[i];
    std::memcpy(entry.name, files[i].name.data(), files[i].name.size());
    entry.flags.name_length = static_cast<uint8_t>(files[i].name.size());
    entry.flags.contiguous = 1;
    entry.set_valid_data_blocks(counts[i]);
    entry.set_allocated_data_blocks(counts[i]);
    entry.set_start_block_number(starts[i]);
    entry.directory_index = i == 1 ? options.second_parent : uint16_t(0xFFFF);
    entry.length = static_cast<uint32_t>(files[i].bytes.size());
    std::memcpy(bytes.data() + block_offset(starts[i]), files[i].bytes.data(),
                files[i].bytes.size());
    for (uint32_t b = 0; b < counts[i]; ++b) {
      const uint32_t block = starts[i] + b;
      table->entries[block].set_level0_next_block(b + 1 < counts[i] ? block + 1 : 0xFFFFFF);
    }
  }
  return bytes;
}

std::vector<PackageFile> DlcFiles() {
  return {{"name.txt", "victory"}, {"dlc.fpk", std::string(5000, 'F') + "end"}};
}

fs::path WritePackage(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  REQUIRE(output);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  REQUIRE(output);
  return path;
}

std::string ReadEntry(rfs::StfsContainerDevice& device, std::string_view name) {
  auto* entry = device.ResolvePath(name);
  REQUIRE(entry);
  rfs::File* file = nullptr;
  REQUIRE(entry->Open(rfs::FileAccess::kFileReadData, &file) == X_STATUS_SUCCESS);
  std::string value(entry->size(), '\0');
  size_t read = 0;
  CHECK(file->ReadSync(std::span(reinterpret_cast<uint8_t*>(value.data()), value.size()), 0,
                       &read) == X_STATUS_SUCCESS);
  file->Destroy();
  CHECK(read == value.size());
  return value;
}

}  // namespace

TEST_CASE("STFS device reads a single-file package across blocks", "[stfs]") {
  TempDirectory temp("rex_stfs_read");
  const auto files = DlcFiles();
  const auto path = WritePackage(temp.path() / "package", BuildPackage(files));

  rfs::StfsContainerDevice device("", path);
  REQUIRE(device.Initialize());
  CHECK(ReadEntry(device, "name.txt") == files[0].bytes);
  CHECK(ReadEntry(device, "dlc.fpk") == files[1].bytes);
}

TEST_CASE("STFS package mounts as a content root without writes", "[stfs]") {
  TempDirectory temp("rex_stfs_vfs");
  const auto files = DlcFiles();
  const auto path = WritePackage(temp.path() / "package", BuildPackage(files));
  constexpr std::string_view kDevice = "\\Device\\Content\\1\\";

  rfs::VirtualFileSystem vfs;
  auto device = std::make_unique<rfs::StfsContainerDevice>(kDevice, path);
  REQUIRE(device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(device)));
  REQUIRE(vfs.RegisterSymbolicLink("dlc:", kDevice));

  auto* name = vfs.ResolvePath("dlc:\\name.txt");
  REQUIRE(name);
  CHECK(name->size() == files[0].bytes.size());
  auto* archive = vfs.ResolvePath("dlc:\\DLC.FPK");
  REQUIRE(archive);
  CHECK(archive->size() == files[1].bytes.size());
  CHECK(vfs.CreatePath("dlc:\\new.txt", rfs::kFileAttributeNormal) == nullptr);
}

TEST_CASE("STFS device refuses an out-of-range parent index", "[stfs]") {
  TempDirectory temp("rex_stfs_parent");
  PackageOptions options;
  options.second_parent = 7;
  const auto path = WritePackage(temp.path() / "package", BuildPackage(DlcFiles(), options));

  rfs::StfsContainerDevice device("", path);
  CHECK_FALSE(device.Initialize());
}

TEST_CASE("STFS device can refuse SVOD before parsing it", "[stfs]") {
  TempDirectory temp("rex_stfs_svod");
  PackageOptions options;
  options.volume_type = rfs::XContentVolumeType::kSvod;
  const auto path = WritePackage(temp.path() / "package", BuildPackage(DlcFiles(), options));

  rfs::StfsContainerDevice device("", path);
  device.RequireSingleFileStfs();
  CHECK_FALSE(device.Initialize());
}

TEST_CASE("STFS single-file mode never opens data fragments", "[stfs]") {
  TempDirectory temp("rex_stfs_fragments");
  auto bytes = BuildPackage(DlcFiles());
  reinterpret_cast<rfs::StfsHeader*>(bytes.data())->metadata.data_file_count = 2;
  const auto path = WritePackage(temp.path() / "package", bytes);
  // Two fragments match the declared count, so only single-file mode stops the open.
  fs::create_directories(temp.path() / "package.data");
  WritePackage(temp.path() / "package.data" / "Data0000", {1});
  WritePackage(temp.path() / "package.data" / "Data0001", {1});

  rfs::StfsContainerDevice device("", path);
  device.RequireSingleFileStfs();
  CHECK_FALSE(device.Initialize());
  // Windows refuses to delete an open file, so a fragment left open would fail here.
  std::error_code ec;
  CHECK(fs::remove(temp.path() / "package.data" / "Data0000", ec));
  CHECK_FALSE(ec);
}

TEST_CASE("STFS device closes a multi-file header it cannot open", "[stfs]") {
  TempDirectory temp("rex_stfs_multi");
  auto bytes = BuildPackage(DlcFiles());
  reinterpret_cast<rfs::StfsHeader*>(bytes.data())->metadata.data_file_count = 2;
  const auto path = WritePackage(temp.path() / "package", bytes);
  {
    rfs::StfsContainerDevice device("", path);
    CHECK_FALSE(device.Initialize());
    std::error_code ec;
    CHECK(fs::remove(path, ec));
    CHECK_FALSE(ec);
  }
}

TEST_CASE("STFS device refuses malformed file entries", "[stfs]") {
  // DlcFiles() layout: name.txt is logical block 1; dlc.fpk is logical blocks 2 and 3. The cycle
  // case declares the whole file so only the reused-block check stops the walk.
  auto directory = [](std::vector<uint8_t>& bytes) {
    return reinterpret_cast<rfs::StfsDirectoryBlock*>(bytes.data() + kDataBase + kBlock);
  };
  auto table = [](std::vector<uint8_t>& bytes) {
    return reinterpret_cast<rfs::StfsHashTable*>(bytes.data() + kDataBase);
  };
  struct Case {
    const char* name;
    std::function<void(std::vector<uint8_t>&)> corrupt;
  };
  const Case cases[] = {
      {"name byte",
       [&](auto& bytes) { std::memset(&directory(bytes)->entries[0].name[1], 0xFF, 1); }},
      {"length", [&](auto& bytes) { directory(bytes)->entries[1].length = 0x10000000; }},
      {"start block",
       [&](auto& bytes) { directory(bytes)->entries[1].set_start_block_number(0x100); }},
      {"chain", [&](auto& bytes) { table(bytes)->entries[2].set_level0_next_block(0xFFFFFF); }},
      {"cycle",
       [&](auto& bytes) {
         directory(bytes)->entries[1].length = static_cast<uint32_t>(bytes.size());
         table(bytes)->entries[3].set_level0_next_block(2);
       }},
      {"separator", [&](auto& bytes) { directory(bytes)->entries[0].name[1] = '\\'; }},
      {"table cycle",
       [&](auto& bytes) {
         reinterpret_cast<rfs::StfsHeader*>(bytes.data())
             ->metadata.volume_descriptor.stfs.file_table_block_count = 2;
         table(bytes)->entries[0].set_level0_next_block(0);
         // Directories own no data blocks, so only the table-block check stops the repeat.
         directory(bytes)->entries[0].flags.directory = 1;
         directory(bytes)->entries[1].flags.directory = 1;
       }},
      {"header size",
       [](auto& bytes) {
         reinterpret_cast<rfs::StfsHeader*>(bytes.data())->header.header_size = 0xFFFFFFFF;
       }},
  };

  TempDirectory temp("rex_stfs_malformed");
  rex::system::xam::ContentManager manager(nullptr, temp.path() / "base");
  for (const auto& test : cases) {
    INFO(test.name);
    auto bytes = BuildPackage(DlcFiles());
    test.corrupt(bytes);
    const auto path = WritePackage(temp.path() / "dlc" / test.name, bytes);
    rfs::StfsContainerDevice device("", path);
    CHECK_FALSE(device.Initialize());
    CHECK(manager.AddTitlePackage(kTitleId, test.name, path) == X_ERROR_FUNCTION_FAILED);
  }
}

TEST_CASE("title package files list as marketplace content after header checks", "[profile]") {
  TempDirectory temp("rex_title_package_file");
  const auto base = temp.path() / "base";
  const auto dlc = temp.path() / "dlc";
  const auto files = DlcFiles();
  const auto good = WritePackage(dlc / "GOODPACKAGE", BuildPackage(files));
  const auto good_bytes = BuildPackage(files);

  PackageOptions wrong_title;
  wrong_title.title_id = 0x0BADF00D;
  PackageOptions saved_game;
  saved_game.content_type = rex::system::XContentType::kSavedGame;
  PackageOptions svod;
  svod.volume_type = rfs::XContentVolumeType::kSvod;
  PackageOptions bad_parent;
  bad_parent.second_parent = 7;
  auto not_package = BuildPackage(files);
  std::memcpy(not_package.data(), "NOPE", 4);

  rex::system::xam::ContentManager manager(nullptr, base);
  CHECK(manager.AddTitlePackage(kTitleId, "GOODPACKAGE", good) == X_ERROR_SUCCESS);
  CHECK(manager.AddTitlePackage(kTitleId, "goodpackage", good) == X_ERROR_ALREADY_EXISTS);
  CHECK(manager.AddTitlePackage(kTitleId, "title",
                                WritePackage(dlc / "title", BuildPackage(files, wrong_title))) ==
        X_ERROR_INVALID_PARAMETER);
  CHECK(manager.AddTitlePackage(kTitleId, "type",
                                WritePackage(dlc / "type", BuildPackage(files, saved_game))) ==
        X_ERROR_INVALID_PARAMETER);
  CHECK(manager.AddTitlePackage(kTitleId, "svod",
                                WritePackage(dlc / "svod", BuildPackage(files, svod))) ==
        X_ERROR_INVALID_PARAMETER);
  CHECK(manager.AddTitlePackage(kTitleId, "parent",
                                WritePackage(dlc / "parent", BuildPackage(files, bad_parent))) ==
        X_ERROR_FUNCTION_FAILED);
  CHECK(manager.AddTitlePackage(kTitleId, "magic", WritePackage(dlc / "magic", not_package)) ==
        X_ERROR_FUNCTION_FAILED);

  const auto listed =
      manager.ListContent(1, 0, rex::system::XContentType::kMarketplaceContent, kTitleId);
  REQUIRE(listed.size() == 1);
  CHECK(listed[0].file_name() == "GOODPACKAGE");
  CHECK(listed[0].title_id == kTitleId);
  CHECK(manager.ContentExists(0, listed[0]));

  CHECK(manager.CreateContent("dlc", 0, listed[0]) == X_ERROR_ACCESS_DENIED);
  CHECK(manager.SetContentThumbnail(0, listed[0], {1, 2, 3}) == X_ERROR_ACCESS_DENIED);
  CHECK(manager.DeleteContent(0, listed[0]) == X_ERROR_ACCESS_DENIED);
  CHECK(manager.UnmountAndDeleteContent(0, listed[0]) == X_ERROR_ACCESS_DENIED);

  std::ifstream input(good, std::ios::binary);
  const std::vector<uint8_t> after{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
  CHECK(after == good_bytes);
  CHECK_FALSE(fs::exists(base / "0000000000000000"));
}
