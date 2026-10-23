#pragma once

#include <array>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics {

// Callback-local gamma storage; register values remain synchronously borrowed.
struct FrameInputSwapView {
  std::array<uint32_t, 256> gamma_256;
  std::array<uint32_t, 128 * 3> gamma_pwl_rgb;

  diagnostic::DrawCaptureFrameInputSwapView Build(const RegisterFile& registers,
                                                  const reg::DC_LUT_30_COLOR* table,
                                                  const reg::DC_LUT_PWL_DATA* pwl,
                                                  CommandProcessor::SwapPostEffect post_effect,
                                                  uint64_t id, uint64_t frame, uint64_t submission,
                                                  uint64_t frontbuffer_ptr, uint32_t width,
                                                  uint32_t height) noexcept {
    for (size_t i = 0; i < gamma_256.size(); ++i) {
      gamma_256[i] = table[i].value;
    }
    for (size_t i = 0; i < gamma_pwl_rgb.size(); ++i) {
      gamma_pwl_rgb[i] = pwl[i].value;
    }
    diagnostic::DrawCaptureFrameInputSwapView view;
    view.id = id;
    view.frame = frame;
    view.submission = submission;
    view.frontbuffer_ptr = frontbuffer_ptr;
    view.frontbuffer_width = width;
    view.frontbuffer_height = height;
    view.registers = std::span<const uint32_t>(registers.values, RegisterFile::kRegisterCount);
    const auto format = registers.GetTextureFetch(0).format;
    view.use_pwl_gamma_ramp = format == xenos::TextureFormat::k_2_10_10_10 ||
                              format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
    view.use_fxaa = post_effect == CommandProcessor::SwapPostEffect::kFxaa ||
                    post_effect == CommandProcessor::SwapPostEffect::kFxaaExtreme;
    view.is_8bpc = !view.use_pwl_gamma_ramp && !view.use_fxaa;
    view.gamma_256 = gamma_256;
    view.gamma_pwl_rgb = gamma_pwl_rgb;
    return view;
  }
};

}  // namespace rex::graphics
