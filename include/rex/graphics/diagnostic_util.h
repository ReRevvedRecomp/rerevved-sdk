/**
 ******************************************************************************
 * ReXGlue - High-level Xbox 360 game recompilation framework                 *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace rex::graphics::diagnostic {

constexpr uint64_t kGuestPhysicalApertureSize = UINT64_C(0x20000000);

bool IsFenceCompletionValueValid(uint64_t completed_value);
bool IsFenceWaitReady(uint64_t completed_value, uint64_t awaited_value, bool value_was_signaled,
                      bool event_was_armed, bool wait_succeeded);
bool IsFenceFailureTerminal(bool tracking_is_unrecoverable, bool device_is_removed);
bool CanReleaseSubmittedGpuResources(bool queue_is_idle, bool device_is_removed);

struct Texture2DSourceRange {
  uint32_t base;
  uint32_t size;
};

bool GetTexture2DSourceRange(uint32_t base_page, uint32_t pitch_texels_div_32,
                             uint32_t width_texels, uint32_t height_texels, bool is_tiled,
                             uint32_t format, Texture2DSourceRange& range_out);

enum class ArtifactPairPublicationStatus {
  kFailed,
  kPublished,
  kAlreadyComplete,
};

struct ArtifactPairPublicationResult {
  ArtifactPairPublicationStatus status = ArtifactPairPublicationStatus::kFailed;
  std::error_code publication_error;
  std::error_code cleanup_error;

  bool succeeded() const { return status != ArtifactPairPublicationStatus::kFailed; }
};

// Both temporary files must be closed before this is called. An existing
// complete final pair is preserved. Otherwise metadata is published before the
// data file. This ordering does not make the pair atomic across interruption.
ArtifactPairPublicationResult PublishArtifactPair(
    const std::filesystem::path& data_temporary_path,
    const std::filesystem::path& metadata_temporary_path,
    const std::filesystem::path& data_final_path, const std::filesystem::path& metadata_final_path);

}  // namespace rex::graphics::diagnostic
