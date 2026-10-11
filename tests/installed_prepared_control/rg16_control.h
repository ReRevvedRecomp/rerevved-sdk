#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics::testing {

struct Rg16ControlCopyRecord {
  std::uint64_t id = 0;
  std::uint64_t frame = 0;
  std::uint64_t submission = 0;
  std::string mode;
  bool original_succeeded = false;
  std::uint32_t written_address = 0;
  std::uint32_t written_length = 0;
  std::uint32_t planned_address = 0;
  std::uint32_t planned_length = 0;
  bool cpu_data_complete = false;
  bool prepared = false;
};

struct Rg16RawTextureRecord {
  std::uint32_t address = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Pitch is reported in texels. The Xenos fetch descriptor uses the
  // equivalent 32-byte fetch-unit value in its fifth word.
  std::uint32_t pitch = 0;
  std::uint32_t fetch_pitch = 0;
  std::uint32_t format = 0;
  std::uint8_t signs = 0;
  std::uint32_t extent = 0;
  // A non-zero value is copied from an actual XenosFenceTrace D3D12 fence
  // completion event. Positive Prepared mode deliberately leaves this zero.
  std::uint64_t completed_fence = 0;
  bool raw_gpu_proof = false;
  std::vector<std::uint8_t> linear_rg16_float;
};

struct Rg16ControlResult {
  bool progressed = false;
  bool completion_observed = false;
  bool terminal_capture = false;
  std::uint32_t terminal_width = 0;
  std::uint32_t terminal_height = 0;
  std::uint32_t terminal_stride = 0;
  std::vector<std::uint8_t> terminal_pixels;
  // The producer addresses and extents bind a raw companion buffer back to
  // the positive fixture's original/prepared planned copy records. Allocation
  // addresses are per-process and may rebase; identity is carried by the
  // copy records, descriptor words, register banks, and shader words.
  std::vector<Rg16ControlCopyRecord> original_copies;
  std::vector<Rg16ControlCopyRecord> prepared_copies;
  Rg16RawTextureRecord producer_a;
  Rg16RawTextureRecord producer_b;
  // Prepared draw identity in callback order. Each bank is copied while its
  // borrowed callback view is alive, so a raw companion can compare the
  // complete three-draw PM4 identity with the positive run.
  std::vector<std::vector<std::uint32_t>> draw_registers;
  std::vector<std::vector<std::uint32_t>> draw_vertex_shader_words;
  std::vector<std::vector<std::uint32_t>> draw_pixel_shader_words;
  std::vector<std::uint32_t> vertex_shader_words;
  std::vector<std::uint32_t> pixel_shader_words;
  std::vector<std::uint32_t> consumer_registers;
  std::vector<std::uint32_t> producer_registers;
  // RunRg16Control selects the positive Prepared fixture unless the process
  // environment contains REREVVED_PREPARED_CONTROL_RAW=1. Positive mode keeps
  // these raw fields empty; raw mode is an opt-in companion proof.
  bool readback_mode = false;
  bool raw_gpu_proof = false;
  // In raw mode this is a non-zero XenosFenceTrace D3D12 completion value and
  // raw_fence_trace_path names the required trace output. Both remain empty or
  // zero for the positive Prepared run.
  std::uint64_t raw_fence_value = 0;
  std::string raw_fence_trace_path;
  // Raw-mode provenance binds the completion event to the final swap packet
  // watched by XenosFenceTrace. Positive Prepared mode leaves these zero.
  std::uint64_t raw_reservation_token = 0;
  std::uint64_t raw_fence_correlation_token = 0;
  std::uint64_t raw_fence_correlation_mask = 0;
  std::uint64_t raw_fence_range_identity = 0;
  std::uint32_t raw_watched_packet_start = 0;
  std::uint32_t raw_watched_packet_end = 0;
  std::uint32_t raw_watched_range_start = 0;
  std::uint32_t raw_watched_range_end = 0;
};

// The observer may be null. The internal observer still records original and
// Prepared copies; a forwarded observer receives original callbacks always and
// Prepared callbacks only when it requests those views.
// Runs two actual format-31 (RG16_FLOAT) resolve producers and a same-frame
// consumer through the installed xenos/d3d12 plugin. The observer sees each
// borrowed original and Prepared view synchronously before it is released.
int RunRg16Control(
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    Rg16ControlResult& result, std::string& error);

}  // namespace rex::graphics::testing
