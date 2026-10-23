/**
 ******************************************************************************
 * ReXGlue - High-level Xbox 360 game recompilation framework                 *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <rex/graphics/diagnostic_util.h>

#include <limits>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>

namespace rex::graphics::diagnostic {

namespace {

bool CheckedAdd(uint64_t a, uint64_t b, uint64_t& result_out) {
  if (a > std::numeric_limits<uint64_t>::max() - b) {
    return false;
  }
  result_out = a + b;
  return true;
}

bool CheckedMultiply(uint64_t a, uint64_t b, uint64_t& result_out) {
  if (a && b > std::numeric_limits<uint64_t>::max() / a) {
    return false;
  }
  result_out = a * b;
  return true;
}

bool GetRegularFileState(const std::filesystem::path& path, bool& exists_out, bool& is_regular_out,
                         std::error_code& error_out) {
  exists_out = std::filesystem::exists(path, error_out);
  if (error_out || !exists_out) {
    is_regular_out = false;
    return !error_out;
  }
  is_regular_out = std::filesystem::is_regular_file(path, error_out);
  return !error_out;
}

bool PathsAlias(const std::filesystem::path& first, const std::filesystem::path& second,
                bool& alias_out, std::error_code& error_out) {
  alias_out = first == second;
  if (alias_out) {
    return true;
  }

  bool first_exists = std::filesystem::exists(first, error_out);
  if (error_out) {
    return false;
  }
  bool second_exists = std::filesystem::exists(second, error_out);
  if (error_out || !first_exists || !second_exists) {
    return !error_out;
  }
  alias_out = std::filesystem::equivalent(first, second, error_out);
  return !error_out;
}

bool ArtifactPathsAreDistinct(const std::filesystem::path* paths, size_t path_count,
                              std::error_code& error_out) {
  for (size_t first = 0; first < path_count; ++first) {
    for (size_t second = first + 1; second < path_count; ++second) {
      bool alias;
      if (!PathsAlias(paths[first], paths[second], alias, error_out)) {
        return false;
      }
      if (alias) {
        error_out = std::make_error_code(std::errc::invalid_argument);
        return false;
      }
    }
  }
  return true;
}

void RemoveForCleanup(const std::filesystem::path& path, std::error_code& cleanup_error_out) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error && !cleanup_error_out) {
    cleanup_error_out = error;
  }
}

}  // namespace

bool IsFenceCompletionValueValid(uint64_t completed_value) {
  return completed_value != std::numeric_limits<uint64_t>::max();
}

bool IsFenceWaitReady(uint64_t completed_value, uint64_t awaited_value, bool value_was_signaled,
                      bool event_was_armed, bool wait_succeeded) {
  return value_was_signaled && event_was_armed && wait_succeeded &&
         IsFenceCompletionValueValid(completed_value) && completed_value >= awaited_value;
}

bool IsFenceFailureTerminal(bool tracking_is_unrecoverable, bool device_is_removed) {
  return tracking_is_unrecoverable || device_is_removed;
}

bool CanReleaseSubmittedGpuResources(bool queue_is_idle, bool device_is_removed) {
  return queue_is_idle || device_is_removed;
}

bool GetTexture2DSourceRange(uint32_t base_page, uint32_t pitch_texels_div_32,
                             uint32_t width_texels, uint32_t height_texels, bool is_tiled,
                             uint32_t format, Texture2DSourceRange& range_out) {
  range_out = {};
  constexpr uint32_t kFetchPitchMax = (UINT32_C(1) << 9) - 1;
  constexpr uint32_t kFetch2DDimensionMax = UINT32_C(1) << 13;
  if (!pitch_texels_div_32 || pitch_texels_div_32 > kFetchPitchMax || !width_texels ||
      width_texels > kFetch2DDimensionMax || !height_texels ||
      height_texels > kFetch2DDimensionMax || format >= 64) {
    return false;
  }

  const FormatInfo* format_info = FormatInfo::Get(format);
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (!format_info->block_width || !format_info->block_height || !bytes_per_block) {
    return false;
  }

  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, pitch_texels_div_32, width_texels, height_texels, 1,
      is_tiled, xenos::TextureFormat(format), false, true, 0);
  uint64_t source_size = layout.base.level_data_extent_bytes;
  if (!source_size) {
    return false;
  }

  if (!is_tiled) {
    uint64_t width_block_numerator;
    uint64_t height_block_numerator;
    if (!CheckedAdd(width_texels, format_info->block_width - 1, width_block_numerator) ||
        !CheckedAdd(height_texels, format_info->block_height - 1, height_block_numerator)) {
      return false;
    }
    const uint64_t width_blocks = width_block_numerator / format_info->block_width;
    const uint64_t height_blocks = height_block_numerator / format_info->block_height;
    uint64_t visible_row_bytes;
    if (!CheckedMultiply(width_blocks, bytes_per_block, visible_row_bytes) ||
        visible_row_bytes > layout.base.row_pitch_bytes) {
      return false;
    }
    uint64_t preceding_rows_bytes;
    uint64_t exact_source_size;
    if (!CheckedMultiply(height_blocks - 1, layout.base.row_pitch_bytes, preceding_rows_bytes) ||
        !CheckedAdd(preceding_rows_bytes, visible_row_bytes, exact_source_size) ||
        exact_source_size != source_size) {
      return false;
    }
  }

  uint64_t base;
  uint64_t end;
  if (!CheckedMultiply(base_page, UINT64_C(4096), base) || !CheckedAdd(base, source_size, end) ||
      end > kGuestPhysicalApertureSize || base > std::numeric_limits<uint32_t>::max() ||
      source_size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }

  range_out.base = uint32_t(base);
  range_out.size = uint32_t(source_size);
  return true;
}

ArtifactPairPublicationResult PublishArtifactPair(
    const std::filesystem::path& data_temporary_path,
    const std::filesystem::path& metadata_temporary_path,
    const std::filesystem::path& data_final_path,
    const std::filesystem::path& metadata_final_path) {
  ArtifactPairPublicationResult result;

  const std::filesystem::path paths[] = {data_temporary_path, metadata_temporary_path,
                                         data_final_path, metadata_final_path};
  if (!ArtifactPathsAreDistinct(paths, std::size(paths), result.publication_error)) {
    return result;
  }

  bool data_final_exists;
  bool data_final_regular;
  bool metadata_final_exists;
  bool metadata_final_regular;
  if (!GetRegularFileState(data_final_path, data_final_exists, data_final_regular,
                           result.publication_error) ||
      !GetRegularFileState(metadata_final_path, metadata_final_exists, metadata_final_regular,
                           result.publication_error)) {
    return result;
  }
  if (data_final_regular && metadata_final_regular) {
    RemoveForCleanup(data_temporary_path, result.cleanup_error);
    RemoveForCleanup(metadata_temporary_path, result.cleanup_error);
    result.status = ArtifactPairPublicationStatus::kAlreadyComplete;
    return result;
  }

  bool data_temporary_exists;
  bool data_temporary_regular;
  bool metadata_temporary_exists;
  bool metadata_temporary_regular;
  if (!GetRegularFileState(data_temporary_path, data_temporary_exists, data_temporary_regular,
                           result.publication_error) ||
      !GetRegularFileState(metadata_temporary_path, metadata_temporary_exists,
                           metadata_temporary_regular, result.publication_error)) {
    return result;
  }
  if (!data_temporary_exists || !data_temporary_regular || !metadata_temporary_exists ||
      !metadata_temporary_regular) {
    result.publication_error = std::make_error_code(std::errc::no_such_file_or_directory);
    return result;
  }
  if ((data_final_exists && !data_final_regular) ||
      (metadata_final_exists && !metadata_final_regular)) {
    result.publication_error = std::make_error_code(std::errc::file_exists);
    return result;
  }

  if (data_final_regular) {
    std::filesystem::remove(data_final_path, result.publication_error);
    if (result.publication_error) {
      return result;
    }
  }
  if (metadata_final_regular) {
    std::filesystem::remove(metadata_final_path, result.publication_error);
    if (result.publication_error) {
      return result;
    }
  }

  std::filesystem::rename(metadata_temporary_path, metadata_final_path, result.publication_error);
  if (result.publication_error) {
    RemoveForCleanup(data_temporary_path, result.cleanup_error);
    RemoveForCleanup(metadata_temporary_path, result.cleanup_error);
    return result;
  }
  std::filesystem::rename(data_temporary_path, data_final_path, result.publication_error);
  if (result.publication_error) {
    RemoveForCleanup(data_temporary_path, result.cleanup_error);
    return result;
  }

  bool final_data_exists;
  bool final_data_regular;
  bool final_metadata_exists;
  bool final_metadata_regular;
  if (!GetRegularFileState(data_final_path, final_data_exists, final_data_regular,
                           result.publication_error) ||
      !GetRegularFileState(metadata_final_path, final_metadata_exists, final_metadata_regular,
                           result.publication_error) ||
      !final_data_exists || !final_data_regular || !final_metadata_exists ||
      !final_metadata_regular) {
    if (!result.publication_error) {
      result.publication_error = std::make_error_code(std::errc::io_error);
    }
    return result;
  }

  result.status = ArtifactPairPublicationStatus::kPublished;
  return result;
}

}  // namespace rex::graphics::diagnostic
