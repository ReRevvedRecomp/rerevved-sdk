#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/crypto/sha256.h>
#include <rex/cvar.h>
#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/xenos_fence_trace.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform/dynlib.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>

#include "rg16_control.h"

namespace {

using rex::graphics::diagnostic::DrawCaptureExecutionStatus;
using rex::graphics::diagnostic::DrawCaptureFrameInputCopyView;
using rex::graphics::diagnostic::DrawCaptureFrameInputDrawView;
using rex::graphics::diagnostic::DrawCaptureFrameInputObserver;
using rex::graphics::diagnostic::DrawCaptureFrameInputSwapView;
using rex::graphics::diagnostic::DrawCaptureReason;

constexpr uint32_t kMmioBaseAddress = 0x7FC80000U;
constexpr uint32_t kCpRbWptr = 0x01C5U;
constexpr uint32_t kWptrAddress = kMmioBaseAddress + kCpRbWptr * 4U;

constexpr uint32_t kRingSizeLog2 = 12U;
constexpr uint32_t kRingWordCount = 1U << (kRingSizeLog2 + 1U);
constexpr uint32_t kWritebackBlockSizeLog2 = 2U;
constexpr uint32_t kSurfaceWidth = 1280U;
constexpr uint32_t kSurfaceHeight = 720U;
constexpr uint32_t kSurfaceLinearBytes = kSurfaceWidth * kSurfaceHeight * 4U;
// Keep the ordinary CPU-owned sources at a canonical single-tile size. The
// signed producer fetches remain full-frame 1280x720 attachments.
constexpr uint32_t kConsumerWidth = 32U;
constexpr uint32_t kConsumerHeight = 32U;

// Keep this subset small enough for the control output to remain reviewable,
// while covering the state that distinguishes the synthetic clear from the
// qualified consumer draw. The full register image is summarized separately.
constexpr std::array<uint32_t, 29> kDiagnosticRegisterIndices = {
    0x2000U,
    0x2001U,
    0x2002U,
    0x200FU,
    0x2080U,
    0x2081U,
    0x2082U,
    0x2100U,
    0x2104U,
    0x210DU,
    0x2180U,
    0x2181U,
    0x2182U,
    0x2200U,
    0x2201U,
    0x2202U,
    0x2204U,
    0x2205U,
    0x2206U,
    0x2208U,
    0x2302U,
    0x2307U,
    0x2308U,
    rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
    rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1U,
    rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 6U,
    rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 7U,
    rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U,
    rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U + 1U};

uint32_t RegisterValue(std::span<const uint32_t> registers, uint32_t index) noexcept {
  return index < registers.size() ? registers[index] : 0U;
}

uint64_t RegisterDigest(std::span<const uint32_t> registers) noexcept {
  uint64_t hash = 1469598103934665603ULL;
  for (uint32_t word : registers) {
    for (uint32_t shift = 0U; shift < 32U; shift += 8U) {
      hash ^= static_cast<uint8_t>(word >> shift);
      hash *= 1099511628211ULL;
    }
  }
  return hash;
}

std::array<uint32_t, kDiagnosticRegisterIndices.size()> CaptureRegisterSubset(
    std::span<const uint32_t> registers) noexcept {
  std::array<uint32_t, kDiagnosticRegisterIndices.size()> result{};
  for (std::size_t i = 0; i < kDiagnosticRegisterIndices.size(); ++i) {
    result[i] = RegisterValue(registers, kDiagnosticRegisterIndices[i]);
  }
  return result;
}

struct DrawRecord {
  uint64_t id = 0;
  uint64_t frame = 0;
  uint64_t submission = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  std::string vertex_word_digest;
  std::string pixel_word_digest;
  bool original_host_issued = false;
  bool cpu_data_complete = false;
  DrawCaptureExecutionStatus execution_status = DrawCaptureExecutionStatus::kUnfinished;
  DrawCaptureReason execution_reason = DrawCaptureReason::kNone;
  uint32_t primitive_type = 0;
  uint32_t requested_index_count = 0;
  std::size_t register_word_count = 0;
  uint64_t register_digest = 0;
  std::array<uint32_t, kDiagnosticRegisterIndices.size()> register_subset{};
  std::vector<uint32_t> registers;
  std::vector<uint32_t> vertex_shader_words;
  std::vector<uint32_t> pixel_shader_words;
};

struct CopyRecord {
  uint64_t id = 0;
  uint64_t frame = 0;
  uint64_t submission = 0;
  std::string mode;
  bool original_succeeded = false;
  uint32_t written_address = 0;
  uint32_t written_length = 0;
  uint32_t planned_address = 0;
  uint32_t planned_length = 0;
  bool cpu_data_complete = false;
  bool prepared = false;
  std::vector<uint32_t> registers;
};

struct SwapRecord {
  uint64_t id = 0;
  uint64_t frame = 0;
  uint64_t submission = 0;
  uint64_t frontbuffer_ptr = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool original_succeeded = false;
};

std::string ShaderWordDigest(std::span<const uint32_t> words) {
  return rex::crypto::sha256(
      std::string_view(reinterpret_cast<const char*>(words.data()), words.size_bytes()));
}

class OriginalObservation final : public DrawCaptureFrameInputObserver {
 public:
  explicit OriginalObservation(std::shared_ptr<DrawCaptureFrameInputObserver> forwarded = nullptr)
      : forwarded_(std::move(forwarded)) {}

  struct BeginRecord {
    uint64_t id = 0;
    uint64_t frame = 0;
    uint64_t submission = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool original_succeeded = false;
  };

  struct Snapshot {
    std::size_t begin_count = 0;
    std::size_t end_count = 0;
    BeginRecord last_begin;
    std::vector<DrawRecord> draws;
    std::vector<DrawRecord> prepared_draws;
    std::vector<CopyRecord> copies;
    std::vector<CopyRecord> prepared_copies;
    std::vector<SwapRecord> swaps;
    uint64_t last_end_frame = 0;
    uint64_t last_end_submission = 0;
    bool last_end_succeeded = false;
  };

  bool OnFrameBegin(const DrawCaptureFrameInputSwapView& boundary) override {
    {
      std::lock_guard lock(mutex_);
      ++begin_count_;
      last_begin_ = {boundary.id,
                     boundary.frame,
                     boundary.submission,
                     boundary.frontbuffer_width,
                     boundary.frontbuffer_height,
                     boundary.original_succeeded};
    }
    condition_.notify_all();
    return !forwarded_ || forwarded_->OnFrameBegin(boundary);
  }

  bool OnDraw(const DrawCaptureFrameInputDrawView& draw) override {
    {
      std::lock_guard lock(mutex_);
      draws_.push_back(MakeDrawRecord(draw));
    }
    condition_.notify_all();
    return !forwarded_ || forwarded_->OnDraw(draw);
  }

  bool OnCopy(const DrawCaptureFrameInputCopyView& copy) override {
    {
      std::lock_guard lock(mutex_);
      copies_.push_back(MakeCopyRecord(copy, false));
    }
    condition_.notify_all();
    return !forwarded_ || forwarded_->OnCopy(copy);
  }

  bool WantsPreparedDraws() const noexcept override { return true; }

  bool OnDrawPrepared(const DrawCaptureFrameInputDrawView& draw) override {
    {
      std::lock_guard lock(mutex_);
      prepared_draws_.push_back(MakeDrawRecord(draw));
    }
    condition_.notify_all();
    return !forwarded_ || !forwarded_->WantsPreparedDraws() || forwarded_->OnDrawPrepared(draw);
  }

  bool WantsPreparedCopies() const noexcept override { return true; }

  bool OnCopyPrepared(const DrawCaptureFrameInputCopyView& copy) override {
    {
      std::lock_guard lock(mutex_);
      prepared_copies_.push_back(MakeCopyRecord(copy, true));
    }
    condition_.notify_all();
    return !forwarded_ || !forwarded_->WantsPreparedCopies() || forwarded_->OnCopyPrepared(copy);
  }

  bool OnSwap(const DrawCaptureFrameInputSwapView& swap) override {
    {
      std::lock_guard lock(mutex_);
      swaps_.push_back({swap.id, swap.frame, swap.submission, swap.frontbuffer_ptr,
                        swap.frontbuffer_width, swap.frontbuffer_height, swap.original_succeeded});
    }
    condition_.notify_all();
    return !forwarded_ || forwarded_->OnSwap(swap);
  }

  bool OnFrameEnd(uint64_t frame, uint64_t submission, bool original_succeeded) override {
    {
      std::lock_guard lock(mutex_);
      ++end_count_;
      last_end_frame_ = frame;
      last_end_submission_ = submission;
      last_end_succeeded_ = original_succeeded;
    }
    condition_.notify_all();
    return !forwarded_ || forwarded_->OnFrameEnd(frame, submission, original_succeeded);
  }

  bool WantsPreparedSwaps() const noexcept override {
    return forwarded_ && forwarded_->WantsPreparedSwaps();
  }

  bool OnSwapPrepared(const DrawCaptureFrameInputSwapView& swap) override {
    return !forwarded_ || !forwarded_->WantsPreparedSwaps() || forwarded_->OnSwapPrepared(swap);
  }

  bool WaitForCompletion(uint32_t terminal_address) {
    (void)terminal_address;
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(15), [&] {
      const bool terminal_copy =
          std::any_of(copies_.begin(), copies_.end(), [&](const CopyRecord& copy) {
            return copy.original_succeeded &&
                   (copy.mode == "resolve" || copy.mode == "resolve_readback");
          });
      const bool terminal_swap = std::any_of(
          swaps_.begin(), swaps_.end(),
          [&](const SwapRecord& swap) { return swap.frontbuffer_ptr == terminal_address; });
      return terminal_copy && terminal_swap && end_count_ >= 1 && last_end_succeeded_;
    });
  }

  Snapshot TakeSnapshot() const {
    std::lock_guard lock(mutex_);
    Snapshot result;
    result.begin_count = begin_count_;
    result.end_count = end_count_;
    result.last_begin = last_begin_;
    result.draws = draws_;
    result.prepared_draws = prepared_draws_;
    result.copies = copies_;
    result.prepared_copies = prepared_copies_;
    result.swaps = swaps_;
    result.last_end_frame = last_end_frame_;
    result.last_end_submission = last_end_submission_;
    result.last_end_succeeded = last_end_succeeded_;
    return result;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t begin_count_ = 0;
  std::size_t end_count_ = 0;
  BeginRecord last_begin_;
  std::vector<DrawRecord> draws_;
  std::vector<DrawRecord> prepared_draws_;
  std::vector<CopyRecord> copies_;
  std::vector<CopyRecord> prepared_copies_;
  std::vector<SwapRecord> swaps_;
  uint64_t last_end_frame_ = 0;
  uint64_t last_end_submission_ = 0;
  bool last_end_succeeded_ = false;
  std::shared_ptr<DrawCaptureFrameInputObserver> forwarded_;

  static DrawRecord MakeDrawRecord(const DrawCaptureFrameInputDrawView& draw) {
    DrawRecord result;
    result.id = draw.id;
    result.frame = draw.frame;
    result.submission = draw.submission;
    result.vertex_shader_hash = draw.vertex_shader.hash;
    result.pixel_shader_hash = draw.pixel_shader.hash;
    result.vertex_word_digest = ShaderWordDigest(draw.vertex_shader.words);
    result.pixel_word_digest = ShaderWordDigest(draw.pixel_shader.words);
    result.original_host_issued = draw.original_host_issued;
    result.cpu_data_complete = draw.cpu_data_complete;
    result.execution_status = draw.execution_status;
    result.execution_reason = draw.execution_reason;
    result.primitive_type = draw.primitive_type;
    result.requested_index_count = draw.requested_index_count;
    result.register_word_count = draw.registers.size();
    result.register_digest = RegisterDigest(draw.registers);
    result.register_subset = CaptureRegisterSubset(draw.registers);
    result.registers.assign(draw.registers.begin(), draw.registers.end());
    result.vertex_shader_words.assign(draw.vertex_shader.words.begin(),
                                      draw.vertex_shader.words.end());
    result.pixel_shader_words.assign(draw.pixel_shader.words.begin(),
                                     draw.pixel_shader.words.end());
    return result;
  }

  static CopyRecord MakeCopyRecord(const DrawCaptureFrameInputCopyView& copy, bool prepared) {
    CopyRecord result{copy.id,
                      copy.frame,
                      copy.submission,
                      std::string(copy.copy_mode),
                      copy.original_succeeded,
                      copy.written_address,
                      copy.written_length,
                      copy.planned_address,
                      copy.planned_length,
                      copy.cpu_data_complete,
                      prepared};
    result.registers.assign(copy.registers.begin(), copy.registers.end());
    return result;
  }
};

class GuestAllocation final {
 public:
  GuestAllocation() = default;
  GuestAllocation(rex::memory::Memory& memory, uint32_t bytes, uint32_t alignment)
      : memory_(&memory),
        virtual_address_(
            memory.SystemHeapAlloc(bytes, alignment, rex::memory::kSystemHeapPhysical)) {
    if (virtual_address_) {
      physical_address_ = memory.GetPhysicalAddress(virtual_address_);
    }
  }

  GuestAllocation(const GuestAllocation&) = delete;
  GuestAllocation& operator=(const GuestAllocation&) = delete;

  GuestAllocation(GuestAllocation&& other) noexcept
      : memory_(std::exchange(other.memory_, nullptr)),
        virtual_address_(std::exchange(other.virtual_address_, 0)),
        physical_address_(std::exchange(other.physical_address_, 0)) {}

  GuestAllocation& operator=(GuestAllocation&& other) noexcept {
    if (this != &other) {
      Reset();
      memory_ = std::exchange(other.memory_, nullptr);
      virtual_address_ = std::exchange(other.virtual_address_, 0);
      physical_address_ = std::exchange(other.physical_address_, 0);
    }
    return *this;
  }

  ~GuestAllocation() { Reset(); }

  explicit operator bool() const noexcept {
    return memory_ != nullptr && virtual_address_ != 0 && physical_address_ != 0;
  }
  uint32_t virtual_address() const noexcept { return virtual_address_; }
  uint32_t physical_address() const noexcept { return physical_address_; }
  uint8_t* physical_bytes() const noexcept {
    return memory_ ? memory_->TranslatePhysical<uint8_t*>(physical_address_) : nullptr;
  }

  void Reset() noexcept {
    if (memory_ && virtual_address_) {
      memory_->SystemHeapFree(virtual_address_);
    }
    memory_ = nullptr;
    virtual_address_ = 0;
    physical_address_ = 0;
  }

 private:
  rex::memory::Memory* memory_ = nullptr;
  uint32_t virtual_address_ = 0;
  uint32_t physical_address_ = 0;
};

void WriteShaderWords(GuestAllocation& allocation, std::span<const uint32_t> words) {
  auto* bytes = allocation.physical_bytes();
  for (std::size_t index = 0; index < words.size(); ++index) {
    rex::memory::store_and_swap<uint32_t>(bytes + index * sizeof(uint32_t), words[index]);
  }
}

class PacketStream final {
 public:
  void Push(uint32_t value) { words_.push_back(value); }

  void Type0(uint32_t index, std::span<const uint32_t> values) {
    if (values.empty()) {
      return;
    }
    Push(rex::graphics::xenos::MakePacketType0(static_cast<uint16_t>(index),
                                               static_cast<uint16_t>(values.size())));
    words_.insert(words_.end(), values.begin(), values.end());
  }

  void Type0(uint32_t index, std::initializer_list<uint32_t> values) {
    Type0(index, std::span<const uint32_t>(values.begin(), values.size()));
  }

  void LoadShader(uint32_t physical_address, rex::graphics::xenos::ShaderType type,
                  uint32_t dword_count) {
    Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_IM_LOAD, 2));
    Push(physical_address | static_cast<uint32_t>(type));
    Push(dword_count);
  }

  void DrawAuto(uint32_t initiator) {
    Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_DRAW_INDX, 2));
    Push(0);
    Push(initiator);
  }

  void DrawDma(uint32_t initiator, uint32_t index_address, uint32_t index_word_count,
               rex::graphics::xenos::Endian index_endian) {
    Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_DRAW_INDX, 4));
    Push(0);
    Push(initiator);
    Push(index_address);
    Push(index_word_count | (static_cast<uint32_t>(index_endian) << 30));
  }

  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height) {
    Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_XE_SWAP, 4));
    Push(rex::graphics::xenos::kSwapSignature);
    Push(frontbuffer);
    Push(width);
    Push(height);
  }

  std::span<const uint32_t> words() const noexcept { return words_; }

 private:
  std::vector<uint32_t> words_;
};

void AppendBigEndianFloat(std::vector<uint8_t>& bytes, float value) {
  uint32_t word = 0;
  static_assert(sizeof(word) == sizeof(value));
  std::memcpy(&word, &value, sizeof(word));
  bytes.push_back(static_cast<uint8_t>(word >> 24));
  bytes.push_back(static_cast<uint8_t>(word >> 16));
  bytes.push_back(static_cast<uint8_t>(word >> 8));
  bytes.push_back(static_cast<uint8_t>(word));
}

std::vector<uint8_t> ClearGeometry(float producer_alpha) {
  std::vector<uint8_t> bytes;
  for (const auto value : std::array<std::array<float, 7>, 3>{
           {{-0.5F, -0.5F, 0.25F, 0.2F, 0.4F, 0.6F, producer_alpha},
            {639.5F, -0.5F, 0.25F, 0.2F, 0.4F, 0.6F, producer_alpha},
            {639.5F, 719.5F, 0.25F, 0.2F, 0.4F, 0.6F, producer_alpha}}}) {
    for (float component : value) {
      AppendBigEndianFloat(bytes, component);
    }
  }
  return bytes;
}

std::vector<uint8_t> ResolveGeometry() {
  std::vector<uint8_t> bytes;
  for (float value : std::array<float, 6>{-0.5F, -0.5F, 639.5F, -0.5F, 639.5F, 719.5F}) {
    AppendBigEndianFloat(bytes, value);
  }
  return bytes;
}

std::vector<uint8_t> RightResolveGeometry() {
  std::vector<uint8_t> bytes;
  for (float value : std::array<float, 6>{639.5F, -0.5F, 1279.5F, -0.5F, 1279.5F, 719.5F}) {
    AppendBigEndianFloat(bytes, value);
  }
  return bytes;
}

std::vector<uint8_t> FullResolveGeometry() {
  std::vector<uint8_t> bytes;
  for (float value : std::array<float, 6>{-0.5F, -0.5F, 1279.5F, -0.5F, 1279.5F, 719.5F}) {
    AppendBigEndianFloat(bytes, value);
  }
  return bytes;
}

std::vector<uint8_t> ConsumerGeometry() {
  std::vector<uint8_t> bytes;
  // The qualified specialization-0048 vertex shader uses vfetch95 with a
  // 56-byte stride: position at offset 0 and texture coordinates at offset
  // 16. Padding in the guest layout is unused.
  for (const auto value : std::array<std::array<float, 14>, 3>{
           {{-1.0F, -1.0F, 0.75F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
            {-1.0F, 3.0F, 0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
            {3.0F, -1.0F, 0.75F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
             0.0F}}}) {
    for (const auto component : value) {
      AppendBigEndianFloat(bytes, component);
    }
  }
  return bytes;
}

std::vector<uint8_t> RequiredConsumerGeometry() {
  std::vector<uint8_t> bytes;
  // The captured consumer VS uses vfetch95 with a 32-byte stride, position at
  // offset 0 and UV at offset 24. Two triangles cover the complete 1280x720
  // target while preserving all four interpolated texture coordinates.
  for (const auto value :
       std::array<std::array<float, 8>, 6>{{{-1.0F, -1.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
                                            {-1.0F, 1.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F},
                                            {1.0F, -1.0F, 0.5F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F},
                                            {1.0F, -1.0F, 0.5F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F},
                                            {-1.0F, 1.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F},
                                            {1.0F, 1.0F, 0.5F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F}}}) {
    for (float component : value) {
      AppendBigEndianFloat(bytes, component);
    }
  }
  return bytes;
}

std::array<uint32_t, 6> VertexFetch(uint32_t physical_address, uint32_t bytes) {
  rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch{};
  fetch.type = rex::graphics::xenos::FetchConstantType::kVertex;
  fetch.address = physical_address / sizeof(uint32_t);
  fetch.endian = rex::graphics::xenos::Endian::k8in32;
  fetch.size = bytes / sizeof(uint32_t);
  return {fetch.dword_0, fetch.dword_1, 0, 0, 0, 0};
}

std::array<uint32_t, 6> TextureFetch(uint32_t physical_address, uint32_t width, uint32_t height,
                                     uint32_t pitch) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = rex::graphics::xenos::FetchConstantType::kTexture;
  fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w =
      rex::graphics::xenos::TextureSign::kUnsigned;
  fetch.pitch = pitch / 32U;
  fetch.format = rex::graphics::xenos::TextureFormat::k_8_8_8_8;
  fetch.endianness = rex::graphics::xenos::Endian::kNone;
  fetch.base_address = physical_address >> 12;
  fetch.size_2d.width = width - 1U;
  fetch.size_2d.height = height - 1U;
  fetch.tiled = true;
  fetch.swizzle = rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  fetch.mag_filter = rex::graphics::xenos::TextureFilter::kLinear;
  fetch.min_filter = rex::graphics::xenos::TextureFilter::kLinear;
  fetch.mip_filter = rex::graphics::xenos::TextureFilter::kPoint;
  fetch.dimension = rex::graphics::xenos::DataDimension::k2DOrStacked;
  return {fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5};
}

std::array<uint32_t, 6> SignedRg16TextureFetch(uint32_t physical_address) {
  // These are the captured signed RG16 words. Only the guest page word is
  // substituted for an allocation alias; all format/sign/filter bits stay
  // byte-identical to the selected fetch (0x55 signs, mapping 0x1B48).
  return {0x8A024956U, (physical_address & 0xFFFFF000U) | 0x9FU, 0x0059E4FFU, 0x00001691U, 0U,
          0x00000218U};
}

void FillRg16FloatPattern(uint8_t* bytes, uint32_t byte_count, uint16_t red, uint16_t green) {
  for (uint32_t offset = 0; offset + 3U < byte_count; offset += 4U) {
    bytes[offset + 0U] = static_cast<uint8_t>(red);
    bytes[offset + 1U] = static_cast<uint8_t>(red >> 8U);
    bytes[offset + 2U] = static_cast<uint8_t>(green);
    bytes[offset + 3U] = static_cast<uint8_t>(green >> 8U);
  }
}
uint32_t GuestTextureExtent(uint32_t width, uint32_t height, uint32_t pitch) {
  const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
      rex::graphics::xenos::DataDimension::k2DOrStacked, pitch / 32U, width, height, 1U, true,
      rex::graphics::xenos::TextureFormat::k_8_8_8_8, false, true, 0U);
  return layout.base.level_data_extent_bytes;
}

uint32_t SignedRg16GuestTextureExtent() {
  const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
      rex::graphics::xenos::DataDimension::k2DOrStacked, 40U, kSurfaceWidth, kSurfaceHeight, 1U,
      true, rex::graphics::xenos::TextureFormat::k_16_16_FLOAT, false, true, 0U);
  return layout.base.level_data_extent_bytes;
}

uint32_t AlignToPage(uint32_t bytes) {
  if (bytes == 0U || bytes > std::numeric_limits<uint32_t>::max() - 0xFFFU) {
    return 0U;
  }
  return (bytes + 0xFFFU) & ~0xFFFU;
}

bool DecodeRgba8Texture(std::span<const uint32_t> fetch_words, std::span<const uint8_t> tiled,
                        uint32_t width, uint32_t height, std::vector<uint8_t>& linear) {
  if (fetch_words.size() != 6U || width == 0U || height == 0U) {
    return false;
  }
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, fetch_words.data(), sizeof(fetch));
  rex::graphics::TextureInfo info{};
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info) ||
      info.format != rex::graphics::xenos::TextureFormat::k_8_8_8_8 ||
      info.dimension != rex::graphics::xenos::DataDimension::k2DOrStacked || !info.is_tiled ||
      info.has_packed_mips || info.mip_min_level != 0U || info.mip_max_level != 0U ||
      info.width + 1U != width || info.height + 1U != height || info.depth != 0U) {
    return false;
  }
  const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
      info.dimension, fetch.pitch, width, height, 1U, info.is_tiled, info.format,
      info.has_packed_mips, true, info.mip_max_level);
  if (layout.base.level_data_extent_bytes == 0U ||
      layout.base.level_data_extent_bytes > tiled.size()) {
    return false;
  }
  const auto* format_info = info.format_info();
  if (!format_info || format_info->block_width != 1U || format_info->block_height != 1U ||
      format_info->bytes_per_block() != 4U) {
    return false;
  }
  const uint64_t linear_bytes = static_cast<uint64_t>(width) * height * 4U;
  if (linear_bytes > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  linear.assign(static_cast<std::size_t>(linear_bytes), 0U);
  rex::graphics::texture_conversion::UntileInfo untile{};
  untile.offset_x = 0U;
  untile.offset_y = 0U;
  untile.width = width;
  untile.height = height;
  untile.input_pitch = layout.base.row_pitch_bytes / format_info->bytes_per_block();
  untile.output_pitch = width;
  untile.input_format_info = format_info;
  untile.output_format_info = format_info;
  untile.copy_callback = [endian = fetch.endianness](void* output, const void* input,
                                                     size_t length) {
    rex::graphics::texture_conversion::CopySwapBlock(endian, output, input, length);
  };
  rex::graphics::texture_conversion::Untile(linear.data(), tiled.data(), &untile);
  return true;
}

bool DecodeRg16FloatTexture(std::span<const uint32_t> fetch_words, std::span<const uint8_t> tiled,
                            std::uint32_t width, std::uint32_t height,
                            std::vector<std::uint8_t>& linear) {
  if (fetch_words.size() != 6U || width == 0U || height == 0U) {
    return false;
  }
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, fetch_words.data(), sizeof(fetch));
  rex::graphics::TextureInfo info{};
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info) ||
      info.format != rex::graphics::xenos::TextureFormat::k_16_16_FLOAT ||
      info.dimension != rex::graphics::xenos::DataDimension::k2DOrStacked || !info.is_tiled ||
      info.has_packed_mips || info.mip_min_level != 0U || info.mip_max_level != 0U ||
      info.width + 1U != width || info.height + 1U != height || info.depth != 0U ||
      ((fetch.dword_0 >> 2U) & 0xFFU) != 0x55U) {
    return false;
  }
  const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
      info.dimension, fetch.pitch, width, height, 1U, info.is_tiled, info.format,
      info.has_packed_mips, true, info.mip_max_level);
  const auto* format_info = info.format_info();
  if (layout.base.level_data_extent_bytes == 0U ||
      layout.base.level_data_extent_bytes > tiled.size() || !format_info ||
      format_info->block_width != 1U || format_info->block_height != 1U ||
      format_info->bytes_per_block() != 4U) {
    return false;
  }
  const std::uint64_t linear_bytes = static_cast<std::uint64_t>(width) * height * 4U;
  if (linear_bytes > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  linear.assign(static_cast<std::size_t>(linear_bytes), 0U);
  rex::graphics::texture_conversion::UntileInfo untile{};
  untile.offset_x = 0U;
  untile.offset_y = 0U;
  untile.width = width;
  untile.height = height;
  untile.input_pitch = layout.base.row_pitch_bytes / format_info->bytes_per_block();
  untile.output_pitch = width;
  untile.input_format_info = format_info;
  untile.output_format_info = format_info;
  untile.copy_callback = [endian = fetch.endianness](void* output, const void* input,
                                                     size_t length) {
    rex::graphics::texture_conversion::CopySwapBlock(endian, output, input, length);
  };
  rex::graphics::texture_conversion::Untile(linear.data(), tiled.data(), &untile);
  return true;
}
bool HasMeaningfulRgba8Pixels(std::span<const uint8_t> pixels) {
  if (pixels.empty() || pixels.size() % 4U != 0U) {
    return false;
  }
  for (std::size_t index = 0; index < pixels.size(); index += 4U) {
    if (pixels[index] != 0U || pixels[index + 1U] != 0U || pixels[index + 2U] != 0U ||
        pixels[index + 3U] != 0U) {
      return true;
    }
  }
  return false;
}

std::size_t NonzeroRgbPixelCount(std::span<const uint8_t> pixels) {
  if (pixels.size() % 4U != 0U) {
    return 0U;
  }
  std::size_t count = 0U;
  for (std::size_t index = 0; index < pixels.size(); index += 4U) {
    if (pixels[index] != 0U || pixels[index + 1U] != 0U || pixels[index + 2U] != 0U) {
      ++count;
    }
  }
  return count;
}

std::size_t FirstNonzeroByte(std::span<const uint8_t> bytes) {
  const auto it = std::find_if(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0U; });
  return it == bytes.end() ? std::numeric_limits<std::size_t>::max()
                           : static_cast<std::size_t>(it - bytes.begin());
}

const std::array<uint32_t, 27> kClearVertexShader = {
    0x30052003U, 0x00001200U, 0xC4000000U, 0x00001005U, 0x00001200U, 0xC2000000U, 0x00001006U,
    0x10071200U, 0x22000000U, 0x30081000U, 0x00393A88U, 0x00000007U, 0x00080000U, 0x40263688U,
    0x00000300U, 0xC80F8000U, 0x00000000U, 0xE2000000U, 0xC80F803EU, 0x00000000U, 0xE2010100U,
    0xC8000000U, 0x00000000U, 0xE2000000U, 0x00000000U, 0x00000000U, 0x00000000U};

const std::array<uint32_t, 9> kClearPixelShader = {0x00000000U, 0x1001C400U, 0x22000000U,
                                                   0xC80F8000U, 0x00000000U, 0xE2000000U,
                                                   0x00000000U, 0x00000000U, 0x00000000U};

// This small writer pair is retained only for the two RG16_FLOAT attachment
// producers. It emits the exact finite source values used by the control.
const std::array<uint32_t, 45> kWriterVertexShader = {
    0x30052003U, 0x00001200U, 0xC2000000U, 0x00006005U, 0x200B1200U, 0x12000000U, 0x00000000U,
    0x100DC400U, 0x22000000U, 0x2DF82000U, 0x00393A88U, 0x0000000EU, 0x05F80000U, 0x40253E78U,
    0x0000040EU, 0xC8020001U, 0x00A7A700U, 0xAF020100U, 0xC8080001U, 0x00A7A700U, 0xAF020300U,
    0xC8020000U, 0x00A7A700U, 0xAF020200U, 0xB8410101U, 0x01A7A741U, 0x8F020008U, 0xC801803EU,
    0x00A7A700U, 0xAF010400U, 0xC802803EU, 0x00A7A700U, 0xAF010500U, 0xC804803EU, 0x00A7A700U,
    0xAF010600U, 0xC808803EU, 0x00A7A700U, 0xAF010700U, 0xC8038000U, 0x00C4C400U, 0xE2000000U,
    0x4E4A0007U, 0x95A570DEU, 0x30B52E1DU};

const std::array<uint32_t, 12> kWriterPixelShader = {
    0x00000000U, 0x2001C400U, 0x22000009U, 0x10080001U, 0x1F1FFFFBU, 0x00004000U,
    0xA8878000U, 0x00C0C000U, 0x02FFFF00U, 0x4E4A0006U, 0x95A570DEU, 0x30B52E1DU};

// This is the signed dual-texture consumer. Its PS contains the two required
// signed format-31 uses at fetch 1 and fetch 0; fetch 1 writes B.RG to r4.XY,
// while fetch 0 routes A.G to r1.X and A.R to r1.Z. The terminal add uses
// r4.XYXY and r1.XXZW so every producer channel reaches terminal RGB. The
// additional required fetches are bound to ordinary prepared RGBA8 sources.
constexpr std::array<uint32_t, 27> kConsumerVertexShader = {
    0x10011003U, 0x00001200U, 0xC2000000U, 0x00001004U, 0x00001200U, 0xC4000000U, 0x10093005U,
    0x00002200U, 0x00000000U, 0x05F81000U, 0x00393A88U, 0x00000008U, 0xC80F803EU, 0x00000000U,
    0xE2010100U, 0x05F80000U, 0x00253FC8U, 0x00000608U, 0xC8000000U, 0x00000000U, 0x02000000U,
    0xC8038000U, 0x00B0B000U, 0xE2000000U, 0x00000000U, 0x00000000U, 0x00000000U};

constexpr std::array<uint32_t, 63> kConsumerPixelShader = {
    0x00000000U, 0x6002C400U, 0x12000425U, 0x00256008U, 0x600E1200U, 0x22000000U, 0x10104001U,
    0x1F1FFFC8U, 0x00004000U, 0x10001001U, 0x1F1FFE39U, 0x00004000U, 0x20220301U, 0x016C6C6CU,
    0x211A0180U, 0x140C0000U, 0x00066CB1U, 0xA1000101U, 0x0C140303U, 0x00C66C6CU, 0xA1001C01U,
    0x10280001U, 0x1F1FF688U, 0x00004000U, 0x90382061U, 0x1F1FF688U, 0x00004000U, 0x18483061U,
    0x1F1FF688U, 0x00004000U, 0x65120000U, 0x001B1BBCU, 0xC002031FU, 0x4C180002U, 0x041B6C6CU,
    0xA001FF00U, 0xB8470002U, 0x00C0C083U, 0x8102191FU, 0xC8070003U, 0x01C0C0C0U, 0xAB031B02U,
    0xC8070002U, 0x00C01BC0U, 0xEB040302U, 0xC9010000U, 0x00C66C00U, 0xE1000000U, 0xB2170002U,
    0x02C0C0C0U, 0x80021E1FU, 0xC80D0000U, 0x00F06CF0U, 0xCB02001EU, 0xC8070002U, 0x00A00C00U,
    0xC0040100U, 0xA8878000U, 0x00000000U, 0x8B02FFFEU, 0x00000000U, 0x00000000U, 0x00000000U};

// The writer VS uses two identity transforms: c0..c3 map its position fetch
// to clip-space inputs and c4..c7 preserve that position. c8 supplies the
// depth subtraction and is zero for this control. Pixel c0.w is the
// multiplicative alpha identity, while c255.xyz supplies the output color.
constexpr std::array<uint32_t, 36> kWriterVertexConstants = {
    0x3F800000U, 0U,          0U, 0U, 0U, 0x3F800000U, 0U,          0U, 0U, 0U,
    0x3F800000U, 0U,          0U, 0U, 0U, 0x3F800000U, 0x3F800000U, 0U, 0U, 0U,
    0U,          0x3F800000U, 0U, 0U, 0U, 0U,          0x3F800000U, 0U, 0U, 0U,
    0U,          0x3F800000U, 0U, 0U, 0U, 0U};

constexpr std::array<uint32_t, 4> kWriterPixelConstant0 = {0U, 0U, 0U, 0x3F800000U};
constexpr std::array<uint32_t, 36> kConsumerVertexConstants = {
    0x4178512EU, 0U,          0U,          0U,          0x459C4000U, 0U,          0U,          0U,
    0U,          0U,          0U,          0U,          0U,          0x44A00000U, 0x44340000U, 0U,
    0U,          0xBE9E3781U, 0x3F737870U, 0U,          0U,          0xBF737870U, 0xBE9E3781U, 0U,
    0U,          0U,          0U,          0x3F800000U, 0U,          0U,          0U,          0U,
    0U,          0x3F800000U, 0xBF7FA631U, 0x3D565DEAU};
constexpr std::array<uint32_t, 128> kConsumerPixelConstants = {
    0x4178512EU, 0U,          0U,          0U,          0x459C4000U, 0U,          0U,
    0U,          0U,          0U,          0U,          0U,          0x44A00000U, 0x44340000U,
    0U,          0U,          0xBE9E3781U, 0x3F737870U, 0U,          0U,          0xBF737870U,
    0xBE9E3781U, 0U,          0U,          0U,          0U,          0x3F800000U, 0U,
    0U,          0U,          0U,          0x3F800000U, 0xBF7FA631U, 0x3D565DEAU, 0U,
    0U,          0xBD565DEAU, 0xBF7FA631U, 0U,          0U,          0U,          0U,
    0x3F800000U, 0U,          0U,          0U,          0U,          0x3F800000U, 0U,
    0U,          0U,          0U,          0U,          0U,          0U,          0U,
    0U,          0U,          0U,          0U,          0U,          0U,          0U,
    0U,          0U,          0U,          0U,          0U,          0U,          0U,
    0U,          0U,          0U,          0U,          0U,          0U,          0U,
    0U,          0U,          0U,          0U,          0U,          0U,          0U,
    0U,          0U,          0U,          0U,          0U,          0U,          0U,
    0U,          0U,          0U,          0U,          0U,          0U,          0U,
    0U,          0U,          0x3EE0E0E2U, 0x3F1B9B9CU, 0x3F34B4B5U, 0U,          0x3A699999U,
    0x3F800000U, 0U,          0U,          0x3F5CDCDEU, 0x3F75F5F7U, 0x3F800000U, 0U,
    0x3A4EB852U, 0xC4000000U, 0U,          0x3F800000U, 0x448AC000U, 0xC2200000U, 0U,
    0U,          0x3EC0C0C2U, 0x3EF6F6F8U, 0x3F7BFBFDU, 0U,          0x4625676EU, 0x4635CF6EU,
    0U,          0x3F800000U};
// c254.w is the alpha identity used by RB_BLENDCONTROL0's source-one path;
// RGB remains driven by the signed A/B fetches in the terminal add.
constexpr std::array<uint32_t, 4> kConsumerPixelConstant254 = {0x3F000000U, 0x3F000000U,
                                                               0x3F000000U, 0x3F800000U};
constexpr std::array<uint32_t, 4> kConsumerPixelConstant255 = {0x3C800000U, 0x3C800000U,
                                                               0x3C800000U, 0x3F800000U};
// These are the actual guest c255 values from the prepared shader inputs.
// build-native-shaders.py specializes color_format=4 with expbias=0 as
// output * 2^-5, so the title's native exports are A=(0.25,-0.015625) and
// B=(-0.0234375,0.375). The resolved RG16_FLOAT halves are therefore
// A=(8,-0.5) and B=(-0.75,12), all within the installed signed resolve range.
constexpr std::array<uint32_t, 4> kProducerAOutputConstant = {0x41000000U, 0xBF000000U, 0U,
                                                              0x3F800000U};
constexpr std::array<uint32_t, 4> kProducerBOutputConstant = {0xBF400000U, 0x41400000U, 0U,
                                                              0x3F800000U};

void WritePacketWords(rex::memory::Memory& memory, uint32_t ring_address,
                      std::span<const uint32_t> words) {
  auto* ring = memory.TranslatePhysical<uint8_t*>(ring_address);
  for (std::size_t i = 0; i < words.size(); ++i) {
    rex::memory::store_and_swap<uint32_t>(ring + i * sizeof(uint32_t), words[i]);
  }
}

void PublishWritePointer(rex::memory::Memory& memory, uint32_t word_count) {
  auto* write_pointer = memory.TranslateVirtual<volatile uint32_t*>(kWptrAddress);
  *write_pointer = rex::byte_swap(word_count);
}

uint32_t ReadWriteback(rex::memory::Memory& memory, uint32_t writeback_address) {
  return rex::memory::load_and_swap<uint32_t>(memory.TranslatePhysical(writeback_address));
}

uint64_t Fnv1a(std::span<const uint8_t> bytes) {
  uint64_t hash = 1469598103934665603ULL;
  for (uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

void PrintHex(std::span<const uint8_t> bytes) {
  std::ios_base::fmtflags flags = std::cout.flags();
  std::cout << std::hex << std::setfill('0');
  for (uint8_t byte : bytes) {
    std::cout << std::setw(2) << static_cast<unsigned>(byte);
  }
  std::cout.flags(flags);
}

void PrintRegisterDiagnostics(const DrawRecord& draw) {
  std::cout << "draw_registers id=" << draw.id << " words=" << std::dec << draw.register_word_count
            << " digest=0x" << std::hex << draw.register_digest;
  for (std::size_t i = 0; i < kDiagnosticRegisterIndices.size(); ++i) {
    std::cout << " r0x" << kDiagnosticRegisterIndices[i] << "=0x" << draw.register_subset[i];
  }
  std::cout << std::dec << '\n';
}

bool WaitForWriteback(rex::memory::Memory& memory, uint32_t writeback_address,
                      uint32_t expected_word_count) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ReadWriteback(memory, writeback_address) == expected_word_count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

bool Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "installed_prepared_control: " << message << '\n';
  }
  return condition;
}

bool WriteTerminalPixels(const rex::ui::RawImage& image, std::filesystem::path& output_path) {
  const char* value = std::getenv("REREVVED_PREPARED_CONTROL_PIXELS");
  if (!value || *value == '\0') {
    return false;
  }
  output_path = std::filesystem::path(value);
  std::ofstream output(output_path, std::ios::binary | std::ios::noreplace);
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char*>(image.data.data()),
               static_cast<std::streamsize>(image.data.size()));
  return output.good();
}

std::unique_ptr<rex::system::IGraphicsSystem> LoadInstalledGpuPlugin(
    rex::platform::DynamicLibrary& library, std::string& error) {
#if defined(_WIN32)
  std::array<wchar_t, 32768> executable_path{};
  const DWORD path_length = GetModuleFileNameW(nullptr, executable_path.data(),
                                               static_cast<DWORD>(executable_path.size()));
  if (path_length == 0U || path_length >= executable_path.size()) {
    error = "GetModuleFileNameW failed for installed GPU plugin";
    return nullptr;
  }
  const std::filesystem::path plugin_path =
      std::filesystem::path(executable_path.data(), executable_path.data() + path_length)
          .parent_path() /
      "rexgpu-xenos.dll";
  if (!library.Load(plugin_path, rex::platform::SymbolResolution::kImmediate)) {
    error = "installed rexgpu-xenos.dll failed to load: " + plugin_path.string();
    return nullptr;
  }
  const auto abi_version =
      library.GetSymbol<rex::system::GpuAbiVersionFn>(rex::system::kGpuAbiVersionSymbol);
  const auto create = library.GetSymbol<rex::system::GpuCreateFn>(rex::system::kGpuCreateSymbol);
  if (!abi_version || !create || abi_version() != rex::system::kGpuPluginAbiVersion) {
    error = "installed rexgpu-xenos.dll has no compatible plugin ABI";
    return nullptr;
  }
  std::string backend = "d3d12";
  rex::system::GpuCreateInfo info{};
  info.struct_size = sizeof(info);
  info.backend = backend.c_str();
  rex::system::IGraphicsSystem* graphics = create(rex::system::kGpuPluginAbiVersion, &info);
  if (!graphics) {
    error = "installed rexgpu-xenos.dll returned no d3d12 graphics system";
    return nullptr;
  }
  return std::unique_ptr<rex::system::IGraphicsSystem>(graphics);
#else
  (void)library;
  error = "installed RG16 control requires Windows D3D12";
  return nullptr;
#endif
}

}  // namespace

namespace rex::graphics::testing {

// The installed plugin locator uses the CRT's wide executable path.
int RunRg16Control(
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& forwarded,
    Rg16ControlResult& result, std::string& error) {
  result = {};
  error.clear();
  struct LoggingScope {
    LoggingScope() {
      rex::LogConfig config;
      config.log_to_console = true;
      rex::InitLogging(config);
    }
    ~LoggingScope() { rex::ShutdownLogging(); }
  } logging;
  const char* raw_mode_value = std::getenv("REREVVED_PREPARED_CONTROL_RAW");
  const bool raw_mode = raw_mode_value && std::string_view(raw_mode_value) == "1";
  result.readback_mode = raw_mode;
  // Keep the installed plugin module alive until the graphics system has shut down.
  rex::platform::DynamicLibrary plugin_library;
  auto graphics = LoadInstalledGpuPlugin(plugin_library, error);
  if (!Require(graphics != nullptr, "xenos d3d12 plugin was not loaded")) {
    return 1;
  }

  rex::Runtime runtime(std::filesystem::path{});
  if (!Require(runtime.Setup(rex::RuntimeConfig{
                   .kernel_init = rex::kernel::InitializeKernel,
                   .tool_mode = true,
               }) == static_cast<rex::X_STATUS>(0),
               "runtime setup failed")) {
    return 1;
  }

  // Positive mode deliberately keeps the Prepared source path CPU complete.
  // The opt-in raw companion runs backend readback and records an actual
  // fence-trace completion separately; it never changes positive mode.
  if (!Require(rex::cvar::SetFlagByName("d3d12_readback_resolve", raw_mode ? "true" : "false"),
               "d3d12_readback_resolve setting was rejected")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  // Match the title's full-range SNORM setting. The title-side /32
  // specialization and this CVar are part of the immutable input identity.
  if (!Require(rex::cvar::SetFlagByName("snorm16_render_target_full_range", "true"),
               "snorm16_render_target_full_range setting was rejected")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  if (!Require(rex::cvar::SetFlagAsApplicationDefault("render_target_path_d3d12", "rtv"),
               "D3D12 RTV render target path application default was rejected")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  if (!Require(rex::cvar::SetFlagByName("async_shader_compilation", "false"),
               "synchronous shader compilation was rejected")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  if (!Require(graphics->SetupPresentation(nullptr) == static_cast<rex::X_STATUS>(0),
               "offscreen D3D12 presenter setup failed")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  if (!Require(graphics->SetupGuestGpu(runtime.function_dispatcher(), runtime.kernel_state()) ==
                   static_cast<rex::X_STATUS>(0),
               "guest GPU setup failed")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  auto& fence_trace = rex::graphics::diagnostic::GetXenosFenceTrace();
  std::filesystem::path fence_trace_path;
  if (raw_mode) {
    const char* trace_value = std::getenv("REREVVED_PREPARED_CONTROL_FENCE_TRACE");
    if (!trace_value || *trace_value == '\0') {
      error = "raw RG16 mode requires REREVVED_PREPARED_CONTROL_FENCE_TRACE";
      graphics->Shutdown();
      runtime.Shutdown();
      return 1;
    }
    fence_trace_path = std::filesystem::path(trace_value);
    if (!Require(fence_trace.Start(fence_trace_path), "XenosFenceTrace failed to start")) {
      graphics->Shutdown();
      runtime.Shutdown();
      return 1;
    }
    result.raw_fence_trace_path = fence_trace_path.string();
  }

  auto mailbox = rex::graphics::diagnostic::GetDrawCaptureMailbox();
  auto observation = std::make_shared<OriginalObservation>(forwarded);
  const auto observer_token = mailbox->RegisterFrameInputObserver(observation);
  if (!Require(static_cast<bool>(observer_token), "mailbox observer registration failed")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  rex::memory::Memory& memory = *runtime.memory();
  const uint32_t terminal_guest_extent =
      GuestTextureExtent(kSurfaceWidth, kSurfaceHeight, kSurfaceWidth);
  const uint32_t rg16_guest_extent = SignedRg16GuestTextureExtent();
  const uint32_t consumer_guest_extent =
      GuestTextureExtent(kConsumerWidth, kConsumerHeight, kConsumerWidth);
  const uint32_t terminal_guest_allocation_bytes = AlignToPage(terminal_guest_extent);
  const uint32_t rg16_guest_allocation_bytes = AlignToPage(rg16_guest_extent);
  const uint32_t consumer_guest_allocation_bytes = AlignToPage(consumer_guest_extent);
  if (!Require(terminal_guest_extent != 0U && terminal_guest_allocation_bytes != 0U,
               "terminal tiled texture layout was empty") ||
      !Require(rg16_guest_extent == 0x398000U && rg16_guest_allocation_bytes != 0U,
               "RG16 tiled texture layout was not the canonical extent") ||
      !Require(consumer_guest_extent != 0U && consumer_guest_allocation_bytes != 0U,
               "consumer tiled texture layout was empty")) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  GuestAllocation ring(memory, kRingWordCount * sizeof(uint32_t), 0x1000U);
  GuestAllocation writeback(memory, 0x1000U, 0x1000U);
  GuestAllocation destination(memory, terminal_guest_allocation_bytes, 0x1000U);
  GuestAllocation producer_a(memory, rg16_guest_allocation_bytes, 0x1000U);
  GuestAllocation producer_b(memory, rg16_guest_allocation_bytes, 0x1000U);
  GuestAllocation consumer_source_2(memory, consumer_guest_allocation_bytes, 0x1000U);
  GuestAllocation consumer_source_3(memory, consumer_guest_allocation_bytes, 0x1000U);
  GuestAllocation consumer_source_4(memory, consumer_guest_allocation_bytes, 0x1000U);
  GuestAllocation resolve_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation full_resolve_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation writer_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation required_consumer_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation index_data(memory, 0x1000U, 0x1000U);
  GuestAllocation required_index_data(memory, 0x1000U, 0x1000U);
  GuestAllocation writer_vertex(memory, static_cast<uint32_t>(kWriterVertexShader.size() * 4U),
                                0x1000U);
  GuestAllocation writer_pixel(memory, static_cast<uint32_t>(kWriterPixelShader.size() * 4U),
                               0x1000U);
  GuestAllocation consumer_vertex(memory, static_cast<uint32_t>(kConsumerVertexShader.size() * 4U),
                                  0x1000U);
  GuestAllocation consumer_pixel(memory, static_cast<uint32_t>(kConsumerPixelShader.size() * 4U),
                                 0x1000U);
  const bool allocations_ok = ring && writeback && destination && producer_a && producer_b &&
                              consumer_source_2 && consumer_source_3 && consumer_source_4 &&
                              resolve_geometry && full_resolve_geometry && writer_geometry &&
                              required_consumer_geometry && index_data && required_index_data &&
                              writer_vertex && writer_pixel && consumer_vertex && consumer_pixel;
  if (!Require(allocations_ok, "guest physical test allocations failed")) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    consumer_pixel.Reset();
    consumer_vertex.Reset();
    writer_pixel.Reset();
    writer_vertex.Reset();
    required_index_data.Reset();
    index_data.Reset();
    required_consumer_geometry.Reset();
    writer_geometry.Reset();
    full_resolve_geometry.Reset();
    resolve_geometry.Reset();
    consumer_source_4.Reset();
    consumer_source_3.Reset();
    consumer_source_2.Reset();
    producer_b.Reset();
    producer_a.Reset();
    destination.Reset();
    writeback.Reset();
    ring.Reset();
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  const auto resolve_geometry_bytes = ResolveGeometry();
  const auto right_resolve_geometry_bytes = RightResolveGeometry();
  const auto full_resolve_geometry_bytes = FullResolveGeometry();
  const auto writer_geometry_bytes = ConsumerGeometry();
  const auto required_consumer_geometry_bytes = RequiredConsumerGeometry();
  std::memcpy(resolve_geometry.physical_bytes(), resolve_geometry_bytes.data(),
              resolve_geometry_bytes.size());
  std::memcpy(resolve_geometry.physical_bytes() + 0x100, right_resolve_geometry_bytes.data(),
              right_resolve_geometry_bytes.size());
  std::memcpy(full_resolve_geometry.physical_bytes(), full_resolve_geometry_bytes.data(),
              full_resolve_geometry_bytes.size());
  std::memcpy(writer_geometry.physical_bytes(), writer_geometry_bytes.data(),
              writer_geometry_bytes.size());
  std::memcpy(required_consumer_geometry.physical_bytes(), required_consumer_geometry_bytes.data(),
              required_consumer_geometry_bytes.size());
  const std::array<uint8_t, 6> indices = {0, 0, 0, 1, 0, 2};
  std::memcpy(index_data.physical_bytes(), indices.data(), indices.size());
  const std::array<uint8_t, 12> required_indices = {0, 0, 0, 2, 0, 1, 0, 3, 0, 5, 0, 4};
  std::memcpy(required_index_data.physical_bytes(), required_indices.data(),
              required_indices.size());
  std::memset(destination.physical_bytes(), 0, terminal_guest_allocation_bytes);
  // Poison both producer allocations with +0.125 in RG16_FLOAT. The raw
  // proof must observe the GPU-written values rather than passing from a
  // prefilled buffer that already contains the expected output.
  FillRg16FloatPattern(producer_a.physical_bytes(), rg16_guest_allocation_bytes, 0x3000U, 0x3000U);
  FillRg16FloatPattern(producer_b.physical_bytes(), rg16_guest_allocation_bytes, 0x3000U, 0x3000U);
  std::memset(consumer_source_2.physical_bytes(), 0x33, consumer_guest_allocation_bytes);
  std::memset(consumer_source_3.physical_bytes(), 0x66, consumer_guest_allocation_bytes);
  std::memset(consumer_source_4.physical_bytes(), 0x99, consumer_guest_allocation_bytes);
  // IM_LOAD reads guest-endian ucode dwords. Keep the arrays in host order
  // for the packet builder and write their guest representation explicitly.
  WriteShaderWords(writer_vertex, kWriterVertexShader);
  WriteShaderWords(writer_pixel, kWriterPixelShader);
  WriteShaderWords(consumer_vertex, kConsumerVertexShader);
  WriteShaderWords(consumer_pixel, kConsumerPixelShader);

  graphics->InitializeRingBuffer(ring.physical_address(), kRingSizeLog2);
  graphics->EnableReadPointerWriteBack(writeback.physical_address(), kWritebackBlockSizeLog2);

  const auto destination_texture =
      TextureFetch(destination.physical_address(), kSurfaceWidth, kSurfaceHeight, kSurfaceWidth);
  const auto producer_a_texture = SignedRg16TextureFetch(producer_a.physical_address());
  const auto producer_b_texture = SignedRg16TextureFetch(producer_b.physical_address());
  const auto full_resolve_fetch =
      VertexFetch(full_resolve_geometry.physical_address(),
                  static_cast<uint32_t>(full_resolve_geometry_bytes.size()));
  const auto writer_fetch = VertexFetch(writer_geometry.physical_address(),
                                        static_cast<uint32_t>(writer_geometry_bytes.size()));
  const auto required_consumer_fetch =
      VertexFetch(required_consumer_geometry.physical_address(),
                  static_cast<uint32_t>(required_consumer_geometry_bytes.size()));
  const auto consumer_source_2_texture = TextureFetch(
      consumer_source_2.physical_address(), kConsumerWidth, kConsumerHeight, kConsumerWidth);
  const auto consumer_source_3_texture = TextureFetch(
      consumer_source_3.physical_address(), kConsumerWidth, kConsumerHeight, kConsumerWidth);
  const auto consumer_source_4_texture = TextureFetch(
      consumer_source_4.physical_address(), kConsumerWidth, kConsumerHeight, kConsumerWidth);
  const auto index_fetch_address = index_data.physical_address();
  const auto required_index_fetch_address = required_index_data.physical_address();

  PacketStream stream;
  // Open the mailbox frame with an actual swap. The destination fetch is a
  // real guest texture descriptor owned by the D3D12 texture cache.
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, destination_texture);
  stream.Swap(destination.physical_address(), kSurfaceWidth, kSurfaceHeight);

  const auto append_rg16_writer = [&](const std::array<uint32_t, 6>& source_fetch,
                                      uint32_t color_surface,
                                      const std::array<uint32_t, 4>& output_constant) {
    // Write the signed RG16 sample into the selected k16_16 EDRAM attachment
    // through the installed shader/render-target path before resolving it.
    stream.LoadShader(writer_vertex.physical_address(), rex::graphics::xenos::ShaderType::kVertex,
                      static_cast<uint32_t>(kWriterVertexShader.size()));
    stream.LoadShader(writer_pixel.physical_address(), rex::graphics::xenos::ShaderType::kPixel,
                      static_cast<uint32_t>(kWriterPixelShader.size()));
    stream.Type0(0x2000U, {0x14000500U, color_surface, 0x530U});
    stream.Type0(0x200FU, {0x20002000U});
    stream.Type0(0x2080U, {0U, 0U, kSurfaceWidth | (kSurfaceHeight << 16)});
    stream.Type0(0x2100U, {2U});
    stream.Type0(0x210FU, {std::bit_cast<uint32_t>(640.0F), std::bit_cast<uint32_t>(640.0F),
                           std::bit_cast<uint32_t>(-360.0F), std::bit_cast<uint32_t>(360.0F),
                           0x3F800000U, 0U});
    stream.Type0(0x2104U, {0xFU});
    stream.Type0(0x210DU, {0x00FFFF07U});
    stream.Type0(0x2200U, {0x24F00730U});
    // Preserve each exported component through the fixed-point attachment;
    // the writer's fetch is only a bounded ordinary source for Prepared data.
    stream.Type0(0x2201U, {0x00010001U});
    stream.Type0(0x2202U, {0x87000004U});
    stream.Type0(0x2204U, {0x80000U});
    stream.Type0(0x2205U, {0x18000U});
    stream.Type0(0x2206U, {0x43FU});
    stream.Type0(0x2208U, {4U});
    stream.Type0(0x2302U, {4U});
    stream.Type0(0x2307U, {0xFF000U});
    stream.Type0(0x2308U, {0xFF100U});
    stream.Type0(0x2180U, {0x10010002U});
    stream.Type0(0x2181U, {4U});
    stream.Type0(0x2182U, {1U});
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X, kWriterVertexConstants);
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X, kWriterPixelConstant0);
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U, output_constant);
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, source_fetch);
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U, writer_fetch);
    stream.Type0(
        0x21FCU,
        {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
         (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6) | (3U << 16)});
    stream.DrawDma(0x00030004U, index_fetch_address, 3U, rex::graphics::xenos::Endian::k8in16);
  };

  const auto append_rg16_resolve = [&](uint32_t output_address, uint32_t color_surface) {
    // The two producer copies retain the captured no-clear source state and
    // the exact RG16_FLOAT destination words. Allocation aliases are used for
    // safety; the register image remains the captured variant in the observer.
    stream.Type0(0x2000U, {0x14000500U, color_surface, 0x530U});
    stream.Type0(0x200FU, {0x20002000U});
    stream.Type0(0x2080U, {0U, 0U, kSurfaceWidth | (kSurfaceHeight << 16)});
    stream.Type0(0x2205U, {0x10000U});
    stream.Type0(0x2208U, {6U});
    stream.Type0(0x2318U, {0x00100000U});
    stream.Type0(0x2319U, {output_address});
    stream.Type0(0x231AU, {0x02D00500U});
    stream.Type0(0x231BU, {0x0000EF82U});
    stream.Type0(0x231CU, {0xFFFU});
    stream.Type0(0x231DU, {0xFFFFFF00U});
    stream.Type0(0x231EU, {0U, 0U});
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, full_resolve_fetch);
    stream.Type0(0x21FCU,
                 {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) |
                  (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kAutoIndex) << 6) |
                  (3U << 16)});
    stream.DrawAuto(0x00030088U);
  };

  const auto append_terminal_resolve = [&](uint32_t output_address) {
    stream.Type0(0x2000U, {0x14000500U, 0x00000000U, 0x530U});
    stream.Type0(0x200FU, {0x20002000U});
    stream.Type0(0x2080U, {0U, 0U, kSurfaceWidth | (kSurfaceHeight << 16)});
    stream.Type0(0x2205U, {0x10000U});
    stream.Type0(0x2208U, {6U});
    stream.Type0(0x2318U, {0x00100000U});
    stream.Type0(0x2319U, {output_address});
    stream.Type0(0x231AU, {0x02D00500U});
    stream.Type0(0x231BU, {0x01000300U});
    stream.Type0(0x231CU, {0xFFFU});
    stream.Type0(0x231DU, {0xFFFFFF00U});
    stream.Type0(0x231EU, {0U, 0U});
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, full_resolve_fetch);
    stream.Type0(0x21FCU,
                 {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) |
                  (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kAutoIndex) << 6) |
                  (3U << 16)});
    stream.DrawAuto(0x00030088U);
  };

  // The writer's output is intentionally independent of texture fetch. The
  // required signed reads happen in the terminal consumer; keeping these
  // producer draws fetch-free isolates the attachment write and makes the
  // resolved bytes prove the two finite constants below.
  append_rg16_writer(consumer_source_2_texture, 0x00040000U, kProducerAOutputConstant);
  append_rg16_resolve(producer_a.physical_address(), 0x00040000U);

  append_rg16_writer(consumer_source_3_texture, 0x000402D0U, kProducerBOutputConstant);
  append_rg16_resolve(producer_b.physical_address(), 0x000402D0U);

  // One full-frame consumer samples signed producer B at fetch 1, signed
  // producer A at fetch 0, and three ordinary prepared sources at fetches 2-4.
  // The captured PS uses all four RG channels from the two signed fetches.
  stream.LoadShader(consumer_vertex.physical_address(), rex::graphics::xenos::ShaderType::kVertex,
                    static_cast<uint32_t>(kConsumerVertexShader.size()));
  stream.LoadShader(consumer_pixel.physical_address(), rex::graphics::xenos::ShaderType::kPixel,
                    static_cast<uint32_t>(kConsumerPixelShader.size()));
  stream.Type0(0x2000U, {0x14000500U, 0U, 0x530U});
  stream.Type0(0x200FU, {0x20002000U});
  stream.Type0(0x2080U, {0U, 0U, kSurfaceWidth | (kSurfaceHeight << 16)});
  stream.Type0(0x2100U, {5U});
  stream.Type0(0x2104U, {0xFU});
  stream.Type0(0x210DU, {0x00FFFF00U});
  stream.Type0(0x2200U, {0x24F00730U});
  // Use source-one blending so the required consumer's explicit alpha
  // identity cannot suppress its four-channel RGB contribution.
  stream.Type0(0x2201U, {0x00010001U});
  stream.Type0(0x2202U, {0x87000000U});
  stream.Type0(0x2204U, {0x80000U});
  stream.Type0(0x2205U, {0x18000U});
  stream.Type0(0x2206U, {0x43FU});
  stream.Type0(0x2208U, {4U});
  stream.Type0(0x2302U, {4U});
  stream.Type0(0x2307U, {0xFF000U});
  stream.Type0(0x2308U, {0xFF100U});
  stream.Type0(0x2180U, {0x10010401U});
  stream.Type0(0x2181U, {4U});
  stream.Type0(0x2182U, {1U});
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X, kConsumerVertexConstants);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X, kConsumerPixelConstants);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X + 254U * 4U,
               kConsumerPixelConstant254);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U,
               kConsumerPixelConstant255);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, producer_a_texture);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 6U, producer_b_texture);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 12U,
               consumer_source_2_texture);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 18U,
               consumer_source_3_texture);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 24U,
               consumer_source_4_texture);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U,
               required_consumer_fetch);
  stream.Type0(0x21FCU, {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
                         (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6) |
                         (6U << 16)});
  stream.DrawDma(0x00060004U, required_index_fetch_address, 6U,
                 rex::graphics::xenos::Endian::k8in16);

  append_terminal_resolve(destination.physical_address());
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, destination_texture);
  stream.Swap(destination.physical_address(), kSurfaceWidth, kSurfaceHeight);

  constexpr std::size_t kSwapPacketWordCount = 5U;
  const std::size_t final_swap_packet_start = stream.words().size() >= kSwapPacketWordCount
                                                  ? stream.words().size() - kSwapPacketWordCount
                                                  : 0U;
  const bool final_swap_packet_shape =
      stream.words().size() >= kSwapPacketWordCount &&
      stream.words()[final_swap_packet_start] ==
          rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_XE_SWAP, 4) &&
      stream.words()[final_swap_packet_start + 1U] == rex::graphics::xenos::kSwapSignature &&
      stream.words()[final_swap_packet_start + 2U] == destination.physical_address() &&
      stream.words()[final_swap_packet_start + 3U] == kSurfaceWidth &&
      stream.words()[final_swap_packet_start + 4U] == kSurfaceHeight;
  if (!Require(stream.words().size() < kRingWordCount, "PM4 stream exceeds ring capacity") ||
      !Require(final_swap_packet_shape, "final control swap packet shape is invalid")) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    graphics->Shutdown();
    consumer_pixel.Reset();
    consumer_vertex.Reset();
    writer_pixel.Reset();
    writer_vertex.Reset();
    required_index_data.Reset();
    index_data.Reset();
    required_consumer_geometry.Reset();
    writer_geometry.Reset();
    resolve_geometry.Reset();
    consumer_source_4.Reset();
    consumer_source_3.Reset();
    consumer_source_2.Reset();
    producer_b.Reset();
    producer_a.Reset();
    destination.Reset();
    writeback.Reset();
    ring.Reset();
    runtime.Shutdown();
    return 1;
  }
  WritePacketWords(memory, ring.physical_address(), stream.words());
  std::uint64_t reservation_token = 0U;
  if (raw_mode) {
    const uint32_t ring_capacity_dwords = kRingWordCount;
    const uint32_t swap_packet_start = static_cast<uint32_t>(final_swap_packet_start);
    const uint32_t reservation_index =
        (swap_packet_start + ring_capacity_dwords - 8U) % ring_capacity_dwords;
    result.raw_watched_packet_start = swap_packet_start;
    result.raw_watched_packet_end = swap_packet_start + kSwapPacketWordCount;
    reservation_token = fence_trace.WatchSwapReservation(
        ring.virtual_address() + reservation_index * sizeof(uint32_t),
        ring.physical_address() + reservation_index * sizeof(uint32_t));
    result.raw_reservation_token = reservation_token;
    if (!Require(reservation_token != 0U,
                 "XenosFenceTrace rejected the control swap reservation")) {
      error = "XenosFenceTrace could not watch the control swap reservation";
    }
  }
  PublishWritePointer(memory, static_cast<uint32_t>(stream.words().size()));
  const bool write_pointer_published = true;
  const bool progressed = WaitForWriteback(memory, writeback.physical_address(),
                                           static_cast<uint32_t>(stream.words().size())) &&
                          write_pointer_published;

  // Ring writeback only proves PM4 consumption. Wait for the actual terminal
  // resolve callback and frame end, then use the installed presenter's own
  // readback before shutdown destroys its resource and fence owners.
  const bool completion_observed =
      progressed && observation->WaitForCompletion(destination.physical_address());
  rex::ui::RawImage terminal_image;
  const bool terminal_capture = completion_observed && graphics->presenter() != nullptr &&
                                graphics->presenter()->CaptureGuestOutput(terminal_image);
  std::filesystem::path terminal_pixels_path;
  const bool terminal_pixels_written =
      terminal_capture && WriteTerminalPixels(terminal_image, terminal_pixels_path);
  result.progressed = progressed;
  result.completion_observed = completion_observed;
  result.terminal_capture = terminal_capture;
  if (terminal_capture) {
    result.terminal_width = terminal_image.width;
    result.terminal_height = terminal_image.height;
    result.terminal_stride = terminal_image.stride;
    result.terminal_pixels = terminal_image.data;
  }
  bool raw_trace_flushed = !raw_mode;
  bool final_swap_decoded = !raw_mode;
  bool final_swap_range_complete = !raw_mode;
  bool final_swap_submission = !raw_mode;
  bool final_swap_completion = !raw_mode;
  mailbox->UnregisterFrameInputObserver(observer_token);
  graphics->Shutdown();
  if (raw_mode) {
    // Shutdown drains the D3D12 queue. Finish the trace only after that drain
    // so the watched final swap has a real completed-fence event.
    raw_trace_flushed = fence_trace.FinishAndFlush();
    if (!Require(raw_trace_flushed, "XenosFenceTrace failed to flush")) {
      error = "XenosFenceTrace did not flush a complete raw GPU trace";
    }
    std::vector<rex::graphics::diagnostic::XenosFenceTraceEvent> trace_events;
    trace_events.reserve(rex::graphics::diagnostic::kXenosFenceTraceCapacity);
    for (std::size_t index = 0; index < rex::graphics::diagnostic::kXenosFenceTraceCapacity;
         ++index) {
      rex::graphics::diagnostic::XenosFenceTraceEvent event{};
      if (fence_trace.CopyEvent(index, event)) {
        trace_events.push_back(event);
      }
    }
    for (const auto& event : trace_events) {
      if (event.point == rex::graphics::diagnostic::XenosFenceTracePoint::kSwapDecoded &&
          event.correlation_token == reservation_token &&
          event.packet_start_index == result.raw_watched_packet_start &&
          event.packet_end_index == result.raw_watched_packet_end) {
        final_swap_decoded = true;
        result.raw_fence_correlation_token = event.correlation_token;
        result.raw_fence_range_identity = event.range_identity;
      }
    }
    std::uint64_t range_correlation_mask = 0U;
    std::uint64_t submitted_correlation_mask = 0U;
    std::uint64_t completed_correlation_mask = 0U;
    std::uint64_t completed_fence = 0U;
    for (const auto& event : trace_events) {
      if (!final_swap_decoded || event.range_identity != result.raw_fence_range_identity) {
        continue;
      }
      if (event.point == rex::graphics::diagnostic::XenosFenceTracePoint::kConsumerRangeBegin) {
        range_correlation_mask = event.correlation_mask;
        result.raw_watched_range_start = event.range_start_index;
      } else if (event.point ==
                 rex::graphics::diagnostic::XenosFenceTracePoint::kConsumerRangeEnd) {
        final_swap_range_complete = event.correlation_mask == range_correlation_mask &&
                                    event.range_end_index == stream.words().size();
        result.raw_watched_range_end = event.range_end_index;
      }
    }
    for (const auto& event : trace_events) {
      if (event.point == rex::graphics::diagnostic::XenosFenceTracePoint::kD3D12Submission &&
          event.correlation_mask == range_correlation_mask && range_correlation_mask != 0U) {
        submitted_correlation_mask = event.correlation_mask;
      }
      if (event.point == rex::graphics::diagnostic::XenosFenceTracePoint::kD3D12FenceCompleted &&
          event.correlation_mask == range_correlation_mask && range_correlation_mask != 0U &&
          (event.flags & rex::graphics::diagnostic::kXenosFenceTraceCompletionSucceeded) &&
          event.fence_completed_value != 0U) {
        completed_correlation_mask = event.correlation_mask;
        completed_fence = std::max(completed_fence, event.fence_completed_value);
      }
    }
    final_swap_submission =
        submitted_correlation_mask == range_correlation_mask && range_correlation_mask != 0U;
    final_swap_completion = completed_correlation_mask == range_correlation_mask &&
                            range_correlation_mask != 0U && completed_fence != 0U;
    result.raw_fence_correlation_mask = completed_correlation_mask;
    const auto trace_statistics = fence_trace.statistics();
    result.raw_fence_value = completed_fence;
    result.raw_gpu_proof = raw_trace_flushed && final_swap_decoded && final_swap_range_complete &&
                           final_swap_submission && final_swap_completion &&
                           trace_statistics.valid_for_promotion();
    if (!result.raw_gpu_proof) {
      error = "XenosFenceTrace final swap completion provenance was incomplete";
    }
  }
  const auto observed = observation->TakeSnapshot();

  const auto copy_result = [](const CopyRecord& copy) {
    return Rg16ControlCopyRecord{copy.id,
                                 copy.frame,
                                 copy.submission,
                                 copy.mode,
                                 copy.original_succeeded,
                                 copy.written_address,
                                 copy.written_length,
                                 copy.planned_address,
                                 copy.planned_length,
                                 copy.cpu_data_complete,
                                 copy.prepared};
  };
  for (const auto& copy : observed.copies) {
    result.original_copies.push_back(copy_result(copy));
  }
  for (const auto& copy : observed.prepared_copies) {
    result.prepared_copies.push_back(copy_result(copy));
  }
  result.vertex_shader_words.assign(kConsumerVertexShader.begin(), kConsumerVertexShader.end());
  result.pixel_shader_words.assign(kConsumerPixelShader.begin(), kConsumerPixelShader.end());
  for (const auto& draw : observed.prepared_draws) {
    result.draw_registers.push_back(draw.registers);
    result.draw_vertex_shader_words.push_back(draw.vertex_shader_words);
    result.draw_pixel_shader_words.push_back(draw.pixel_shader_words);
  }
  for (const auto& draw : observed.draws) {
    if (draw.pixel_word_digest == ShaderWordDigest(kConsumerPixelShader)) {
      result.consumer_registers = draw.registers;
      break;
    }
  }
  for (const auto& copy : observed.copies) {
    if (copy.planned_length == rg16_guest_extent) {
      if (result.producer_registers.empty()) {
        result.producer_registers = copy.registers;
      }
    }
  }
  result.producer_a = {producer_a.physical_address(),
                       kSurfaceWidth,
                       kSurfaceHeight,
                       kSurfaceWidth,
                       40U,
                       31U,
                       0x55U,
                       rg16_guest_extent,
                       0U,
                       false,
                       {}};
  result.producer_b = {producer_b.physical_address(),
                       kSurfaceWidth,
                       kSurfaceHeight,
                       kSurfaceWidth,
                       40U,
                       31U,
                       0x55U,
                       rg16_guest_extent,
                       0U,
                       false,
                       {}};
  if (result.raw_gpu_proof) {
    result.producer_a.completed_fence = result.raw_fence_value;
    result.producer_b.completed_fence = result.raw_fence_value;
    result.producer_a.raw_gpu_proof = true;
    result.producer_b.raw_gpu_proof = true;
  }
  const bool producer_a_decoded =
      raw_mode && DecodeRg16FloatTexture(
                      producer_a_texture,
                      std::span<const uint8_t>(producer_a.physical_bytes(), rg16_guest_extent),
                      kSurfaceWidth, kSurfaceHeight, result.producer_a.linear_rg16_float);
  const bool producer_b_decoded =
      raw_mode && DecodeRg16FloatTexture(
                      producer_b_texture,
                      std::span<const uint8_t>(producer_b.physical_bytes(), rg16_guest_extent),
                      kSurfaceWidth, kSurfaceHeight, result.producer_b.linear_rg16_float);

  std::vector<uint8_t> producer_pixels;
  const bool producer_decoded = DecodeRgba8Texture(
      destination_texture,
      std::span<const uint8_t>(destination.physical_bytes(), terminal_guest_extent), kSurfaceWidth,
      kSurfaceHeight, producer_pixels);
  const uint64_t producer_pixel_hash = Fnv1a(producer_pixels);
  const uint64_t terminal_pixel_hash = terminal_capture ? Fnv1a(terminal_image.data) : 0U;
  const std::size_t producer_nonzero_rgb = NonzeroRgbPixelCount(producer_pixels);
  const std::size_t terminal_nonzero_rgb =
      terminal_capture ? NonzeroRgbPixelCount(terminal_image.data) : 0U;
  const std::size_t producer_first_nonzero = FirstNonzeroByte(producer_pixels);
  const std::size_t terminal_first_nonzero = terminal_capture
                                                 ? FirstNonzeroByte(terminal_image.data)
                                                 : std::numeric_limits<std::size_t>::max();
  const bool terminal_has_activity = terminal_capture && terminal_nonzero_rgb != 0U;

  const auto expected_writer_draw = [](const DrawRecord& draw) {
    return draw.original_host_issued && draw.cpu_data_complete &&
           draw.execution_status == DrawCaptureExecutionStatus::kIssued &&
           draw.execution_reason == DrawCaptureReason::kNone &&
           draw.primitive_type ==
               static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) &&
           draw.requested_index_count == 3 &&
           draw.vertex_word_digest == ShaderWordDigest(kWriterVertexShader) &&
           draw.pixel_word_digest == ShaderWordDigest(kWriterPixelShader) &&
           draw.vertex_shader_hash != 0U && draw.pixel_shader_hash != 0U;
  };
  const auto expected_consumer_draw = [](const DrawRecord& draw) {
    return draw.original_host_issued &&
           draw.execution_status == DrawCaptureExecutionStatus::kIssued &&
           draw.execution_reason == DrawCaptureReason::kNone &&
           draw.primitive_type ==
               static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) &&
           draw.requested_index_count == 6 &&
           draw.vertex_word_digest == ShaderWordDigest(kConsumerVertexShader) &&
           draw.pixel_word_digest == ShaderWordDigest(kConsumerPixelShader) &&
           draw.vertex_shader_hash != 0U && draw.pixel_shader_hash != 0U;
  };
  const bool expected_draws =
      observed.draws.size() == 3 && expected_writer_draw(observed.draws[0]) &&
      expected_writer_draw(observed.draws[1]) && expected_consumer_draw(observed.draws[2]);
  const std::string_view expected_copy_mode = raw_mode ? "resolve_readback" : "resolve";
  const bool expected_copies =
      observed.copies.size() == 3 && observed.copies[0].planned_length == rg16_guest_extent &&
      observed.copies[1].planned_length == rg16_guest_extent &&
      observed.copies[2].planned_length == terminal_guest_extent &&
      std::all_of(observed.copies.begin(), observed.copies.end(), [&](const CopyRecord& copy) {
        return copy.mode == expected_copy_mode && copy.original_succeeded &&
               copy.planned_address != 0U && copy.planned_length != 0U &&
               copy.written_address == copy.planned_address &&
               copy.written_length == copy.planned_length;
      });
  const bool expected_prepared_copies =
      observed.prepared_copies.size() == observed.copies.size() &&
      observed.prepared_copies.size() == 3 && [&] {
        for (std::size_t index = 0; index < observed.copies.size(); ++index) {
          const auto& prepared = observed.prepared_copies[index];
          const auto& original = observed.copies[index];
          if (prepared.id != original.id || prepared.frame != original.frame ||
              (raw_mode ? (prepared.submission + 1U != original.submission)
                        : (prepared.submission != original.submission)) ||
              prepared.mode != expected_copy_mode || original.mode != expected_copy_mode ||
              !prepared.prepared || prepared.original_succeeded || prepared.written_address != 0U ||
              prepared.written_length != 0U || prepared.planned_address == 0U ||
              prepared.planned_length == 0U || original.id != prepared.id ||
              !original.original_succeeded ||
              original.written_address != prepared.planned_address ||
              (raw_mode ? prepared.cpu_data_complete : !prepared.cpu_data_complete) ||
              original.written_length != prepared.planned_length) {
            return false;
          }
        }
        return true;
      }();
  const bool expected_terminal =
      observed.swaps.size() == 2 &&
      observed.swaps.back().frontbuffer_ptr == destination.physical_address() &&
      observed.swaps.back().width == kSurfaceWidth &&
      observed.swaps.back().height == kSurfaceHeight && observed.end_count >= 1 &&
      observed.last_end_succeeded && terminal_capture && terminal_image.width == kSurfaceWidth &&
      terminal_image.height == kSurfaceHeight && terminal_image.stride == kSurfaceWidth * 4U &&
      terminal_image.data.size() == terminal_image.stride * terminal_image.height &&
      terminal_has_activity;
  const auto has_uniform_rg16_sample = [](std::span<const std::uint8_t> pixels, std::uint16_t red,
                                          std::uint16_t green) {
    if (pixels.size() != static_cast<std::size_t>(kSurfaceWidth) * kSurfaceHeight * 4U) {
      return false;
    }
    for (std::size_t index = 0; index < pixels.size(); index += 4U) {
      if (pixels[index] != static_cast<std::uint8_t>(red) ||
          pixels[index + 1U] != static_cast<std::uint8_t>(red >> 8U) ||
          pixels[index + 2U] != static_cast<std::uint8_t>(green) ||
          pixels[index + 3U] != static_cast<std::uint8_t>(green >> 8U)) {
        return false;
      }
    }
    return true;
  };
  // Keep the raw companion strict. Candidate-0003's full-range k_16_16
  // resolve clamps decoded signed EDRAM lanes below -1 before the final
  // RG16_FLOAT write, so this fixture stays within that demonstrated range;
  // wider negative probes remain rejected evidence.
  const bool raw_values =
      raw_mode && result.raw_gpu_proof && producer_a_decoded && producer_b_decoded &&
      has_uniform_rg16_sample(result.producer_a.linear_rg16_float, 0x4800U, 0xB800U) &&
      has_uniform_rg16_sample(result.producer_b.linear_rg16_float, 0xBA00U, 0x4A00U);
  // Positive mode deliberately exposes no CPU copy of the producer textures:
  // raw mode supplies a separately read-back and fence-qualified GPU proof.
  const bool expected_pixels = terminal_capture &&
                               terminal_image.data.size() == kSurfaceLinearBytes &&
                               terminal_nonzero_rgb != 0U && (!raw_mode || raw_values);

  std::cout << "control=installed_prepared_original\n";
  std::cout << "readback_mode=" << (raw_mode ? "true" : "false") << '\n';
  std::cout << "progressed=" << (progressed ? "true" : "false") << '\n';
  std::cout << "completion_observed=" << (completion_observed ? "true" : "false") << '\n';
  std::cout << "pm4_words=" << stream.words().size() << '\n';
  std::cout << "draw_count=" << observed.draws.size() << '\n';
  std::cout << "copy_count=" << observed.copies.size() << '\n';
  std::cout << "prepared_copy_count=" << observed.prepared_copies.size() << '\n';
  std::cout << "swap_count=" << observed.swaps.size() << '\n';
  std::cout << "frame_begin_count=" << observed.begin_count << '\n';
  std::cout << "frame_end_count=" << observed.end_count << '\n';
  for (const auto& draw : observed.draws) {
    std::cout << "draw id=" << draw.id << " frame=" << draw.frame
              << " submission=" << draw.submission
              << " issued=" << (draw.original_host_issued ? "true" : "false")
              << " complete=" << (draw.cpu_data_complete ? "true" : "false")
              << " status=" << static_cast<uint32_t>(draw.execution_status)
              << " reason=" << static_cast<uint32_t>(draw.execution_reason)
              << " primitive=" << draw.primitive_type << " indices=" << draw.requested_index_count
              << " vs_hash=0x" << std::hex << draw.vertex_shader_hash << " ps_hash=0x"
              << draw.pixel_shader_hash << std::dec << " vs_digest=" << draw.vertex_word_digest
              << " ps_digest=" << draw.pixel_word_digest << '\n';
    PrintRegisterDiagnostics(draw);
    std::cout << "draw_consts id=" << draw.id << " c0=0x" << std::hex
              << RegisterValue(draw.registers, 0x4400U) << " c25=0x"
              << RegisterValue(draw.registers, 0x4464U) << " c255=0x"
              << RegisterValue(draw.registers, 0x47FCU) << std::dec << '\n';
  }
  for (const auto& copy : observed.copies) {
    std::cout << "copy id=" << copy.id << " frame=" << copy.frame
              << " submission=" << copy.submission << " mode=" << copy.mode
              << " succeeded=" << (copy.original_succeeded ? "true" : "false") << " written=0x"
              << std::hex << copy.written_address << std::dec << "," << copy.written_length
              << " planned=0x" << std::hex << copy.planned_address << std::dec << ","
              << copy.planned_length << " complete=" << (copy.cpu_data_complete ? "true" : "false")
              << '\n';
  }
  for (const auto& copy : observed.prepared_copies) {
    std::cout << "prepared_copy id=" << copy.id << " frame=" << copy.frame
              << " submission=" << copy.submission << " mode=" << copy.mode
              << " succeeded=" << (copy.original_succeeded ? "true" : "false") << " planned=0x"
              << std::hex << copy.planned_address << std::dec << "," << copy.planned_length
              << " complete=" << (copy.cpu_data_complete ? "true" : "false") << '\n';
  }
  for (std::size_t index = 0; index < result.draw_registers.size(); ++index) {
    std::cout << "prepared_draw_bank index=" << index
              << " registers=" << result.draw_registers[index].size() << " reg_digest=0x"
              << std::hex << RegisterDigest(result.draw_registers[index]) << std::dec
              << " vs_words=" << result.draw_vertex_shader_words[index].size()
              << " vs_digest=" << ShaderWordDigest(result.draw_vertex_shader_words[index])
              << " ps_words=" << result.draw_pixel_shader_words[index].size()
              << " ps_digest=" << ShaderWordDigest(result.draw_pixel_shader_words[index]) << '\n';
  }
  std::cout << "expected_draw_graph=" << (expected_draws ? "true" : "false") << '\n';
  std::cout << "expected_copy_graph=" << (expected_copies ? "true" : "false") << '\n';
  std::cout << "expected_prepared_copy_pairing=" << (expected_prepared_copies ? "true" : "false")
            << '\n';
  std::cout << "expected_terminal=" << (expected_terminal ? "true" : "false") << '\n';
  std::cout << "expected_pixels=" << (expected_pixels ? "true" : "false") << '\n';
  std::cout << "terminal_guest_tiled_bytes=" << terminal_guest_extent << '\n';
  std::cout << "consumer_source_guest_tiled_bytes=" << consumer_guest_extent << '\n';
  std::cout << "consumer_source_guest_allocation_bytes=" << consumer_guest_allocation_bytes << '\n';
  std::cout << "producer_linear_bytes=" << producer_pixels.size() << "\n";
  std::cout << "producer_pixel_fnv1a=0x" << std::hex << producer_pixel_hash << std::dec << '\n';
  std::cout << "producer_nonzero_rgb_pixels=" << producer_nonzero_rgb << '\n';
  std::cout << "producer_first_nonzero_byte=" << producer_first_nonzero << '\n';
  std::cout << "terminal_capture=" << (terminal_capture ? "true" : "false") << '\n';
  std::cout << "terminal_pixels_written=" << (terminal_pixels_written ? "true" : "false") << '\n';
  std::cout << "terminal_pixels_path=" << terminal_pixels_path.string() << '\n';
  std::cout << "terminal_image=" << terminal_image.width << "x" << terminal_image.height
            << " stride=" << terminal_image.stride << " bytes=" << terminal_image.data.size()
            << " fnv1a=0x" << std::hex << terminal_pixel_hash << std::dec << '\n';
  std::cout << "terminal_nonzero_rgb_pixels=" << terminal_nonzero_rgb << '\n';
  std::cout << "terminal_first_nonzero_byte=" << terminal_first_nonzero << '\n';
  std::cout << "raw_reservation_token=0x" << std::hex << result.raw_reservation_token
            << " raw_fence_correlation_token=0x" << result.raw_fence_correlation_token
            << " raw_fence_correlation_mask=0x" << result.raw_fence_correlation_mask
            << " raw_fence_range_identity=0x" << result.raw_fence_range_identity << std::dec
            << '\n';
  std::cout << "raw_watched_packet=" << result.raw_watched_packet_start << ','
            << result.raw_watched_packet_end
            << " raw_watched_range=" << result.raw_watched_range_start << ','
            << result.raw_watched_range_end << '\n';
  std::cout << "terminal_pixel_sample=";
  PrintHex(std::span<const uint8_t>(terminal_image.data)
               .first(std::min<std::size_t>(terminal_image.data.size(), 64U)));
  std::cout << '\n';

  const bool passed = progressed && completion_observed && expected_draws && expected_copies &&
                      expected_prepared_copies && expected_terminal && expected_pixels;

  consumer_pixel.Reset();
  consumer_vertex.Reset();
  writer_pixel.Reset();
  writer_vertex.Reset();
  required_index_data.Reset();
  index_data.Reset();
  required_consumer_geometry.Reset();
  writer_geometry.Reset();
  full_resolve_geometry.Reset();
  resolve_geometry.Reset();
  consumer_source_4.Reset();
  consumer_source_3.Reset();
  consumer_source_2.Reset();
  producer_b.Reset();
  producer_a.Reset();
  destination.Reset();
  writeback.Reset();
  ring.Reset();
  runtime.Shutdown();
  return passed ? 0 : 1;
}

}  // namespace rex::graphics::testing
