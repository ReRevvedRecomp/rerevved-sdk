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
#include <memory>
#include <mutex>
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
  bool IsPending(Token token) const;
  std::shared_ptr<const DrawCaptureSnapshot> TryTake(Token token);
  bool Cancel(Token token);

  // The producer retains the selected token even after cancellation, so an
  // armed live capture cannot become an ordinary disk capture or a newer request.
  Token GetRequestToken(const std::filesystem::path& output_path) const;
  // Producer operations. Claim must match the request exactly. Publish and
  // Fail are token-bound and silently reject stale or cancelled completions.
  Token Claim(Token expected_token, const std::filesystem::path& output_path,
              uint64_t vertex_shader_hash, uint64_t pixel_shader_hash);
  bool Publish(Token token, std::shared_ptr<const DrawCaptureSnapshot> snapshot);
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
  bool claimed_ = false;
  std::shared_ptr<const DrawCaptureSnapshot> ready_;
};

}  // namespace rex::graphics::diagnostic
