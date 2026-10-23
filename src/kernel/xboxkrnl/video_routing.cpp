/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * Vd* export routing and VdSwap decoding, kept apart from the export entries
 * so they can be unit tested.
 */

#include "video_routing.h"

#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

namespace rex::kernel::xboxkrnl {
namespace {

VideoExportRoute WarnNoGpuEmulation(const char* export_name, std::atomic<bool>& warned) {
  if (!warned.exchange(true)) {
    REXKRNL_WARN("{}: no GPU emulation loaded (gpu_plugin not set); call ignored", export_name);
    return VideoExportRoute::kNoGpuWarned;
  }
  return VideoExportRoute::kNoGpuIgnored;
}

}  // namespace

VideoExportRoute RouteSetInterruptCallback(system::IDetachedVideoDelegate* video_delegate,
                                           system::IGraphicsSystem* graphics,
                                           std::atomic<bool>& warned, uint32_t callback,
                                           uint32_t user_data) {
  if (video_delegate) {
    video_delegate->SetInterruptCallback(callback, user_data);
    return VideoExportRoute::kDelegate;
  }
  if (!graphics) {
    return WarnNoGpuEmulation("VdSetGraphicsInterruptCallback", warned);
  }
  graphics->SetInterruptCallback(callback, user_data);
  return VideoExportRoute::kGraphics;
}

VideoExportRoute RouteInitializeRingBuffer(system::IDetachedVideoDelegate* video_delegate,
                                           system::IGraphicsSystem* graphics,
                                           std::atomic<bool>& warned, uint32_t ptr,
                                           uint32_t size_log2) {
  if (video_delegate) {
    video_delegate->InitializeRingBuffer(ptr, size_log2);
    return VideoExportRoute::kDelegate;
  }
  if (!graphics) {
    return WarnNoGpuEmulation("VdInitializeRingBuffer", warned);
  }
  graphics->InitializeRingBuffer(ptr, size_log2);
  return VideoExportRoute::kGraphics;
}

VideoExportRoute RouteEnableReadPointerWriteBack(system::IDetachedVideoDelegate* video_delegate,
                                                 system::IGraphicsSystem* graphics,
                                                 std::atomic<bool>& warned, uint32_t ptr,
                                                 uint32_t block_size_log2) {
  if (video_delegate) {
    video_delegate->EnableReadPointerWriteBack(ptr, block_size_log2);
    return VideoExportRoute::kDelegate;
  }
  if (!graphics) {
    return WarnNoGpuEmulation("VdEnableRingBufferRPtrWriteBack", warned);
  }
  graphics->EnableReadPointerWriteBack(ptr, block_size_log2);
  return VideoExportRoute::kGraphics;
}

bool ProcessVdSwap(const memory::Memory& memory, system::IDetachedVideoDelegate* video_delegate,
                   const VdSwapInputs& inputs) {
  namespace xenos = rex::graphics::xenos;

  xenos::xe_gpu_texture_fetch_t gpu_fetch;
  memory::copy_and_swap_32_unaligned(&gpu_fetch, inputs.fetch_host, 6);

  // The fetch constant passed is not a true GPU fetch constant, but rather, the
  // fetch constant stored in the Direct3D 9 texture header, which contains the
  // address in one of the virtual mappings of the physical memory rather than
  // the physical address itself. We're emulating swapping in the GPU subsystem,
  // which works with GPU memory addresses (physical addresses directly) from
  // proper fetch constants like ones used to bind textures to shaders, not CPU
  // MMU addresses, so translation from virtual to physical is needed.
  uint32_t frontbuffer_virtual_address = gpu_fetch.base_address << 12;
  assert_true(*inputs.frontbuffer == frontbuffer_virtual_address);
  uint32_t frontbuffer_physical_address = memory.GetPhysicalAddress(frontbuffer_virtual_address);
  assert_true(frontbuffer_physical_address != UINT32_MAX);
  if (frontbuffer_physical_address == UINT32_MAX) {
    // Xenia-specific safety check.
    REXKRNL_ERROR("VdSwap: Invalid front buffer virtual address 0x{:08X}",
                  frontbuffer_virtual_address);
    return false;
  }
  gpu_fetch.base_address = frontbuffer_physical_address >> 12;

  uint32_t texture_format_value = *inputs.texture_format;
  auto texture_format = xenos::TextureFormat(texture_format_value);
  uint32_t color_space = *inputs.color_space;
  assert_true(texture_format == xenos::TextureFormat::k_8_8_8_8 ||
              texture_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16);
  assert_true(color_space == 0);  // RGB(0)
  assert_true(*inputs.width == 1 + gpu_fetch.size_2d.width);
  assert_true(*inputs.height == 1 + gpu_fetch.size_2d.height);

  // The caller seems to reserve 64 words (256b) in the primary ringbuffer
  // for this method to do what it needs. We just zero them out and send a
  // token value. It'd be nice to figure out what this is really doing so
  // that we could simulate it, though due to TCR I bet all games need to
  // use this method.
  std::memset(inputs.reservation_host, 0, 64 * 4);

  uint32_t offset = 0;
  auto* dwords = static_cast<rex::be<uint32_t>*>(inputs.reservation_host);

  // Write in the GPU texture fetch.
  dwords[offset++] =
      xenos::MakePacketType0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
  dwords[offset++] = gpu_fetch.dword_0;
  dwords[offset++] = gpu_fetch.dword_1;
  dwords[offset++] = gpu_fetch.dword_2;
  dwords[offset++] = gpu_fetch.dword_3;
  dwords[offset++] = gpu_fetch.dword_4;
  dwords[offset++] = gpu_fetch.dword_5;

  dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
  dwords[offset++] = xenos::kSwapSignature;
  dwords[offset++] = frontbuffer_physical_address;

  uint32_t width = *inputs.width;
  uint32_t height = *inputs.height;
  dwords[offset++] = width;
  dwords[offset++] = height;

  // Fill the rest of the buffer with NOP packets.
  for (uint32_t i = offset; i < 64; i++) {
    dwords[i] = xenos::MakePacketType2();
  }

  if (video_delegate) {
    system::DetachedVideoSwap swap{};
    swap.reservation = inputs.reservation;
    swap.frontbuffer_virtual = frontbuffer_virtual_address;
    swap.frontbuffer_physical = frontbuffer_physical_address;
    swap.fetch[0] = gpu_fetch.dword_0;
    swap.fetch[1] = gpu_fetch.dword_1;
    swap.fetch[2] = gpu_fetch.dword_2;
    swap.fetch[3] = gpu_fetch.dword_3;
    swap.fetch[4] = gpu_fetch.dword_4;
    swap.fetch[5] = gpu_fetch.dword_5;
    swap.texture_format = texture_format_value;
    swap.color_space = color_space;
    swap.width = width;
    swap.height = height;
    video_delegate->OnSwap(swap);
  }
  return true;
}

X_STATUS CheckDetachedVideoDelegateChange(bool tool_mode, bool has_graphics, bool has_current,
                                          bool has_replacement) {
  if (!has_replacement) {
    return X_STATUS_SUCCESS;
  }
  if (tool_mode || has_graphics) {
    return X_STATUS_NOT_SUPPORTED;
  }
  if (has_current) {
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  return X_STATUS_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl
