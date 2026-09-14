/**
 ******************************************************************************
 * ReXGlue - Owned live draw capture handoff                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rex::graphics::d3d12 {
class D3D12CommandProcessor;
}

namespace rex::graphics::diagnostic {

// Returns the runtime-owned mailbox shared by the graphics producer and title
// coordinator. The storage is in the rexcore implementation, rather than in
// this header, so consumers do not create competing slots.
std::shared_ptr<class DrawCaptureMailbox> GetDrawCaptureMailbox();

// The title owns the request token. The graphics producer can only complete the
// exact request that it claimed, so a late GPU completion cannot reach a newer
// request after cancellation or reset.
struct DrawCaptureRequest {
  std::filesystem::path output_path;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
};

struct DrawCaptureVertexFetch {
  uint32_t fetch_constant = 0;
  uint32_t base = 0;
  std::vector<uint8_t> bytes;
};

struct DrawCaptureIndex {
  bool present = false;
  uint32_t guest_base = 0;
  uint32_t format = 0;
  uint32_t endianness = 0;
  uint32_t count = 0;
  uint32_t dma_count = 0;
  uint32_t dma_length = 0;
  std::vector<uint8_t> bytes;
};

// This is an owned CPU snapshot. It contains no D3D12 resources, guest
// pointers, or views into command-processor state.
struct DrawCaptureSnapshot {
  uint64_t frame = 0;
  uint64_t submission = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;

  uint32_t primitive_type = 0;
  uint32_t requested_index_count = 0;
  bool major_mode_explicit = false;
  bool indexed = false;
  uint32_t guest_draw_vertex_count = 0;
  uint32_t host_draw_vertex_count = 0;
  uint32_t guest_primitive_type = 0;
  uint32_t host_primitive_type = 0;
  uint32_t host_vertex_shader_type = 0;
  uint32_t tessellation_mode = 0;
  uint32_t guest_index_base = 0;
  uint32_t guest_index_size = 0;
  uint32_t host_index_format = 0;
  uint32_t host_shader_index_endian = 0;
  bool host_primitive_reset_enabled = false;
  bool color_target_written = false;
  uint32_t used_texture_mask = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t native_topology = 0;
  uint32_t native_vertex_count = 0;
  uint32_t native_index_count = 0;
  uint32_t native_instance_count = 1;
  uint32_t native_start_vertex = 0;
  uint32_t native_start_index = 0;
  int32_t native_base_vertex = 0;
  uint32_t native_start_instance = 0;
  bool half_pixel_offset = false;

  std::vector<uint32_t> registers;
  std::vector<uint32_t> vertex_ucode;
  std::vector<uint32_t> pixel_ucode;
  std::vector<DrawCaptureVertexFetch> vertex_fetches;
  DrawCaptureIndex index;
  std::vector<uint8_t> edram_before;
  std::vector<uint8_t> edram_after;
};

// A texture use is attached to one translated shader binding.  The producer
// keeps the fetch and binding coordinates even when the cache cannot resolve
// the data (for example, a static shader binding with a zero dynamic mask).
struct DrawCaptureTextureUse {
  static constexpr uint32_t kUnavailableTexture = std::numeric_limits<uint32_t>::max();

  uint32_t texture_index = kUnavailableTexture;
  uint32_t stage = 0;
  uint32_t shader_binding_index = 0;
  uint32_t fetch_constant = 0;
  uint32_t dimension = 0;
  uint32_t sampler_value = 0;
  uint32_t component_mapping = 0;
  uint32_t host_swizzle = 0;
  uint8_t swizzled_signs = 0;
  bool is_signed = false;
  bool data_available = false;
};

// This is an owned host-resource mip-0 payload.  The guest key is retained so
// a title can match unresolved static bindings without reading guest memory.
struct DrawCaptureTexture {
  uint32_t guest_base_page = 0;
  uint32_t guest_mip_page = 0;
  uint32_t guest_width = 0;
  uint32_t guest_height = 0;
  uint32_t guest_depth_or_array_size = 1;
  uint32_t guest_pitch = 0;
  uint32_t guest_format = 0;
  uint32_t guest_endianness = 0;
  bool guest_tiled = false;
  bool guest_packed_mips = false;
  bool scaled_resolve = false;

  uint32_t host_format = 0;
  uint32_t host_width = 0;
  uint32_t host_height = 0;
  uint32_t host_depth_or_array_size = 1;
  uint32_t mip_level = 0;
  uint32_t row_pitch = 0;
  uint32_t row_size = 0;
  uint32_t row_count = 0;
  std::vector<uint8_t> bytes;
};

// DrawCaptureSnapshot remains the established one-draw DTO.  Frame draws use
// it as their guest/host draw payload and add the point-of-use texture uses.
struct DrawCaptureFrameDraw {
  DrawCaptureSnapshot draw;
  // Original guest shader identity is retained even when host setup skips a
  // pixel shader or rewrites the bound shader for an early-return event.
  uint64_t guest_vertex_shader_hash = 0;
  uint64_t guest_pixel_shader_hash = 0;
  bool texture_bindings_complete = true;
  std::vector<DrawCaptureTextureUse> texture_uses;
};

struct DrawCaptureFrameEvent {
  enum class Kind : uint32_t { kDraw, kCopy, kSwap };

  uint64_t id = 0;
  uint64_t frame = 0;
  uint64_t submission = 0;
  Kind kind = Kind::kDraw;
  bool success = false;
  bool host_issued = false;
  DrawCaptureFrameDraw draw;
  std::string copy_mode;
  uint64_t frontbuffer_ptr = 0;
  uint32_t frontbuffer_width = 0;
  uint32_t frontbuffer_height = 0;
};

struct DrawCaptureGamma {
  bool use_pwl_gamma_ramp = false;
  bool use_fxaa = false;
  bool is_8bpc = false;
  uint32_t source_format = 0;
  uint32_t output_format = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  std::vector<uint32_t> gamma_256;
  std::vector<uint32_t> gamma_pwl_rgb;
};

struct DrawCaptureFinalSwap {
  uint64_t frontbuffer_ptr = 0;
  uint32_t source_format = 0;
  uint32_t source_width = 0;
  uint32_t source_height = 0;
  uint32_t source_row_pitch = 0;
  uint32_t source_row_size = 0;
  uint32_t source_row_count = 0;
  std::vector<uint8_t> source_bytes;
  uint32_t output_format = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  uint32_t output_row_pitch = 0;
  uint32_t output_row_size = 0;
  uint32_t output_row_count = 0;
  std::vector<uint8_t> output_bytes;
  DrawCaptureGamma gamma;
};

// All vectors in this DTO own their storage and are safe to retain after the
// D3D12 command processor has returned to guest execution.
struct DrawCaptureFrameSnapshot {
  uint64_t start_frame = 0;
  uint64_t start_submission = 0;
  uint64_t end_frame = 0;
  uint64_t end_submission = 0;
  std::vector<DrawCaptureFrameEvent> events;
  std::vector<DrawCaptureTexture> textures;
  DrawCaptureFinalSwap final_swap;
};

class DrawCaptureMailbox {
 public:
  struct Token {
    uint64_t value = 0;

    explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(Token, Token) = default;
  };

  // Consumer operations. A mailbox has one active request and one completed
  // snapshot. Start returns an invalid token when another request is active.
  Token Start(const DrawCaptureRequest& request);
  // Starts a frame request without requiring a shader selector or a capture
  // directory. The optional path is retained only for exact arm latching.
  Token StartFrame(const std::filesystem::path& output_path = {});
  bool IsPending(Token token) const;
  std::shared_ptr<const DrawCaptureSnapshot> TryTake(Token token);
  std::shared_ptr<const DrawCaptureFrameSnapshot> TryTakeFrame(Token token);
  bool Cancel(Token token);

  // The producer retains the selected token even after cancellation, so an
  // armed live capture cannot become an ordinary disk capture or a newer request.
  Token GetRequestToken(const std::filesystem::path& output_path) const;
  // Producer operations. Claim must match the request exactly. Publish and
  // Fail are token-bound and silently reject stale or cancelled completions.
  Token Claim(Token expected_token, const std::filesystem::path& output_path,
              uint64_t vertex_shader_hash, uint64_t pixel_shader_hash);
  bool Publish(Token token, std::shared_ptr<const DrawCaptureSnapshot> snapshot);
  Token ClaimFrame(Token expected_token, const std::filesystem::path& output_path = {});
  bool IsFrameRequest(Token token) const;
  Token GetFrameRequestToken() const;
  std::filesystem::path GetRequestPath(Token token) const;
  bool PublishFrame(Token token, std::shared_ptr<const DrawCaptureFrameSnapshot> snapshot);
  bool Fail(Token token);

 private:
  friend class ::rex::graphics::d3d12::D3D12CommandProcessor;

  bool HasActiveRequest() const;
  void FailCurrent();
  void ResetLocked();

  mutable std::mutex mutex_;
  uint64_t next_token_ = 1;
  Token active_token_;
  DrawCaptureRequest request_;
  bool frame_request_ = false;
  bool claimed_ = false;
  std::shared_ptr<const DrawCaptureSnapshot> ready_;
  std::shared_ptr<const DrawCaptureFrameSnapshot> ready_frame_;
};

}  // namespace rex::graphics::diagnostic
