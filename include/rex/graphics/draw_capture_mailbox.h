/**
 ******************************************************************************
 * ReXGlue - Owned live draw capture handoff                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rex::graphics::d3d12 {
class D3D12CommandProcessor;
}

namespace rex::graphics::diagnostic {

inline constexpr uint32_t kDrawCaptureFrameSchemaVersion = 2;

enum class DrawCaptureExecutionStatus : uint32_t {
  kUnfinished = 0,
  kIssued = 1,
  kNoEffect = 2,
  kFailed = 3,
};

enum class DrawCaptureAvailability : uint32_t {
  kUnavailable = 0,
  kPartial = 1,
  kComplete = 2,
};

// Reasons describe the first known acquisition limitation. They are kept
// separate from execution because the original Xenos command still runs when
// an auxiliary native input cannot be copied.
enum class DrawCaptureReason : uint32_t {
  kNone = 0,
  kNoVertexShader,
  kRenderTargetPath,
  kResolutionScale,
  kMemexport,
  kIndexTranslation,
  kVertexFetch,
  kIndexSource,
  kReadbackBudget,
  kReadbackAllocation,
  kTextureUnresolved,
  kTextureDimension,
  kTextureFormat,
  kTextureSource,
  kReadback,
  kSubmission,
  kFence,
  kJournal,
  kSwap,
  kNoEffect,
  kExecutionFailure,
  kSignMaskUnused,
};

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
  DrawCaptureAvailability availability = DrawCaptureAvailability::kComplete;
  DrawCaptureReason unavailable_reason = DrawCaptureReason::kNone;
  // Repeated draws may own one immutable copy of the same point-of-use data.
  std::shared_ptr<const std::vector<uint8_t>> shared_bytes;

  std::span<const uint8_t> Payload() const {
    return shared_bytes ? std::span<const uint8_t>(*shared_bytes) : std::span<const uint8_t>(bytes);
  }
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
  DrawCaptureAvailability availability = DrawCaptureAvailability::kComplete;
  DrawCaptureReason unavailable_reason = DrawCaptureReason::kNone;
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
  // Shader translation may retain a signed or unsigned companion binding even
  // when the active fetch constant's sign mask cannot use that companion.
  bool required = true;
  DrawCaptureAvailability availability = DrawCaptureAvailability::kUnavailable;
  DrawCaptureReason unavailable_reason = DrawCaptureReason::kNone;
};

struct DrawCaptureTextureSubresource {
  uint32_t mip_level = 0;
  uint32_t array_slice = 0;
  uint32_t host_width = 0;
  uint32_t host_height = 0;
  uint32_t row_pitch = 0;
  uint32_t row_size = 0;
  uint32_t row_count = 0;
  // bytes is packed row_size * row_count data; row_pitch retains the source
  // footprint stride for consumers that need the D3D12 layout.
  std::vector<uint8_t> bytes;
};

// This is an owned host-resource payload. The top-level bytes and row layout
// describe mip 0, slice 0; subresources contains every captured mip and array/cube slice.
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

  uint32_t host_mip_levels = 1;
  uint64_t texture_instance_id = 0;
  uint64_t content_generation = 0;
  DrawCaptureAvailability availability = DrawCaptureAvailability::kComplete;
  DrawCaptureReason unavailable_reason = DrawCaptureReason::kNone;
  std::vector<DrawCaptureTextureSubresource> subresources;
};

// Frame draws use DrawCaptureSnapshot for guest and host draw data and add
// the point-of-use texture uses.
struct DrawCaptureFrameDraw {
  DrawCaptureSnapshot draw;
  // Original guest shader identity is retained even when host setup skips a
  // pixel shader or rewrites the bound shader for an early-return event.
  uint64_t guest_vertex_shader_hash = 0;
  uint64_t guest_pixel_shader_hash = 0;
  bool texture_bindings_complete = true;
  std::vector<DrawCaptureTextureUse> texture_uses;

  DrawCaptureAvailability acquisition = DrawCaptureAvailability::kComplete;
  DrawCaptureReason acquisition_reason = DrawCaptureReason::kNone;
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

  DrawCaptureExecutionStatus execution_status = DrawCaptureExecutionStatus::kUnfinished;
  DrawCaptureReason execution_reason = DrawCaptureReason::kNone;
  DrawCaptureAvailability acquisition = DrawCaptureAvailability::kComplete;
  DrawCaptureReason acquisition_reason = DrawCaptureReason::kNone;
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

  DrawCaptureAvailability source_availability = DrawCaptureAvailability::kUnavailable;
  DrawCaptureReason source_unavailable_reason = DrawCaptureReason::kNone;
  DrawCaptureAvailability output_availability = DrawCaptureAvailability::kUnavailable;
  DrawCaptureReason output_unavailable_reason = DrawCaptureReason::kNone;
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

  uint32_t schema_version = kDrawCaptureFrameSchemaVersion;
  DrawCaptureAvailability acquisition = DrawCaptureAvailability::kComplete;
  DrawCaptureReason acquisition_reason = DrawCaptureReason::kNone;
};

// Live input observer views are valid only during the synchronous callback
// and contain no host GPU resources or readback payloads.
struct DrawCaptureGuestMemoryReader {
  using CopyFunction = bool (*)(void* context, uint32_t physical_address,
                                std::span<uint8_t> destination) noexcept;

  void* context = nullptr;
  CopyFunction copy = nullptr;

  bool Copy(uint32_t physical_address, std::span<uint8_t> destination) const noexcept {
    return copy && copy(context, physical_address, destination);
  }
};

struct DrawCaptureFrameInputShaderView {
  bool present = false;
  uint64_t hash = 0;
  std::span<const uint32_t> words;
};

// A guest-memory watch version, not a host texture or GPU completion token.
// Only identity and generation may be retained. The context/check function is
// valid solely during the synchronous observer callback. Check before and after
// copying, and on reuse; a missing or invalid token requires reading the bytes.
struct DrawCaptureGuestMemoryVersion {
  using CheckFunction = bool (*)(const void* context, uint64_t generation) noexcept;
  uint64_t identity = 0;
  uint64_t generation = 0;
  uint32_t base_address = 0;
  uint32_t base_size = 0;
  uint32_t mip_address = 0;
  uint32_t mip_size = 0;
  const void* context = nullptr;
  CheckFunction check = nullptr;

  bool IsCurrent() const noexcept {
    return identity && generation && check && check(context, generation);
  }
};

struct DrawCaptureFrameInputVertexFetchView {
  uint32_t fetch_constant = 0;
  uint32_t physical_address = 0;
  uint32_t size = 0;
  DrawCaptureGuestMemoryReader memory;
  // Exact CPU-readable fetch range; unavailable for GPU-owned memory.
  DrawCaptureGuestMemoryVersion memory_version;
};

struct DrawCaptureFrameInputIndexView {
  bool present = false;
  uint32_t format = 0;
  uint32_t endianness = 0;
  uint32_t count = 0;
  uint32_t physical_address = 0;
  uint32_t size = 0;
  DrawCaptureGuestMemoryReader memory;
  // Covers the submitted index prefix, not unused DMA capacity.
  DrawCaptureGuestMemoryVersion memory_version;
};

struct DrawCaptureFrameInputTextureView {
  uint32_t fetch_constant = 0;
  std::span<const uint32_t> fetch_words;
  uint32_t format = 0;
  uint32_t endianness = 0;
  uint32_t dimension = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t pitch = 0;
  uint32_t mip_min_level = 0;
  uint32_t mip_max_level = 0;
  bool tiled = false;
  bool packed_mips = false;
  uint32_t base_address = 0;
  uint32_t base_size = 0;
  uint32_t mip_address = 0;
  uint32_t mip_size = 0;
  DrawCaptureGuestMemoryReader memory;
  // These are the CPU-side binding values used by the native replay
  // converter. They describe guest fetch interpretation and sampler state;
  // they do not identify or retain a host texture resource.
  uint32_t stage = 0;
  uint32_t shader_binding_index = 0;
  uint32_t sampler_value = 0;
  // D3D12 component mapping with the always-set validation bit, derived from
  // the active guest fetch's host format swizzle.
  uint32_t component_mapping = 0;
  // Packed post-fetch TextureSign values after the guest destination swizzle.
  uint8_t swizzled_signs = 0;
  bool is_signed = false;
  bool required = true;
  // False means the producer could identify this binding but cannot provide
  // the host-side descriptor or CPU source needed to replay it.
  bool available = true;
  DrawCaptureReason unavailable_reason = DrawCaptureReason::kNone;
  // Current shared-memory ownership, including previous-frame GPU writes.
  // A CPU rewrite clears the page ownership even if the address was resolved
  // earlier. This does not supply the GPU data itself.
  bool gpu_written = false;
  // Covers exactly the named ranges while IsCurrent succeeds. It does not
  // replace the current CPU/GPU ownership check above or promise an atomic frame.
  DrawCaptureGuestMemoryVersion memory_version;
  // Copy event ID for an admitted native source, or zero. A paired destination
  // uses its right-half copy ID and the latest current matching left with a
  // smaller ID. Retain both events even across draws or unrelated resolves.
  // This identity is scoped to the observer stream and is not GPU completion.
  uint64_t native_source_generation = 0;
  // Left-half copy ID of a paired destination, or zero for CPU, rejected and
  // standalone views. It is always smaller than native_source_generation.
  uint64_t native_source_left_generation = 0;
};

struct DrawCaptureFrameInputDrawView {
  uint64_t id = 0;
  uint64_t frame = 0;
  uint64_t submission = 0;
  std::span<const uint32_t> registers;
  DrawCaptureFrameInputShaderView vertex_shader;
  DrawCaptureFrameInputShaderView pixel_shader;
  uint32_t primitive_type = 0;
  uint32_t requested_index_count = 0;
  bool major_mode_explicit = false;
  bool indexed = false;
  DrawCaptureFrameInputIndexView index;
  std::span<const DrawCaptureFrameInputVertexFetchView> vertex_fetches;
  std::span<const DrawCaptureFrameInputTextureView> textures;
  bool original_host_issued = false;
  bool cpu_data_complete = true;
  DrawCaptureReason unavailable_reason = DrawCaptureReason::kNone;
  DrawCaptureExecutionStatus execution_status = DrawCaptureExecutionStatus::kUnfinished;
  DrawCaptureReason execution_reason = DrawCaptureReason::kNone;
};

struct DrawCaptureFrameInputCopyView {
  uint64_t id = 0;
  uint64_t frame = 0;
  uint64_t submission = 0;
  std::span<const uint32_t> registers;
  std::string_view copy_mode = "resolve";
  DrawCaptureFrameInputVertexFetchView source_geometry;
  // When present, this is the resolved EDRAM source rectangle in pixels. The
  // values are owned by this call and remain valid for the callback only.
  uint32_t source_x = 0;
  uint32_t source_y = 0;
  uint32_t source_width = 0;
  uint32_t source_height = 0;
  bool source_rect_present = false;
  // The original resolve result and the exact guest range it wrote. A
  // successful copy with zero length is a valid clear-only result.
  bool original_succeeded = false;
  uint32_t written_address = 0;
  uint32_t written_length = 0;
  // CPU admission and prospective output only. These fields never establish a
  // guest write or GPU completion; prepared callbacks retain zero written_*.
  bool cpu_data_complete = false;
  DrawCaptureReason unavailable_reason = DrawCaptureReason::kNone;
  uint32_t planned_address = 0;
  uint32_t planned_length = 0;
  // Origin of the resolved rectangle within its destination tile (32 pixels at
  // 32bpp and wider, 64 at 16bpp, 128 at 8bpp), in pixels. planned_address is
  // the tile lower bound and source_x/y are only sub-tile EDRAM offsets, so
  // neither shows where the texels land. Valid with source_rect_present; zero
  // and meaningless for formats that cannot be resolved.
  uint32_t copy_dest_offset_x = 0;
  uint32_t copy_dest_offset_y = 0;
};

struct DrawCaptureFrameInputSwapView {
  // Paired prepared/original views share this ordered observer-local ID.
  uint64_t id = 0;
  // Backend observations may repeat after original failure.
  uint64_t frame = 0;
  uint64_t submission = 0;
  uint64_t frontbuffer_ptr = 0;
  uint32_t frontbuffer_width = 0;
  uint32_t frontbuffer_height = 0;
  std::span<const uint32_t> registers;
  bool use_pwl_gamma_ramp = false;
  bool use_fxaa = false;
  bool is_8bpc = false;
  std::span<const uint32_t> gamma_256;
  std::span<const uint32_t> gamma_pwl_rgb;
  bool original_succeeded = false;
};

class DrawCaptureFrameInputObserver {
 public:
  virtual ~DrawCaptureFrameInputObserver() = default;

  // The first selected swap boundary opens the stream, at decode for prepared
  // opt-in or at original swap otherwise. Setup is bounded and synchronous.
  virtual bool OnFrameBegin(const DrawCaptureFrameInputSwapView& boundary) = 0;
  virtual bool OnDraw(const DrawCaptureFrameInputDrawView& draw) = 0;
  virtual bool OnCopy(const DrawCaptureFrameInputCopyView& copy) = 0;
  virtual bool OnSwap(const DrawCaptureFrameInputSwapView& swap) = 0;
  virtual bool OnFrameEnd(uint64_t frame, uint64_t submission, bool original_succeeded) = 0;

  // Opt in before registration. Prepared views borrow proven CPU inputs before
  // original submission setup, with original execution still unfinished.
  // Texture-dependent draws are incomplete until their CPU metadata is proved.
  virtual bool WantsPreparedDraws() const noexcept { return false; }
  virtual bool OnDrawPrepared(const DrawCaptureFrameInputDrawView&) { return false; }
  virtual bool WantsPreparedCopies() const noexcept { return false; }
  virtual bool OnCopyPrepared(const DrawCaptureFrameInputCopyView&) { return false; }
  // Frozen at registration. The decoded boundary opens successor preparation;
  // original End(false) is an observation, not closure of this prepared stream.
  virtual bool WantsPreparedSwaps() const noexcept { return false; }
  virtual bool OnSwapPrepared(const DrawCaptureFrameInputSwapView&) { return false; }
};

class DrawCaptureMailbox {
 public:
  struct Token {
    uint64_t value = 0;

    explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(Token, Token) = default;
  };

  struct ObserverToken {
    uint64_t value = 0;

    explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(ObserverToken, ObserverToken) = default;
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

  // Core-owned live input observer. Registration is single-consumer and
  // quiescent: UnregisterFrameInputObserver waits for an in-flight callback
  // before returning. Producers must treat callback results as advisory; the
  // guest command's result and ring progress are never changed by a callback.
  // Dispatch is single-producer and nonreentrant. A concurrent or recursive
  // dispatch is rejected without entering the observer a second time.
  ObserverToken RegisterFrameInputObserver(std::shared_ptr<DrawCaptureFrameInputObserver> observer);
  bool UnregisterFrameInputObserver(ObserverToken token);
  bool HasFrameInputObserver() const noexcept;
  ObserverToken GetFrameInputObserverToken() const noexcept;
  bool HasFrameInputPreparedDrawObserver(ObserverToken token) const noexcept;
  bool HasFrameInputPreparedCopyObserver(ObserverToken token) const noexcept;
  bool HasFrameInputPreparedSwapObserver(ObserverToken token) const noexcept;
  bool DispatchFrameInputBegin(ObserverToken token, const DrawCaptureFrameInputSwapView& boundary);
  bool DispatchFrameInputDraw(ObserverToken token, const DrawCaptureFrameInputDrawView& draw);
  // This result is advisory and never authorizes suppression of original GPU
  // work. A later OnDraw for the same observer retains the ID and actual result.
  bool DispatchFrameInputPreparedDraw(ObserverToken token,
                                      const DrawCaptureFrameInputDrawView& draw);
  bool DispatchFrameInputCopy(ObserverToken token, const DrawCaptureFrameInputCopyView& copy);
  // Frozen opt-in and advisory result follow prepared draws. The later OnCopy
  // retains the event ID and reports actual original results, including failure
  // after a nonzero partial write.
  bool DispatchFrameInputPreparedCopy(ObserverToken token,
                                      const DrawCaptureFrameInputCopyView& copy);
  bool DispatchFrameInputSwap(ObserverToken token, const DrawCaptureFrameInputSwapView& swap);
  // Borrows final CPU state before original GPU setup, with false original
  // provenance. The later OnSwap retains its ID and OnFrameEnd its actual result.
  bool DispatchFrameInputPreparedSwap(ObserverToken token,
                                      const DrawCaptureFrameInputSwapView& swap);
  bool DispatchFrameInputEnd(ObserverToken token, uint64_t frame, uint64_t submission,
                             bool original_succeeded);

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

  struct ObserverState;
  template <typename Callback>
  bool DispatchFrameInputObserver(ObserverToken token, Callback&& callback);
  std::shared_ptr<ObserverState> observer_state_;
  std::atomic<uint64_t> observer_fast_token_{0};
  std::atomic<uint64_t> observer_prepared_draw_token_{0};
  std::atomic<uint64_t> observer_prepared_copy_token_{0};
  std::atomic<uint64_t> observer_prepared_swap_token_{0};
  uint64_t next_observer_token_ = 1;
  mutable std::mutex observer_mutex_;
};

}  // namespace rex::graphics::diagnostic
