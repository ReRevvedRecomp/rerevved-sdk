#pragma once

#include <array>
#include <cmath>
#include <cstring>

#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/util/draw.h>
#include <rex/system/xmemory.h>

namespace rex::graphics::d3d12 {

struct FrameInputCopyGeometry {
  std::array<float, 6> vertices;
  uint32_t address = 0;
  bool present = false;
  static bool Read(void* context, uint32_t address, std::span<uint8_t> destination) noexcept {
    const auto& geometry = *static_cast<const FrameInputCopyGeometry*>(context);
    if (!geometry.present || address < geometry.address ||
        uint64_t(address - geometry.address) + destination.size() > sizeof(geometry.vertices)) {
      return false;
    }
    std::memcpy(
        destination.data(),
        reinterpret_cast<const uint8_t*>(geometry.vertices.data()) + (address - geometry.address),
        destination.size());
    return true;
  }
};

// CPU-only value preparation shared by both observer boundaries. The reader
// must guard CPU accessibility and GPU ownership, as it does for draw inputs.
inline void PrepareFrameInputCopyPlan(diagnostic::DrawCaptureFrameInputCopyView& copy,
                                      const RegisterFile& registers, rex::memory::Memory& memory,
                                      uint32_t scale_x, uint32_t scale_y, bool fixed_16_truncated,
                                      FrameInputCopyGeometry& geometry, bool prepared = true) {
  using diagnostic::DrawCaptureReason;
  copy.cpu_data_complete = false;
  copy.unavailable_reason = DrawCaptureReason::kTextureSource;
  copy.planned_address = copy.planned_length = 0;
  copy.source_rect_present = false;
  geometry.present = false;
  copy.registers = {registers.values, RegisterFile::kRegisterCount};
  const auto fetch = registers.GetVertexFetch(0);
  copy.source_geometry.fetch_constant = 0;
  const uint64_t address = uint64_t(fetch.address) * sizeof(uint32_t);
  constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFFu;
  const uint32_t physical_address = uint32_t(address) & kPhysicalAddressMask;
  copy.source_geometry.physical_address = address <= UINT32_MAX ? uint32_t(address) : 0;
  copy.source_geometry.size =
      fetch.size <= UINT32_MAX / sizeof(uint32_t) ? fetch.size * sizeof(uint32_t) : 0;
  const auto control = registers.Get<reg::RB_COPY_CONTROL>();
  const auto surface = registers.Get<reg::RB_SURFACE_INFO>();
  if ((control.copy_command != xenos::CopyCommand::kRaw &&
       control.copy_command != xenos::CopyCommand::kConvert) ||
      surface.msaa_samples > xenos::MsaaSamples::k4X ||
      // ResolveCoordinateInfo stores each resolution scale in three bits.
      scale_x > 7 || scale_y > 7) {
    return;
  }
  const bool supported_input = control.copy_src_select <= xenos::kMaxColorRenderTargets &&
                               surface.surface_pitch >= 8 && scale_x && scale_y;
  if (prepared && !supported_input) {
    return;
  }
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 6 || address > UINT32_MAX ||
      physical_address > SharedMemory::kBufferSize ||
      copy.source_geometry.size > uint64_t(SharedMemory::kBufferSize) - physical_address) {
    copy.unavailable_reason = DrawCaptureReason::kVertexFetch;
    return;
  }
  std::array<uint8_t, 6 * sizeof(float)> vertex_bytes;
  if (!copy.source_geometry.memory.Copy(copy.source_geometry.physical_address, vertex_bytes)) {
    copy.unavailable_reason = DrawCaptureReason::kVertexFetch;
    return;
  }
  std::array<float, 6> vertices;
  std::memcpy(vertices.data(), vertex_bytes.data(), vertex_bytes.size());
  geometry.vertices = vertices;
  geometry.address = copy.source_geometry.physical_address;
  geometry.present = true;
  bool finite_vertices = true;
  for (auto& vertex : vertices) {
    vertex = xenos::GpuSwap(vertex, fetch.endian);
    finite_vertices &= std::isfinite(vertex);
    if (prepared && !finite_vertices) {
      copy.unavailable_reason = DrawCaptureReason::kVertexFetch;
      return;
    }
  }
  const bool nondegenerate_vertices = !(vertices[0] == vertices[2] && vertices[0] == vertices[4]) &&
                                      !(vertices[1] == vertices[3] && vertices[1] == vertices[5]);
  if (prepared && !nondegenerate_vertices) {
    copy.unavailable_reason = DrawCaptureReason::kNoEffect;
    return;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(registers, scissor, false);
  const bool nonempty_scissor = scissor.extent[0] && scissor.extent[1];
  if (prepared && !nonempty_scissor) {
    copy.unavailable_reason = DrawCaptureReason::kNoEffect;
    return;
  }
  const auto destination = registers.Get<reg::RB_COPY_DEST_INFO>();
  const auto destination_pitch = registers.Get<reg::RB_COPY_DEST_PITCH>();
  // Bound the legacy signed 32-bit tiled-offset arithmetic before planning.
  // Array destinations and regions outside the existing resolve domain are
  // unsupported by this CPU input boundary.
  const bool supported_destination =
      !destination.copy_dest_array && destination_pitch.copy_dest_pitch &&
      destination_pitch.copy_dest_pitch <= xenos::kMaxResolveSize &&
      surface.surface_pitch <= xenos::kMaxResolveSize &&
      uint64_t(scissor.offset[0]) + scissor.extent[0] <= xenos::kMaxResolveSize &&
      uint64_t(scissor.offset[1]) + scissor.extent[1] <= xenos::kMaxResolveSize;
  if (prepared && !supported_destination) {
    return;
  }
  const bool is_depth = control.copy_src_select >= xenos::kMaxColorRenderTargets;
  const auto format =
      is_depth ? DepthRenderTargetToTextureFormat(registers.Get<reg::RB_DEPTH_INFO>().depth_format)
               : xenos::TextureFormat(destination.copy_dest_format);
  const auto* format_info = FormatInfo::Get(format);
  if (prepared && (!format_info->bits_per_pixel ||
                   (!is_depth && format_info->type != FormatType::kResolvable))) {
    copy.unavailable_reason = DrawCaptureReason::kTextureFormat;
    return;
  }
  // ResolveInfo adds the destination base in uint32_t. Plan relative to zero
  // first so an overflowing guest destination never enters its assertions.
  RegisterFile planning_registers = registers;
  planning_registers[XE_GPU_REG_RB_COPY_DEST_BASE] = 0;
  draw_util::ResolveInfo resolve;
  if (!draw_util::GetResolveInfo(planning_registers, memory, scale_x, scale_y, fixed_16_truncated,
                                 fixed_16_truncated, resolve, geometry.vertices)) {
    return;
  }
  copy.source_x = resolve.coordinate_info.edram_offset_x_div_8 * 8;
  copy.source_y = resolve.coordinate_info.edram_offset_y_div_8 * 8;
  copy.source_width = resolve.coordinate_info.width_div_8 * 8;
  copy.source_height = resolve.height_div_8 * 8;
  copy.source_rect_present = copy.source_width != 0 && copy.source_height != 0;
  const uint64_t planned_address =
      uint64_t(registers[XE_GPU_REG_RB_COPY_DEST_BASE]) + resolve.copy_dest_extent_start;
  if (planned_address > SharedMemory::kBufferSize ||
      uint64_t(resolve.copy_dest_extent_length) >
          uint64_t(SharedMemory::kBufferSize) - planned_address) {
    return;
  }
  copy.planned_address = uint32_t(planned_address);
  copy.planned_length = resolve.copy_dest_extent_length;
  if (copy.copy_mode != "resolve") {
    copy.unavailable_reason = DrawCaptureReason::kReadback;
    return;
  }
  if (!copy.source_rect_present || !copy.planned_length) {
    copy.unavailable_reason = DrawCaptureReason::kNoEffect;
    return;
  }
  if (!supported_input || !finite_vertices || !nondegenerate_vertices || !nonempty_scissor ||
      !supported_destination || control.color_clear_enable || control.depth_clear_enable) {
    // Legacy rectangle extraction does not admit unsupported CPU input or
    // prove that either original clear occurred.
    return;
  }
  copy.cpu_data_complete = true;
  copy.unavailable_reason = DrawCaptureReason::kNone;
}

}  // namespace rex::graphics::d3d12
