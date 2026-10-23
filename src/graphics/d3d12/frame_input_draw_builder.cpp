#include "frame_input_draw_builder.h"

#include <algorithm>
#include <cstdint>
#include <span>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>

#include "../pipeline/texture/prepared_cube.h"

namespace rex::graphics::d3d12 {
namespace {

constexpr uint32_t kComponentMappingAlwaysSet = 1U << 12;

constexpr uint8_t kSwizzledSignsAllUnsigned =
    uint8_t(xenos::TextureSign::kUnsigned) * uint8_t(0b01010101);
constexpr uint8_t kSwizzledSignsAllSigned =
    uint8_t(xenos::TextureSign::kSigned) * uint8_t(0b01010101);

bool IsStrictlyUnsigned(const diagnostic::DrawCaptureFrameInputTextureView& view) {
  return !view.is_signed && view.swizzled_signs == kSwizzledSignsAllUnsigned;
}

bool IsStrictlySignedRg16Float(const diagnostic::DrawCaptureFrameInputTextureView& view) {
  constexpr uint32_t kSelectedWidth = 1280;
  constexpr uint32_t kSelectedHeight = 720;
  constexpr uint32_t kSelectedPitch = 40;
  constexpr uint32_t kSelectedComponentMapping = 0x1B48;
  return view.is_signed && view.swizzled_signs == kSwizzledSignsAllSigned &&
         view.format == static_cast<uint32_t>(xenos::TextureFormat::k_16_16_FLOAT) &&
         view.dimension == static_cast<uint32_t>(xenos::DataDimension::k2DOrStacked) &&
         view.width == kSelectedWidth && view.height == kSelectedHeight &&
         view.pitch == kSelectedPitch && view.component_mapping == kSelectedComponentMapping &&
         view.depth == 1 && view.mip_min_level == 0 && view.mip_max_level == 0 && view.tiled &&
         !view.packed_mips;
}

bool IsSelectedSignedRg16Fetch(const xenos::xe_gpu_texture_fetch_t& fetch) {
  constexpr uint32_t kSelectedWidth = 1280;
  constexpr uint32_t kSelectedHeight = 720;
  constexpr uint32_t kSelectedPitch = 40;
  constexpr uint32_t kSelectedComponentMapping = 0xB48;
  return fetch.format == xenos::TextureFormat::k_16_16_FLOAT &&
         fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
         fetch.size_2d.width + 1 == kSelectedWidth && fetch.size_2d.height + 1 == kSelectedHeight &&
         fetch.pitch == kSelectedPitch && fetch.swizzle == kSelectedComponentMapping &&
         fetch.tiled && !fetch.packed_mips && fetch.mip_min_level == 0 &&
         fetch.mip_max_level == 0 && fetch.sign_x == xenos::TextureSign::kSigned &&
         fetch.sign_y == xenos::TextureSign::kSigned &&
         fetch.sign_z == xenos::TextureSign::kSigned && fetch.sign_w == xenos::TextureSign::kSigned;
}

bool IsNativeTextureShape(const diagnostic::DrawCaptureFrameInputTextureView& view) {
  return (IsStrictlyUnsigned(view) &&
          view.format == static_cast<uint32_t>(xenos::TextureFormat::k_8_8_8_8) &&
          view.dimension == static_cast<uint32_t>(xenos::DataDimension::k2DOrStacked) &&
          view.depth == 1 && view.mip_min_level == 0 && view.mip_max_level == 0 &&
          !view.packed_mips) ||
         IsStrictlySignedRg16Float(view);
}

bool IsValidatedPreparedTextureFetch(const xenos::xe_gpu_texture_fetch_t& fetch) {
  return (fetch.type == xenos::FetchConstantType::kTexture &&
          fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked) ||
         IsPreparedCubeFetch(fetch);
}

void ClassifyTextureDemand(const xenos::xe_gpu_texture_fetch_t& fetch, bool is_signed,
                           diagnostic::DrawCaptureFrameInputTextureView& view) {
  if (!IsValidatedPreparedTextureFetch(fetch)) {
    return;
  }
  view.swizzled_signs = texture_util::SwizzleSigns(fetch);
  view.required = is_signed ? texture_util::IsAnySignSigned(view.swizzled_signs)
                            : texture_util::IsAnySignNotSigned(view.swizzled_signs);
}

void NormalizeTextureDimensions(const xenos::xe_gpu_texture_fetch_t& fetch,
                                diagnostic::DrawCaptureFrameInputTextureView& view) {
  // TextureInfo stores fetch extents as minus-one values and pitch in texels.
  // The frame-input DTO carries the guest fetch dimensions and pitch fields so
  // the title-side resource proof can compare them with the borrowed words.
  view.width = fetch.size_2d.width + 1;
  view.height = fetch.size_2d.height + 1;
  view.depth = IsPreparedCubeFetch(fetch) ? 6 : 1;
  view.pitch = fetch.pitch;
}

void MarkTextureFailure(const diagnostic::DrawCaptureFrameInputTextureView& view,
                        FrameInputDrawBuilderResult& result,
                        bool optional_payload_failure = false) {
  if (!view.required && optional_payload_failure) {
    return;
  }
  if (view.required &&
      view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureUnresolved) {
    result.native_texture_unresolved = true;
  } else {
    result.non_native_texture_incomplete = true;
  }
  result.texture_bindings_complete = false;
}

}  // namespace

void FrameInputDrawBuilder::PopulateShaderViews(const Shader* vertex_shader,
                                                const Shader* pixel_shader,
                                                diagnostic::DrawCaptureFrameInputDrawView& draw) {
  auto populate = [](const Shader* shader, diagnostic::DrawCaptureFrameInputShaderView& view) {
    view = {};
    if (!shader) {
      return;
    }
    view.present = true;
    view.hash = shader->ucode_data_hash();
    view.words = std::span<const uint32_t>(shader->ucode_dwords(), shader->ucode_dword_count());
  };
  populate(vertex_shader, draw.vertex_shader);
  populate(pixel_shader, draw.pixel_shader);
}

bool FrameInputDrawBuilder::BuildPrepared(const FrameInputDrawBuilderInput& input,
                                          FrameInputDrawBuilderResult& result,
                                          diagnostic::DrawCaptureFrameInputDrawView& draw) {
  result = {};
  result.vertex_fetches_valid = input.vertex_shader != nullptr;
  if (!input.guest_vertex_shader || !input.guest_pixel_shader ||
      !input.guest_vertex_shader->is_ucode_analyzed() ||
      !input.guest_pixel_shader->is_ucode_analyzed()) {
    result.vertex_fetches_valid = false;
    result.texture_bindings_complete = false;
    result.non_native_texture_incomplete = true;
    return false;
  }

  if (input.vertex_shader) {
    const auto& constant_map = input.vertex_shader->constant_register_map();
    for (uint32_t i = 0; i < rex::countof(constant_map.vertex_fetch_bitmap); ++i) {
      uint32_t bits = constant_map.vertex_fetch_bitmap[i];
      uint32_t bit = 0;
      while (rex::bit_scan_forward(bits, &bit)) {
        bits &= ~(uint32_t(1) << bit);
        const uint32_t fetch_constant = i * 32 + bit;
        const xenos::xe_gpu_vertex_fetch_t fetch = input.registers.GetVertexFetch(fetch_constant);
        diagnostic::DrawCaptureFrameInputVertexFetchView view;
        view.fetch_constant = fetch_constant;
        const uint64_t physical_address = uint64_t(fetch.address) * sizeof(uint32_t);
        view.physical_address =
            physical_address > UINT32_MAX ? 0 : static_cast<uint32_t>(physical_address);
        view.size = fetch.size > UINT32_MAX / sizeof(uint32_t) ? 0 : fetch.size * sizeof(uint32_t);
        view.memory = input.memory;
        if (!fetch.size || fetch.size > UINT32_MAX / sizeof(uint32_t) ||
            physical_address > SharedMemory::kBufferSize ||
            uint64_t(view.size) > uint64_t(SharedMemory::kBufferSize) - physical_address) {
          result.vertex_fetches_valid = false;
        } else if ((input.dirty && input.dirty(view.physical_address, view.size)) ||
                   (input.gpu_written && input.gpu_written(view.physical_address, view.size))) {
          result.vertex_fetches_valid = false;
          result.vertex_fetch_dirty = true;
        } else if (input.readable && !input.readable(view.physical_address, view.size)) {
          result.vertex_fetches_valid = false;
        } else if (input.version) {
          view.memory_version = input.version(view.physical_address, view.size);
        }
        result.vertex_fetches.push_back(view);
      }
    }
  }

  result.texture_words.reserve(
      std::max<size_t>(xenos::kTextureFetchConstantCount,
                       2 * (input.guest_vertex_shader->texture_bindings().size() +
                            input.guest_pixel_shader->texture_bindings().size())));
  result.textures.reserve(result.texture_words.capacity());

  auto append_texture =
      [&](uint32_t stage, uint32_t binding_index, uint32_t fetch_constant, bool is_signed,
          const std::vector<Shader::PreparedSamplerBinding>& samplers, bool available,
          diagnostic::DrawCaptureReason unavailable_reason) -> bool {
    if (fetch_constant >= xenos::kTextureFetchConstantCount) {
      diagnostic::DrawCaptureFrameInputTextureView view;
      view.fetch_constant = fetch_constant;
      view.stage = stage;
      view.shader_binding_index = binding_index;
      view.is_signed = is_signed;
      view.available = false;
      view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
      result.textures.push_back(view);
      MarkTextureFailure(view, result);
      return false;
    }
    const auto fetch = input.registers.GetTextureFetch(fetch_constant);
    result.texture_words.push_back(
        {fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5});
    diagnostic::DrawCaptureFrameInputTextureView view;
    view.fetch_constant = fetch_constant;
    view.fetch_words = result.texture_words.back();
    view.stage = stage;
    view.shader_binding_index = binding_index;
    view.is_signed = is_signed;
    view.available = available;
    view.unavailable_reason = available ? diagnostic::DrawCaptureReason::kNone : unavailable_reason;
    ClassifyTextureDemand(fetch, is_signed, view);
    if (!available) {
      result.textures.push_back(view);
      MarkTextureFailure(view, result);
      return false;
    }
    const bool rg16_unsigned_companion = !view.is_signed && IsSelectedSignedRg16Fetch(fetch);
    // Keep signed and unsigned companions for one fetch; the selected RG16
    // owner is signed. Admit the optional companion through that signed range
    // without making it an owner or native dependency.
    if (rg16_unsigned_companion) {
      view.is_signed = true;
    }
    if (IsValidatedPreparedTextureFetch(fetch)) {
      NormalizeTextureDimensions(fetch, view);
    }
    const bool prepared = input.prepare_texture
                              ? input.prepare_texture(fetch, view)
                              : input.texture_cache.PrepareFrameInputTexture(fetch, view);
    if (rg16_unsigned_companion) {
      view.is_signed = false;
      view.required = false;
    }
    if (!prepared) {
      view.available = false;
      if (view.unavailable_reason == diagnostic::DrawCaptureReason::kNone) {
        view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
      }
      result.textures.push_back(view);
      MarkTextureFailure(view, result, true);
      return false;
    }
    NormalizeTextureDimensions(fetch, view);
    view.component_mapping |= kComponentMappingAlwaysSet;
    bool sampler_found = false;
    for (const auto& sampler : samplers) {
      if (sampler.fetch_constant != fetch_constant) {
        continue;
      }
      uint32_t value = 0;
      if (!input.sampler_value || !input.sampler_value(sampler, value)) {
        view.available = false;
        view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
        result.textures.push_back(view);
        MarkTextureFailure(view, result);
        return false;
      }
      if (sampler_found && view.sampler_value != value) {
        view.available = false;
        view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
        result.textures.push_back(view);
        MarkTextureFailure(view, result);
        return false;
      }
      sampler_found = true;
      view.sampler_value = value;
    }
    view.memory = input.memory;
    if (!sampler_found) {
      view.available = false;
      view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
      result.textures.push_back(view);
      MarkTextureFailure(view, result);
      return false;
    }
    const bool dirty_ledger = input.dirty && (input.dirty(view.base_address, view.base_size) ||
                                              input.dirty(view.mip_address, view.mip_size));
    view.gpu_written = input.gpu_written && (input.gpu_written(view.base_address, view.base_size) ||
                                             input.gpu_written(view.mip_address, view.mip_size));
    // Classify planned sources before CPU ownership/version reads. A same-frame
    // resolve is dirty; only its exact native owner bypasses that bit, while
    // dirty, stale, partial, and unknown writes remain rejected.
    const bool native_shape = IsNativeTextureShape(view);
    // Legacy RGBA8 stays a valid clean view, but its broad shape cannot use the
    // RG16 dirty-range exception; that exception is limited to the
    // source-backed 1280x720 signed-float producer.
    const bool selected_rg16_native_shape = IsStrictlySignedRg16Float(view);
    const bool native_candidate = view.required;
    diagnostic::DrawCaptureReason native_reason = diagnostic::DrawCaptureReason::kNone;
    if (native_candidate && input.native_source) {
      native_reason = input.native_source(view.base_address, view.base_size);
    }
    if (dirty_ledger && view.required &&
        !(selected_rg16_native_shape &&
          native_reason == diagnostic::DrawCaptureReason::kTextureUnresolved)) {
      view.available = false;
      view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
      result.textures.push_back(view);
      MarkTextureFailure(view, result);
      return false;
    }
    if (native_candidate && input.native_source) {
      if (native_reason == diagnostic::DrawCaptureReason::kTextureUnresolved) {
        if (native_shape) {
          view.available = false;
          view.memory_version = {};
          view.unavailable_reason = native_reason;
        } else {
          view.available = false;
          view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
        }
      } else if (native_reason != diagnostic::DrawCaptureReason::kNone) {
        view.available = false;
        view.unavailable_reason = native_reason;
      }
      if (!view.available) {
        result.textures.push_back(view);
        MarkTextureFailure(view, result);
        return false;
      }
    }
    if (view.required && (view.gpu_written ||
                          (input.readable && !input.readable(view.base_address, view.base_size)) ||
                          (input.readable && !input.readable(view.mip_address, view.mip_size)))) {
      view.available = false;
      view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
    }
    result.textures.push_back(view);
    if (!view.available && view.required) {
      MarkTextureFailure(view, result);
    }
    return view.available || !view.required;
  };

  auto append_shader = [&](uint32_t stage, const Shader* shader, bool host_present) {
    if (!shader) {
      return;
    }
    std::vector<Shader::PreparedTextureBinding> bindings;
    std::vector<Shader::PreparedSamplerBinding> samplers;
    if (!Shader::PrepareTextureBindings(shader->texture_bindings(), stage != 0, bindings,
                                        samplers)) {
      result.texture_bindings_complete = false;
      result.non_native_texture_incomplete = true;
      return;
    }
    if (!host_present) {
      if (!bindings.empty()) {
        result.texture_bindings_complete = false;
      }
      for (size_t i = 0; i < bindings.size(); ++i) {
        append_texture(stage, static_cast<uint32_t>(i), bindings[i].fetch_constant,
                       bindings[i].is_signed, samplers, false,
                       diagnostic::DrawCaptureReason::kTextureSource);
      }
      return;
    }
    for (size_t i = 0; i < bindings.size(); ++i) {
      // Record mismatched demand like an unsupported texture, preserving other
      // bindings and the omitted-host-shader path above.
      const auto fetch = input.registers.GetTextureFetch(bindings[i].fetch_constant);
      const bool matching_dimension =
          std::ranges::none_of(shader->texture_bindings(), [&](const auto& binding) {
            const auto& instruction = binding.fetch_instr;
            return binding.fetch_constant == bindings[i].fetch_constant &&
                   instruction.GetNonZeroResultComponents() &&
                   !(instruction.dimension == xenos::FetchOpDimension::kCube
                         ? IsPreparedCubeFetch(fetch)
                         : fetch.dimension == xenos::DataDimension::k2DOrStacked);
          });
      if (!append_texture(stage, static_cast<uint32_t>(i), bindings[i].fetch_constant,
                          bindings[i].is_signed, samplers, matching_dimension,
                          matching_dimension ? diagnostic::DrawCaptureReason::kNone
                                             : diagnostic::DrawCaptureReason::kTextureSource)) {
        // append_texture already records the specific failure reason.
      }
    }
  };

  append_shader(0, input.guest_vertex_shader, input.vertex_shader != nullptr);
  append_shader(1, input.guest_pixel_shader, input.pixel_shader != nullptr);

  if (input.guest_pixel_shader && !input.pixel_shader &&
      input.guest_pixel_shader->texture_bindings().empty()) {
    // An omitted pixel shader with no texture demand is valid.
  }

  if (draw.index.present) {
    draw.index.memory = input.memory;
    result.index_valid =
        draw.index.size != 0 && draw.index.physical_address <= SharedMemory::kBufferSize &&
        draw.index.size <= SharedMemory::kBufferSize - draw.index.physical_address &&
        (!input.dirty || !input.dirty(draw.index.physical_address, draw.index.size));
    result.index_dirty = !result.index_valid && draw.index.size != 0 && input.dirty &&
                         input.dirty(draw.index.physical_address, draw.index.size);
    if (result.index_valid && input.readable &&
        !input.readable(draw.index.physical_address, draw.index.size)) {
      result.index_valid = false;
    }
    const uint32_t element_size = draw.index.format == 0 ? 2 : draw.index.format == 1 ? 4 : 0;
    const uint64_t consumed_size = uint64_t(input.requested_index_count) * element_size;
    if (result.index_valid && element_size && input.requested_index_count &&
        input.requested_index_count <= draw.index.count && consumed_size <= draw.index.size &&
        input.version) {
      draw.index.memory_version =
          input.version(draw.index.physical_address, static_cast<uint32_t>(consumed_size));
    }
  }

  draw.vertex_fetches = result.vertex_fetches;
  draw.textures = result.textures;
  draw.cpu_data_complete =
      result.vertex_fetches_valid && result.index_valid && result.texture_bindings_complete;
  if (!draw.cpu_data_complete) {
    draw.unavailable_reason =
        (result.vertex_fetch_dirty || result.index_dirty)
            ? diagnostic::DrawCaptureReason::kMemexport
        : !result.vertex_fetches_valid ? diagnostic::DrawCaptureReason::kVertexFetch
        : !result.index_valid          ? diagnostic::DrawCaptureReason::kIndexSource
        : (result.native_texture_unresolved && !result.non_native_texture_incomplete)
            ? diagnostic::DrawCaptureReason::kTextureUnresolved
            : diagnostic::DrawCaptureReason::kTextureSource;
  } else {
    draw.unavailable_reason = diagnostic::DrawCaptureReason::kNone;
  }
  if ((input.vertex_shader && input.vertex_shader->memexport_eM_written() != 0) ||
      (input.pixel_shader && input.pixel_shader->memexport_eM_written() != 0)) {
    draw.cpu_data_complete = false;
    draw.unavailable_reason = diagnostic::DrawCaptureReason::kMemexport;
  }
  return draw.cpu_data_complete ||
         (draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureUnresolved);
}

}  // namespace rex::graphics::d3d12
