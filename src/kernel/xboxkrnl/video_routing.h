

/**
 * @file        kernel/xboxkrnl/video_routing.h
 * @brief       Vd* export routing between the detached-video delegate and graphics
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/system/interfaces/graphics.h>
#include <rex/system/interfaces/video_delegate.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace rex::memory {
class Memory;
}

namespace rex::kernel::xboxkrnl {

// Which target handled a guest GPU registration export.
enum class VideoExportRoute {
  kDelegate,
  kGraphics,
  kNoGpuWarned,  // First call without a target; the warning was logged.
  kNoGpuIgnored,
};

// Each routes to the delegate first, else the graphics system, else warns once
// per `warned` flag. Both targets are borrowed for the call.
VideoExportRoute RouteSetInterruptCallback(system::IDetachedVideoDelegate* video_delegate,
                                           system::IGraphicsSystem* graphics,
                                           std::atomic<bool>& warned, uint32_t callback,
                                           uint32_t user_data);
VideoExportRoute RouteInitializeRingBuffer(system::IDetachedVideoDelegate* video_delegate,
                                           system::IGraphicsSystem* graphics,
                                           std::atomic<bool>& warned, uint32_t ptr,
                                           uint32_t size_log2);
VideoExportRoute RouteEnableReadPointerWriteBack(system::IDetachedVideoDelegate* video_delegate,
                                                 system::IGraphicsSystem* graphics,
                                                 std::atomic<bool>& warned, uint32_t ptr,
                                                 uint32_t block_size_log2);

// Host views of the VdSwap guest arguments. The scalar pointers are read in the
// same order and on the same paths as before routing was factored out.
struct VdSwapInputs {
  void* reservation_host;  // 64-dword ring reservation.
  uint32_t reservation;
  const void* fetch_host;                // Six big-endian D3D texture-header fetch dwords.
  const rex::be<uint32_t>* frontbuffer;  // Read only by a debug assert.
  const rex::be<uint32_t>* texture_format;
  const rex::be<uint32_t>* color_space;
  const rex::be<uint32_t>* width;
  const rex::be<uint32_t>* height;
};

// Decodes the swap, writes the ring packet and then notifies the delegate.
// Returns false without writing or notifying when the frontbuffer virtual
// address has no physical mapping.
bool ProcessVdSwap(const memory::Memory& memory, system::IDetachedVideoDelegate* video_delegate,
                   const VdSwapInputs& inputs);

// Status Runtime::SetDetachedVideoDelegate reports for a requested change.
X_STATUS CheckDetachedVideoDelegateChange(bool tool_mode, bool has_graphics, bool has_current,
                                          bool has_replacement);

}  // namespace rex::kernel::xboxkrnl
