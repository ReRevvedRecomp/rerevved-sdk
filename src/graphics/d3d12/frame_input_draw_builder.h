#pragma once

#include <array>
#include <functional>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/register_file.h>

namespace rex::graphics::d3d12 {

// Private inputs for the prepared draw constructor. The builder owns only the
// borrowed DTO backing vectors for the duration of the observer callback.
struct FrameInputDrawBuilderInput {
  const RegisterFile& registers;
  const TextureCache& texture_cache;
  const Shader* vertex_shader = nullptr;
  const Shader* pixel_shader = nullptr;
  const Shader* guest_vertex_shader = nullptr;
  const Shader* guest_pixel_shader = nullptr;
  diagnostic::DrawCaptureGuestMemoryReader memory;
  // The ledger records this frame's known writes. GPU ownership is kept
  // separate so an exact native producer can bypass only the latter.
  std::function<bool(uint32_t, uint32_t)> dirty;
  std::function<bool(uint32_t, uint32_t)> gpu_written;
  std::function<bool(uint32_t, uint32_t)> readable;
  std::function<diagnostic::DrawCaptureGuestMemoryVersion(uint32_t, uint32_t)> version;
  std::function<bool(const Shader::PreparedSamplerBinding&, uint32_t&)> sampler_value;
  std::function<diagnostic::DrawCaptureReason(uint32_t, uint32_t)> native_source;
  uint32_t requested_index_count = 0;
  // Internal test seam for a targeted payload-preparation failure. The
  // normal path leaves this unset and uses TextureCache directly.
  std::function<bool(const xenos::xe_gpu_texture_fetch_t&,
                     diagnostic::DrawCaptureFrameInputTextureView&)>
      prepare_texture;
};

struct FrameInputDrawBuilderResult {
  std::vector<diagnostic::DrawCaptureFrameInputVertexFetchView> vertex_fetches;
  std::vector<std::array<uint32_t, 6>> texture_words;
  std::vector<diagnostic::DrawCaptureFrameInputTextureView> textures;
  bool vertex_fetches_valid = false;
  bool vertex_fetch_dirty = false;
  bool index_valid = true;
  bool index_dirty = false;
  bool texture_bindings_complete = true;
  bool native_texture_unresolved = false;
  bool non_native_texture_incomplete = false;
};

class FrameInputDrawBuilder final {
 public:
  // Copies the analyzed guest shader identity into the borrowed draw view.
  // The span points into the caller-owned Shader and is valid only for the
  // synchronous observer callback.
  static void PopulateShaderViews(const Shader* vertex_shader, const Shader* pixel_shader,
                                  diagnostic::DrawCaptureFrameInputDrawView& draw);

  static bool BuildPrepared(const FrameInputDrawBuilderInput& input,
                            FrameInputDrawBuilderResult& result,
                            diagnostic::DrawCaptureFrameInputDrawView& draw);
};

}  // namespace rex::graphics::d3d12
