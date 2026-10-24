/**
 * @file        system/interfaces/graphics.h
 * @brief       Abstract graphics system interface for dependency injection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/system/xtypes.h>

// Forward declarations
namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::ui {
class GraphicsProvider;
class Presenter;
class WindowedAppContext;
}  // namespace rex::ui
namespace rex::system {
class KernelState;
}

namespace rex::system {

// Display aspect for one guest swap, snapshotted when the swap command is
// produced and carried with it to the GPU thread. policy_sequence counts the
// accepted SetPresentationAspect calls from 1; 0 means the swap carried none.
// override_valid is false for an explicit clear.
struct SwapPresentationAspect {
  uint32_t aspect_x = 0;
  uint32_t aspect_y = 0;
  uint64_t policy_sequence = 0;
  bool override_valid = false;
};

enum class SwapRefreshResult : uint8_t {
  kSkipped,  // The swap never reached the presenter.
  kOk,
  kFailed,  // The presenter kept the previous image.
};

// What the GPU thread did with one decoded swap.
struct SwapPresentationReport {
  SwapPresentationAspect request;  // As decoded from the swap command.
  uint32_t frontbuffer_ptr = 0;
  // Aspect handed to the presenter; 0 when the swap did not reach it.
  uint32_t display_x = 0;
  uint32_t display_y = 0;
  // Identifies the source content; repeated swaps of unchanged content share it.
  uint64_t content_id = 0;
  bool content_known = false;
  bool repeated = false;  // Meaningful when content_known.
  SwapRefreshResult refresh = SwapRefreshResult::kSkipped;
};

// Called on the GPU thread after each decoded swap, including skipped and
// failed ones. It must not call SetSwapPresentationObserver.
using SwapPresentationObserver = void (*)(void* context, const SwapPresentationReport& report);

class IGraphicsSystem {
 public:
  virtual ~IGraphicsSystem() = default;

  // Build the provider + presenter. Safe to call standalone (without a
  // Runtime) to stand up a window + ImGui for an installer. Idempotent.
  // Must be called before SetupGuestGpu if presentation is desired: some
  // backends (e.g. Vulkan) bake swapchain support into the provider, and
  // a headless provider from SetupGuestGpu cannot be upgraded in place.
  virtual X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) = 0;

  // Wire the GPU into the guest address space: MMIO, command processor,
  // vsync worker. Needs the Runtime's dispatcher + kernel state. If
  // SetupPresentation has not been called, a headless provider is built.
  virtual X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                 KernelState* kernel_state) = 0;

  virtual bool has_presentation() const = 0;

  // --- Optional capabilities, default no-op -------------------------------

  // Host presentation objects for ReXApp's overlay wiring; custom systems may
  // leave these null.
  virtual ui::GraphicsProvider* provider() const { return nullptr; }
  virtual ui::Presenter* presenter() const { return nullptr; }

  // Physical pixel size of the target the presenter will draw to. Reported
  // before SetupGuestGpu so size-dependent choices (automatic draw resolution)
  // can be made before the GPU caches are created; 0x0 means unknown.
  virtual void SetDisplaySurfaceSize(uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
  }

  // Physical pixel size last reported through SetDisplaySurfaceSize. Returns
  // false and zeroes both outputs while the size is unknown or minimized.
  virtual bool GetDisplaySurfaceSize(uint32_t& width, uint32_t& height) const {
    width = 0;
    height = 0;
    return false;
  }

  // Requests the display aspect for the next guest swap; call it on the guest
  // thread immediately before that swap. Both values positive request that
  // aspect and (0, 0) clears the override. The request is one-shot and rides
  // with the swap command, so the image keeps its aspect through GPU lag,
  // repeated swaps and repaints. Exactly one zero is invalid. Returns true
  // when the backend accepts the request. Returns false for an invalid pair or
  // unsupported backend, leaving any pending request unchanged.
  virtual bool SetPresentationAspect(uint32_t aspect_x, uint32_t aspect_y) {
    (void)aspect_x;
    (void)aspect_y;
    return false;
  }

  // Swap production side of SetPresentationAspect: moves the pending request,
  // if any, into `aspect` and clears it. Returns whether one was pending.
  virtual bool TakePresentationAspect(SwapPresentationAspect& aspect) {
    aspect = {};
    return false;
  }

  // Reports every decoded swap to `observer`; nullptr clears it. Returning
  // from the clear guarantees no callback is running.
  virtual void SetSwapPresentationObserver(SwapPresentationObserver observer, void* context) {
    (void)observer;
    (void)context;
  }

  // Called by the command processor after each decoded swap.
  virtual void NotifySwapPresentation(const SwapPresentationReport& report) { (void)report; }

  // Guest GPU services reached from the xboxkrnl Vd* exports.
  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
    (void)callback;
    (void)user_data;
  }
  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
    (void)ptr;
    (void)size_log2;
  }
  virtual void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
    (void)ptr;
    (void)block_size_log2;
  }

  // Persistent shader/pipeline storage under the cache root. Default: none.
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                       bool blocking) {
    (void)cache_root;
    (void)title_id;
    (void)blocking;
  }

  // Optional command-processor controls used while repairing guest GPU state.
  virtual bool PauseAndResetGpuWritePointer() { return false; }
  virtual void ResumeGpu() {}

  // One-shot convenience for callers that don't care about the split.
  X_STATUS Setup(runtime::FunctionDispatcher* function_dispatcher, KernelState* kernel_state,
                 ui::WindowedAppContext* app_context, bool with_presentation) {
    if (with_presentation && !has_presentation()) {
      X_STATUS status = SetupPresentation(app_context);
      if (XFAILED(status)) {
        return status;
      }
    }
    return SetupGuestGpu(function_dispatcher, kernel_state);
  }

  virtual void Shutdown() = 0;
};

}  // namespace rex::system
