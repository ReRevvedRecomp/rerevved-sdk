/**
 * @file        system/interfaces/video_delegate.h
 * @brief       Guest video services for titles that run without an IGraphicsSystem
 */

#pragma once

#include <cstdint>

namespace rex::system {

// One decoded VdSwap call. Values are host-endian. `fetch` holds the six
// frontbuffer fetch dwords as written to the ring, with the base address
// already rewritten to `frontbuffer_physical >> 12`.
struct DetachedVideoSwap {
  uint32_t reservation;  // Guest address of the 64-dword ring reservation.
  uint32_t frontbuffer_virtual;
  uint32_t frontbuffer_physical;
  uint32_t fetch[6];
  uint32_t texture_format;
  uint32_t color_space;
  uint32_t width;
  uint32_t height;
};

// Receives the guest GPU registration exports when a title provides its own
// video services instead of an IGraphicsSystem. Register it through
// Runtime::SetDetachedVideoDelegate.
//
// SetInterruptCallback, InitializeRingBuffer and EnableReadPointerWriteBack
// replace the SDK's handling of VdSetGraphicsInterruptCallback,
// VdInitializeRingBuffer and VdEnableRingBufferRPtrWriteBack. VdSwap still
// writes its ring packet; OnSwap follows that write and is skipped when the
// frontbuffer has no physical mapping. Other Vd* exports keep SDK behavior.
//
// Threading: every method runs synchronously on the calling guest XThread and
// may run concurrently from several guest threads. Methods must not block on
// host presentation and must not call Runtime::SetDetachedVideoDelegate.
// Exports hold their own reference for the duration of a call, so a cleared
// delegate can still receive a call that started before the clear.
class IDetachedVideoDelegate {
 public:
  virtual ~IDetachedVideoDelegate() = default;

  // Guest callback address and its user_data argument.
  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data) = 0;
  // Ring buffer address (from MmGetPhysicalAddress) and log2 of its size.
  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) = 0;
  // Read-pointer writeback address and log2 of the writeback block size.
  virtual void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) = 0;
  virtual void OnSwap(const DetachedVideoSwap& swap) = 0;
};

}  // namespace rex::system
