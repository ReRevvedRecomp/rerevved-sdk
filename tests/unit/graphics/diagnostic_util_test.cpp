/**
 ******************************************************************************
 * ReXGlue - High-level Xbox 360 game recompilation framework                 *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include <rex/graphics/diagnostic_util.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::diagnostic {

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("rex-diagnostic-util-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << contents;
  REQUIRE(file.good());
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("Diagnostic fence readiness rejects removal and incomplete waits",
          "[graphics][diagnostic]") {
  CHECK_FALSE(IsFenceCompletionValueValid(std::numeric_limits<uint64_t>::max()));
  CHECK_FALSE(IsFenceWaitReady(std::numeric_limits<uint64_t>::max(), 7, true, true, true));
  CHECK_FALSE(IsFenceWaitReady(7, 7, false, true, true));
  CHECK_FALSE(IsFenceWaitReady(7, 7, true, false, true));
  CHECK_FALSE(IsFenceWaitReady(7, 7, true, true, false));
  CHECK(IsFenceWaitReady(7, 7, true, true, true));

  CHECK_FALSE(IsFenceFailureTerminal(false, false));
  CHECK(IsFenceFailureTerminal(true, false));
  CHECK(IsFenceFailureTerminal(false, true));
  CHECK(IsFenceFailureTerminal(true, true));

  CHECK_FALSE(CanReleaseSubmittedGpuResources(false, false));
  CHECK(CanReleaseSubmittedGpuResources(true, false));
  CHECK(CanReleaseSubmittedGpuResources(false, true));
  CHECK(CanReleaseSubmittedGpuResources(true, true));
}

TEST_CASE("Diagnostic texture range validates fetch field bounds", "[graphics][diagnostic]") {
  constexpr uint32_t kPitchFieldMax = (1 << 9) - 1;
  constexpr uint32_t kDimensionFieldMax = 1 << 13;
  const uint32_t format = uint32_t(xenos::TextureFormat::k_8);
  Texture2DSourceRange range;

  REQUIRE(GetTexture2DSourceRange(0, 1, 1, 1, false, format, range));
  CHECK(range.base == 0);
  CHECK(range.size != 0);
  CHECK(GetTexture2DSourceRange(0, kPitchFieldMax, 1, 1, false, format, range));
  CHECK(GetTexture2DSourceRange(0, 256, kDimensionFieldMax, 1, false, format, range));
  CHECK(GetTexture2DSourceRange(0, 1, 1, kDimensionFieldMax, false, format, range));

  CHECK_FALSE(GetTexture2DSourceRange(0, 0, 1, 1, false, format, range));
  CHECK_FALSE(GetTexture2DSourceRange(0, kPitchFieldMax + 1, 1, 1, false, format, range));
  CHECK_FALSE(GetTexture2DSourceRange(0, 1, 0, 1, false, format, range));
  CHECK_FALSE(GetTexture2DSourceRange(0, 256, kDimensionFieldMax + 1, 1, false, format, range));
  CHECK_FALSE(GetTexture2DSourceRange(0, 1, 1, 0, false, format, range));
  CHECK_FALSE(GetTexture2DSourceRange(0, 1, 1, kDimensionFieldMax + 1, false, format, range));
}

TEST_CASE("Diagnostic texture range uses exact canonical extents", "[graphics][diagnostic]") {
  constexpr uint32_t kLastAperturePage = 0x1FFFF;
  constexpr uint32_t kPitchTexelsDiv32 = 4;
  const uint32_t format = uint32_t(xenos::TextureFormat::k_8);
  Texture2DSourceRange range;

  REQUIRE(GetTexture2DSourceRange(1, 1, 32, 32, true, format, range));
  CHECK(range.base == 4096);
  CHECK(range.size != 0);
  REQUIRE(
      GetTexture2DSourceRange(kLastAperturePage, kPitchTexelsDiv32, 128, 32, false, format, range));
  CHECK(uint64_t(range.base) + range.size == kGuestPhysicalApertureSize);
  REQUIRE(GetTexture2DSourceRange(kLastAperturePage, 1, 32, 128, true, format, range));
  CHECK(uint64_t(range.base) + range.size == kGuestPhysicalApertureSize);
  const uint32_t compressed_format = uint32_t(xenos::TextureFormat::k_DXT1);
  REQUIRE(GetTexture2DSourceRange(kLastAperturePage, 4, 128, 64, false, compressed_format, range));
  CHECK(uint64_t(range.base) + range.size == kGuestPhysicalApertureSize);
  CHECK_FALSE(
      GetTexture2DSourceRange(kLastAperturePage, kPitchTexelsDiv32, 1, 33, false, format, range));
  CHECK_FALSE(
      GetTexture2DSourceRange(kLastAperturePage, kPitchTexelsDiv32, 32, 33, true, format, range));
  CHECK_FALSE(GetTexture2DSourceRange(1, 1, 33, 1, false, format, range));
}

TEST_CASE("Diagnostic publication rejects every exact path alias without mutation",
          "[graphics][diagnostic]") {
  constexpr std::array<std::array<size_t, 2>, 6> kAliasedPairs = {
      {{{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}}}};

  for (const auto& aliased_pair : kAliasedPairs) {
    TemporaryDirectory temporary_directory;
    std::array<std::filesystem::path, 4> paths = {
        temporary_directory.path() / "data.tmp",
        temporary_directory.path() / "metadata.tmp",
        temporary_directory.path() / "data.dds",
        temporary_directory.path() / "metadata.txt",
    };
    paths[aliased_pair[1]] = paths[aliased_pair[0]];
    for (size_t index = 0; index < paths.size(); ++index) {
      if (!std::filesystem::exists(paths[index])) {
        WriteFile(paths[index], "artifact " + std::to_string(index));
      }
    }
    std::array<std::string, 4> contents;
    for (size_t index = 0; index < paths.size(); ++index) {
      contents[index] = ReadFile(paths[index]);
    }

    const ArtifactPairPublicationResult result =
        PublishArtifactPair(paths[0], paths[1], paths[2], paths[3]);

    CHECK_FALSE(result.succeeded());
    CHECK(result.publication_error == std::errc::invalid_argument);
    for (size_t index = 0; index < paths.size(); ++index) {
      CHECK(std::filesystem::is_regular_file(paths[index]));
      CHECK(ReadFile(paths[index]) == contents[index]);
    }
  }
}

TEST_CASE("Diagnostic publication rejects an existing filesystem alias without mutation",
          "[graphics][diagnostic]") {
  TemporaryDirectory temporary_directory;
  const auto data_temporary = temporary_directory.path() / "data.tmp";
  const auto metadata_temporary = temporary_directory.path() / "metadata.tmp";
  const auto data_final = temporary_directory.path() / "data.dds";
  const auto metadata_final = temporary_directory.path() / "metadata.txt";
  WriteFile(data_temporary, "temporary pair");
  std::error_code link_error;
  std::filesystem::create_hard_link(data_temporary, metadata_temporary, link_error);
  if (link_error) {
    SKIP("Hard links are not supported in this test environment");
  }
  WriteFile(data_final, "old data");
  WriteFile(metadata_final, "old metadata");

  const ArtifactPairPublicationResult result =
      PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final);

  CHECK_FALSE(result.succeeded());
  CHECK(result.publication_error == std::errc::invalid_argument);
  CHECK(ReadFile(data_temporary) == "temporary pair");
  CHECK(ReadFile(metadata_temporary) == "temporary pair");
  CHECK(ReadFile(data_final) == "old data");
  CHECK(ReadFile(metadata_final) == "old metadata");
}

TEST_CASE("Diagnostic publication retries data-only and metadata-only partial pairs",
          "[graphics][diagnostic]") {
  TemporaryDirectory temporary_directory;

  SECTION("data-only") {
    const auto data_temporary = temporary_directory.path() / "data.tmp";
    const auto metadata_temporary = temporary_directory.path() / "metadata.tmp";
    const auto data_final = temporary_directory.path() / "data.dds";
    const auto metadata_final = temporary_directory.path() / "metadata.txt";
    WriteFile(data_final, "partial data");
    WriteFile(data_temporary, "new data");

    CHECK_FALSE(PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final)
                    .succeeded());
    CHECK(ReadFile(data_final) == "partial data");
    WriteFile(metadata_temporary, "new metadata");
    CHECK(PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final)
              .succeeded());
    CHECK(ReadFile(data_final) == "new data");
    CHECK(ReadFile(metadata_final) == "new metadata");
  }

  SECTION("metadata-only") {
    const auto data_temporary = temporary_directory.path() / "other-data.tmp";
    const auto metadata_temporary = temporary_directory.path() / "other-metadata.tmp";
    const auto data_final = temporary_directory.path() / "other-data.dds";
    const auto metadata_final = temporary_directory.path() / "other-metadata.txt";
    WriteFile(metadata_final, "partial metadata");
    WriteFile(metadata_temporary, "new metadata");

    CHECK_FALSE(PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final)
                    .succeeded());
    CHECK(ReadFile(metadata_final) == "partial metadata");
    WriteFile(data_temporary, "new data");
    CHECK(PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final)
              .succeeded());
    CHECK(ReadFile(data_final) == "new data");
    CHECK(ReadFile(metadata_final) == "new metadata");
  }
}

TEST_CASE("Diagnostic publication preserves a complete pair when replacement cleanup fails",
          "[graphics][diagnostic]") {
  TemporaryDirectory temporary_directory;
  const auto data_final = temporary_directory.path() / "capture.dds";
  const auto metadata_final = temporary_directory.path() / "capture.txt";
  const auto data_temporary = temporary_directory.path() / "capture.dds.tmp";
  const auto metadata_temporary = temporary_directory.path() / "capture.txt.tmp";
  WriteFile(data_final, "old data");
  WriteFile(metadata_final, "old metadata");
  std::filesystem::create_directory(data_temporary);
  WriteFile(data_temporary / "blocking-file", "new data");
  WriteFile(metadata_temporary, "new metadata");

  const ArtifactPairPublicationResult result =
      PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final);

  CHECK(result.status == ArtifactPairPublicationStatus::kAlreadyComplete);
  CHECK(result.cleanup_error);
  CHECK(ReadFile(data_final) == "old data");
  CHECK(ReadFile(metadata_final) == "old metadata");
}

TEST_CASE("Diagnostic publication does not publish data before metadata",
          "[graphics][diagnostic]") {
  TemporaryDirectory temporary_directory;
  const auto data_final = temporary_directory.path() / "capture.dds";
  const auto metadata_final = temporary_directory.path() / "missing" / "capture.txt";
  const auto data_temporary = temporary_directory.path() / "capture.dds.tmp";
  const auto metadata_temporary = temporary_directory.path() / "capture.txt.tmp";
  WriteFile(data_temporary, "new data");
  WriteFile(metadata_temporary, "new metadata");

  const ArtifactPairPublicationResult result =
      PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final);

  CHECK_FALSE(result.succeeded());
  CHECK_FALSE(std::filesystem::exists(data_final));
  CHECK_FALSE(std::filesystem::exists(metadata_final));
  CHECK_FALSE(std::filesystem::exists(data_temporary));
  CHECK_FALSE(std::filesystem::exists(metadata_temporary));
}

TEST_CASE("Diagnostic publication retries after data publication fails", "[graphics][diagnostic]") {
  TemporaryDirectory temporary_directory;
  const auto data_final = temporary_directory.path() / "missing" / "capture.dds";
  const auto metadata_final = temporary_directory.path() / "capture.txt";
  const auto data_temporary = temporary_directory.path() / "capture.dds.tmp";
  const auto metadata_temporary = temporary_directory.path() / "capture.txt.tmp";
  WriteFile(data_temporary, "new data");
  WriteFile(metadata_temporary, "new metadata");

  const ArtifactPairPublicationResult first_result =
      PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final);

  CHECK_FALSE(first_result.succeeded());
  CHECK(ReadFile(metadata_final) == "new metadata");
  CHECK_FALSE(std::filesystem::exists(data_final));
  CHECK_FALSE(std::filesystem::exists(data_temporary));

  std::filesystem::create_directory(data_final.parent_path());
  WriteFile(data_temporary, "retry data");
  WriteFile(metadata_temporary, "retry metadata");
  const ArtifactPairPublicationResult retry_result =
      PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final);
  CHECK(retry_result.status == ArtifactPairPublicationStatus::kPublished);
  CHECK(ReadFile(data_final) == "retry data");
  CHECK(ReadFile(metadata_final) == "retry metadata");
}

TEST_CASE("Diagnostic publication preserves an existing complete pair", "[graphics][diagnostic]") {
  TemporaryDirectory temporary_directory;
  const auto data_final = temporary_directory.path() / "capture.dds";
  const auto metadata_final = temporary_directory.path() / "capture.txt";
  const auto data_temporary = temporary_directory.path() / "capture.dds.tmp";
  const auto metadata_temporary = temporary_directory.path() / "capture.txt.tmp";
  WriteFile(data_final, "old data");
  WriteFile(metadata_final, "old metadata");
  WriteFile(data_temporary, "new data");
  WriteFile(metadata_temporary, "new metadata");

  const ArtifactPairPublicationResult result =
      PublishArtifactPair(data_temporary, metadata_temporary, data_final, metadata_final);

  CHECK(result.status == ArtifactPairPublicationStatus::kAlreadyComplete);
  CHECK_FALSE(result.cleanup_error);
  CHECK(ReadFile(data_final) == "old data");
  CHECK(ReadFile(metadata_final) == "old metadata");
  CHECK_FALSE(std::filesystem::exists(data_temporary));
  CHECK_FALSE(std::filesystem::exists(metadata_temporary));
}

}  // namespace rex::graphics::diagnostic
