#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics::testing {

struct CompanionTextureRecord {
  std::uint32_t stage = 0;
  std::uint32_t shader_binding_index = 0;
  std::uint32_t fetch_constant = 0;
  std::vector<std::uint32_t> fetch_words;
  std::uint32_t format = 0;
  std::uint32_t endianness = 0;
  std::uint32_t dimension = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t depth = 0;
  std::uint32_t pitch = 0;
  std::uint32_t mip_min_level = 0;
  std::uint32_t mip_max_level = 0;
  bool tiled = false;
  bool packed_mips = false;
  std::uint32_t base_address = 0;
  std::uint32_t base_size = 0;
  std::uint32_t mip_address = 0;
  std::uint32_t mip_size = 0;
  std::uint32_t sampler_value = 0;
  std::uint32_t component_mapping = 0;
  std::uint8_t swizzled_signs = 0;
  bool is_signed = false;
  bool required = true;
  bool available = false;
  bool gpu_written = false;
  std::uint32_t unavailable_reason = 0;
  std::uint64_t memory_version_identity = 0;
  std::uint64_t memory_version_generation = 0;
  bool memory_version_current_before = false;
  bool memory_version_current_after = false;
};

struct CompanionDrawRecord {
  std::uint64_t id = 0;
  std::uint64_t frame = 0;
  std::uint64_t submission = 0;
  bool prepared = false;
  bool original_host_issued = false;
  bool cpu_data_complete = false;
  std::uint32_t unavailable_reason = 0;
  std::uint32_t execution_status = 0;
  std::uint32_t execution_reason = 0;
  std::uint32_t primitive_type = 0;
  std::uint32_t requested_index_count = 0;
  std::uint64_t vertex_shader_hash = 0;
  std::uint64_t pixel_shader_hash = 0;
  std::vector<std::uint32_t> registers;
  std::vector<std::uint32_t> vertex_shader_words;
  std::vector<std::uint32_t> pixel_shader_words;
  std::vector<CompanionTextureRecord> textures;
};

struct CompanionSwapRecord {
  std::uint64_t id = 0;
  std::uint64_t frame = 0;
  std::uint64_t submission = 0;
  std::uint64_t frontbuffer_ptr = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool original_succeeded = false;
};

struct CompanionControlResult {
  std::string mode;
  bool progressed = false;
  bool completion_observed = false;
  std::vector<CompanionDrawRecord> original_draws;
  std::vector<CompanionDrawRecord> prepared_draws;
  std::vector<CompanionSwapRecord> original_swaps;
  std::vector<CompanionSwapRecord> prepared_swaps;
  std::uint32_t source_width = 0;
  std::uint32_t source_height = 0;
  std::uint32_t source_pitch = 0;
  std::uint32_t source_format = 0;
  std::uint32_t source_endianness = 0;
  std::uint32_t source_swizzle = 0;
  std::uint32_t source_mip_min_level = 0;
  std::uint32_t source_mip_max_level = 0;
  bool source_tiled = false;
  bool source_packed_mips = false;
  std::vector<std::uint32_t> source_fetch_words;
};

// Runs one actual PM4 draw with a CPU-written texture source. The default
// case is an unsigned linear RGBA8 source; REREVVED_PREPARED_COMPANION_CASE
// selects unsigned, signed, mixed, or bc1 descriptor controls. The observer
// receives synchronous original and Prepared callbacks before any borrowed
// field is released, and is forwarded every callback it requests.
int RunCompanionControl(
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    CompanionControlResult& result, std::string& error);

}  // namespace rex::graphics::testing
