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

#include <rex/crypto/sha256.h>
#include <rex/cvar.h>
#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>

#include "main_clear_control.h"

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
constexpr uint32_t kMainResolveWidth = 640U;
// Keep the CPU-owned source at a canonical single-tile size. The consumer
// still covers the paired 640-pixel attachment halves; the source only needs
// to provide a uniform opaque sample to the regular pixel shader.
constexpr uint32_t kConsumerWidth = 32U;
constexpr uint32_t kConsumerHeight = 32U;

// Keep this subset small enough for the control output to remain reviewable,
// while covering the state that distinguishes the synthetic clear from the
// qualified consumer draw. The full register image is summarized separately.
constexpr std::array<uint32_t, 27> kDiagnosticRegisterIndices = {
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
      DrawRecord record;
      record.id = draw.id;
      record.frame = draw.frame;
      record.submission = draw.submission;
      record.vertex_shader_hash = draw.vertex_shader.hash;
      record.pixel_shader_hash = draw.pixel_shader.hash;
      record.vertex_word_digest = ShaderWordDigest(draw.vertex_shader.words);
      record.pixel_word_digest = ShaderWordDigest(draw.pixel_shader.words);
      record.original_host_issued = draw.original_host_issued;
      record.cpu_data_complete = draw.cpu_data_complete;
      record.execution_status = draw.execution_status;
      record.execution_reason = draw.execution_reason;
      record.primitive_type = draw.primitive_type;
      record.requested_index_count = draw.requested_index_count;
      record.register_word_count = draw.registers.size();
      record.register_digest = RegisterDigest(draw.registers);
      record.register_subset = CaptureRegisterSubset(draw.registers);
      draws_.push_back(std::move(record));
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

  bool WantsPreparedCopies() const noexcept override { return true; }

  bool OnCopyPrepared(const DrawCaptureFrameInputCopyView& copy) override {
    {
      std::lock_guard lock(mutex_);
      prepared_copies_.push_back(MakeCopyRecord(copy, true));
    }
    condition_.notify_all();
    return !forwarded_ || forwarded_->OnCopyPrepared(copy);
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

  bool WantsPreparedDraws() const noexcept override {
    return forwarded_ && forwarded_->WantsPreparedDraws();
  }

  bool OnDrawPrepared(const DrawCaptureFrameInputDrawView& draw) override {
    return !forwarded_ || forwarded_->OnDrawPrepared(draw);
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
    return !forwarded_ || forwarded_->OnSwapPrepared(swap);
  }

  bool WaitForCompletion(uint32_t terminal_address) {
    (void)terminal_address;
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(15), [&] {
      const bool terminal_copy =
          std::any_of(copies_.begin(), copies_.end(), [&](const CopyRecord& copy) {
            return copy.original_succeeded && copy.mode == "resolve";
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
  std::vector<CopyRecord> copies_;
  std::vector<CopyRecord> prepared_copies_;
  std::vector<SwapRecord> swaps_;
  uint64_t last_end_frame_ = 0;
  uint64_t last_end_submission_ = 0;
  bool last_end_succeeded_ = false;
  std::shared_ptr<DrawCaptureFrameInputObserver> forwarded_;

  static CopyRecord MakeCopyRecord(const DrawCaptureFrameInputCopyView& copy, bool prepared) {
    return {copy.id,
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

uint32_t GuestTextureExtent(uint32_t width, uint32_t height, uint32_t pitch) {
  const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
      rex::graphics::xenos::DataDimension::k2DOrStacked, pitch / 32U, width, height, 1U, true,
      rex::graphics::xenos::TextureFormat::k_8_8_8_8, false, true, 0U);
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

// These are the raw words from catalog specialization-0048, whose generated
// source is the same specialization-0010 pair. The pixel shader is unsigned
// and samples texture fetch constant 0, while the vertex shader reads vfetch95
// at offsets 0 and 16 with a 56-byte stride.
const std::array<uint32_t, 45> kConsumerVertexShader = {
    0x30052003U, 0x00001200U, 0xC2000000U, 0x00006005U, 0x200B1200U, 0x12000000U, 0x00000000U,
    0x100DC400U, 0x22000000U, 0x2DF82000U, 0x00393A88U, 0x0000000EU, 0x05F80000U, 0x40253E78U,
    0x0000040EU, 0xC8020001U, 0x00A7A700U, 0xAF020100U, 0xC8080001U, 0x00A7A700U, 0xAF020300U,
    0xC8020000U, 0x00A7A700U, 0xAF020200U, 0xB8410101U, 0x01A7A741U, 0x8F020008U, 0xC801803EU,
    0x00A7A700U, 0xAF010400U, 0xC802803EU, 0x00A7A700U, 0xAF010500U, 0xC804803EU, 0x00A7A700U,
    0xAF010600U, 0xC808803EU, 0x00A7A700U, 0xAF010700U, 0xC8038000U, 0x00C4C400U, 0xE2000000U,
    0x4E4A0007U, 0x95A570DEU, 0x30B52E1DU};

const std::array<uint32_t, 12> kConsumerPixelShader = {
    0x00000000U, 0x2001C400U, 0x22000009U, 0x10080001U, 0x1F1FFFFBU, 0x00004000U,
    0xA8878000U, 0x00C0C000U, 0x02FFFF00U, 0x4E4A0006U, 0x95A570DEU, 0x30B52E1DU};

// The qualified VS uses two identity transforms: c0..c3 map its position
// fetch to clip-space inputs and c4..c7 preserve that position. c8 supplies
// the depth subtraction and is zero for this control. Pixel c0.w is the
// multiplicative alpha identity, while c255.xyz supplies the output color.
constexpr std::array<uint32_t, 36> kConsumerVertexConstants = {
    0x3F800000U, 0U,          0U, 0U, 0U, 0x3F800000U, 0U,          0U, 0U, 0U,
    0x3F800000U, 0U,          0U, 0U, 0U, 0x3F800000U, 0x3F800000U, 0U, 0U, 0U,
    0U,          0x3F800000U, 0U, 0U, 0U, 0U,          0x3F800000U, 0U, 0U, 0U,
    0U,          0x3F800000U, 0U, 0U, 0U, 0U};

constexpr std::array<uint32_t, 4> kConsumerPixelConstant0 = {0U, 0U, 0U, 0x3F800000U};
constexpr std::array<uint32_t, 4> kConsumerPixelConstant255 = {0x3DCCCCCDU, 0x3E4CCCCDU,
                                                               0x3E99999AU, 0x3F800000U};
constexpr std::array<uint32_t, 4> kStencilSeedPixelConstant255 = {0x3E4CCCCDU, 0x3ECCCCCDU,
                                                                  0x3F19999AU, 0x3F800000U};

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

bool ReadProducerAlpha(float& producer_alpha) {
  const char* value = std::getenv("REREVVED_PREPARED_CONTROL_ALPHA");
  if (!value || *value == '\0' || std::string_view(value) == "1") {
    producer_alpha = 1.0F;
    return true;
  }
  if (std::string_view(value) == "0") {
    producer_alpha = 0.0F;
    return true;
  }
  if (std::string_view(value) == "0.5") {
    producer_alpha = 0.5F;
    return true;
  }
  return false;
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

}  // namespace

namespace rex::graphics::testing {

// The installed plugin locator uses the CRT's wide executable path.
int RunMainClearControl(
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& forwarded,
    MainClearControlResult& result, std::string& error) {
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
  float producer_alpha = 1.0F;
  if (!ReadProducerAlpha(producer_alpha)) {
    std::cerr
        << "installed_prepared_control: REREVVED_PREPARED_CONTROL_ALPHA must be 0, 0.5, or 1\n";
    return 1;
  }

  // Match the production ReXApp order by loading the plugin before runtime setup.
  auto graphics = rex::system::LoadGpuPlugin("xenos", "d3d12");
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

  // The control explicitly selects the installed backend's disabled readback
  // mode after the plugin has registered its backend flags. This is a
  // per-process test input and does not alter SDK defaults.
  if (!Require(rex::cvar::SetFlagByName("readback_resolve", "none"),
               "readback_resolve=none was rejected")) {
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

  auto mailbox = rex::graphics::diagnostic::GetDrawCaptureMailbox();
  auto observation = std::make_shared<OriginalObservation>(forwarded);
  const auto observer_token = mailbox->RegisterFrameInputObserver(observation);
  if (!Require(static_cast<bool>(observer_token), "mailbox observer registration failed")) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  rex::memory::Memory& memory = *runtime.memory();
  const uint32_t producer_guest_extent =
      GuestTextureExtent(kSurfaceWidth, kSurfaceHeight, kSurfaceWidth);
  const uint32_t consumer_guest_extent =
      GuestTextureExtent(kConsumerWidth, kConsumerHeight, kConsumerWidth);
  const uint32_t producer_guest_allocation_bytes = AlignToPage(producer_guest_extent);
  const uint32_t consumer_guest_allocation_bytes = AlignToPage(consumer_guest_extent);
  if (!Require(producer_guest_extent != 0U && producer_guest_allocation_bytes != 0U,
               "producer tiled texture layout was empty") ||
      !Require(consumer_guest_extent != 0U && consumer_guest_allocation_bytes != 0U,
               "consumer tiled texture layout was empty")) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }
  GuestAllocation ring(memory, kRingWordCount * sizeof(uint32_t), 0x1000U);
  GuestAllocation writeback(memory, 0x1000U, 0x1000U);
  GuestAllocation destination(memory, producer_guest_allocation_bytes, 0x1000U);
  GuestAllocation consumer_source(memory, consumer_guest_allocation_bytes, 0x1000U);
  GuestAllocation clear_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation resolve_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation consumer_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation index_data(memory, 0x1000U, 0x1000U);
  GuestAllocation clear_vertex(memory, static_cast<uint32_t>(kClearVertexShader.size() * 4U),
                               0x1000U);
  GuestAllocation clear_pixel(memory, static_cast<uint32_t>(kClearPixelShader.size() * 4U),
                              0x1000U);
  GuestAllocation consumer_vertex(memory, static_cast<uint32_t>(kConsumerVertexShader.size() * 4U),
                                  0x1000U);
  GuestAllocation consumer_pixel(memory, static_cast<uint32_t>(kConsumerPixelShader.size() * 4U),
                                 0x1000U);
  const bool allocations_ok = ring && writeback && destination && consumer_source &&
                              clear_geometry && resolve_geometry && consumer_geometry &&
                              index_data && clear_vertex && clear_pixel && consumer_vertex &&
                              consumer_pixel;
  if (!Require(allocations_ok, "guest physical test allocations failed")) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    consumer_pixel.Reset();
    consumer_vertex.Reset();
    clear_pixel.Reset();
    clear_vertex.Reset();
    index_data.Reset();
    consumer_geometry.Reset();
    resolve_geometry.Reset();
    clear_geometry.Reset();
    consumer_source.Reset();
    destination.Reset();
    writeback.Reset();
    ring.Reset();
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  const auto clear_geometry_bytes = ClearGeometry(producer_alpha);
  const auto resolve_geometry_bytes = ResolveGeometry();
  const auto right_resolve_geometry_bytes = RightResolveGeometry();
  const auto consumer_geometry_bytes = ConsumerGeometry();
  std::memcpy(clear_geometry.physical_bytes(), clear_geometry_bytes.data(),
              clear_geometry_bytes.size());
  std::memcpy(resolve_geometry.physical_bytes(), resolve_geometry_bytes.data(),
              resolve_geometry_bytes.size());
  std::memcpy(resolve_geometry.physical_bytes() + 0x100, right_resolve_geometry_bytes.data(),
              right_resolve_geometry_bytes.size());
  std::memcpy(consumer_geometry.physical_bytes(), consumer_geometry_bytes.data(),
              consumer_geometry_bytes.size());
  const std::array<uint8_t, 6> indices = {0, 0, 0, 1, 0, 2};
  std::memcpy(index_data.physical_bytes(), indices.data(), indices.size());
  std::memset(destination.physical_bytes(), 0, producer_guest_allocation_bytes);
  std::memset(consumer_source.physical_bytes(), 0xFF, consumer_guest_allocation_bytes);
  // IM_LOAD reads guest-endian ucode dwords. Keep the arrays in host order
  // for the packet builder and write their guest representation explicitly.
  WriteShaderWords(clear_vertex, kClearVertexShader);
  WriteShaderWords(clear_pixel, kClearPixelShader);
  WriteShaderWords(consumer_vertex, kConsumerVertexShader);
  WriteShaderWords(consumer_pixel, kConsumerPixelShader);

  graphics->InitializeRingBuffer(ring.physical_address(), kRingSizeLog2);
  graphics->EnableReadPointerWriteBack(writeback.physical_address(), kWritebackBlockSizeLog2);

  const auto destination_texture =
      TextureFetch(destination.physical_address(), kSurfaceWidth, kSurfaceHeight, kSurfaceWidth);
  const auto consumer_source_texture = TextureFetch(
      consumer_source.physical_address(), kConsumerWidth, kConsumerHeight, kConsumerWidth);
  auto clear_fetch = VertexFetch(clear_geometry.physical_address(),
                                 static_cast<uint32_t>(clear_geometry_bytes.size()));
  clear_fetch[1] |= 0x10000000U;
  const auto resolve_fetch = VertexFetch(resolve_geometry.physical_address(),
                                         static_cast<uint32_t>(resolve_geometry_bytes.size()));
  const auto right_resolve_fetch =
      VertexFetch(resolve_geometry.physical_address() + 0x100,
                  static_cast<uint32_t>(right_resolve_geometry_bytes.size()));
  const auto consumer_fetch = VertexFetch(consumer_geometry.physical_address(),
                                          static_cast<uint32_t>(consumer_geometry_bytes.size()));
  const auto index_fetch_address = index_data.physical_address();

  PacketStream stream;
  // Open the mailbox frame with an actual swap. The destination fetch is a
  // real guest texture descriptor owned by the D3D12 texture cache.
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, destination_texture);
  stream.Swap(destination.physical_address(), kSurfaceWidth, kSurfaceHeight);

  // Synthetic earliest producer: clear the selected 640x720 RGBA8 color-0
  // surface and seed depth with the main clear values.
  stream.LoadShader(clear_vertex.physical_address(), rex::graphics::xenos::ShaderType::kVertex,
                    static_cast<uint32_t>(kClearVertexShader.size()));
  stream.LoadShader(clear_pixel.physical_address(), rex::graphics::xenos::ShaderType::kPixel,
                    static_cast<uint32_t>(kClearPixelShader.size()));
  stream.Type0(0x2000U, {0x0A010280U, 0U, 0x530U});
  stream.Type0(0x200FU, {0x20002000U});
  stream.Type0(0x2080U, {0U, 0U, 0x20002000U});
  stream.Type0(0x2104U, {0xFU});
  stream.Type0(0x2100U, {2U});
  stream.Type0(0x210DU, {0x00FF0000U});
  stream.Type0(0x2200U, {0x8777U});
  stream.Type0(0x2201U, {0x00010001U});
  stream.Type0(0x2204U, {0x10000U});
  stream.Type0(0x2205U, {0x10000U});
  stream.Type0(0x2206U, {0x300U});
  stream.Type0(0x2208U, {4U});
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, clear_fetch);
  stream.Type0(
      0x21FCU,
      {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) |
       (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kAutoIndex) << 6) | (3U << 16)});
  stream.DrawAuto(0x00030088U);

  // Replace stencil with seven through a regular color draw. Its color output
  // preserves the seed identity while depth remains disabled, and its opaque
  // CPU texture keeps both prepared shader views available.
  stream.LoadShader(consumer_vertex.physical_address(), rex::graphics::xenos::ShaderType::kVertex,
                    static_cast<uint32_t>(kConsumerVertexShader.size()));
  stream.LoadShader(consumer_pixel.physical_address(), rex::graphics::xenos::ShaderType::kPixel,
                    static_cast<uint32_t>(kConsumerPixelShader.size()));
  stream.Type0(0x2000U, {0x0A010280U, 0U, 0x530U});
  stream.Type0(0x200FU, {0x20002000U});
  stream.Type0(0x2080U, {0U, 0U, kMainResolveWidth | (kSurfaceHeight << 16)});
  stream.Type0(0x210FU, {std::bit_cast<uint32_t>(320.0F), std::bit_cast<uint32_t>(320.0F),
                         std::bit_cast<uint32_t>(-360.0F), std::bit_cast<uint32_t>(360.0F),
                         0x3F800000U, 0U});
  stream.Type0(0x2104U, {0xFU});
  stream.Type0(0x210DU, {0x00FFFF07U});
  stream.Type0(0x2200U, {0x8701U});
  stream.Type0(0x2201U, {0x00010001U});
  stream.Type0(0x2202U, {0x87000000U});
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
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X, kConsumerVertexConstants);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X, kConsumerPixelConstant0);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U,
               kStencilSeedPixelConstant255);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, consumer_source_texture);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U, consumer_fetch);
  stream.Type0(0x21FCU, {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
                         (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6) |
                         (3U << 16)});
  stream.DrawDma(0x00030004U, index_fetch_address, 3U, rex::graphics::xenos::Endian::k8in16);

  const auto append_resolve = [&](uint32_t output_address) {
    // Resolve the current EDRAM color-0 surface through the real D3D12
    // render-target cache and readback owner. This is an actual copy draw,
    // not a mailbox fixture.
    stream.Type0(0x2000U, {0x0A010280U, 0U, 0x530U});
    stream.Type0(0x200FU, {0x20002000U});
    stream.Type0(0x2080U, {0U, 0U, kMainResolveWidth | (kSurfaceHeight << 16)});
    stream.Type0(0x2205U, {0x10000U});
    stream.Type0(0x2208U, {6U});
    stream.Type0(0x2318U, {0x00100340U});
    stream.Type0(0x2319U, {output_address});
    stream.Type0(0x231AU, {0x02D00500U});
    stream.Type0(0x231BU, {0x01000300U});
    stream.Type0(0x231CU, {0xFFFU});
    stream.Type0(0x231DU, {0xFFFFFF00U});
    stream.Type0(0x231EU, {0U, 0U});
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, resolve_fetch);
    stream.Type0(0x21FCU,
                 {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) |
                  (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kAutoIndex) << 6) |
                  (3U << 16)});
    stream.DrawAuto(0x00030088U);
  };

  const auto append_right_resolve = [&](uint32_t output_address) {
    stream.Type0(0x2000U, {0x0A010280U, 0U, 0x530U});
    stream.Type0(0x200FU, {0x20002000U});
    stream.Type0(0x2080U, {0x00007D80U, kMainResolveWidth, kSurfaceWidth | (kSurfaceHeight << 16)});
    stream.Type0(0x2205U, {0x10000U});
    stream.Type0(0x2208U, {6U});
    stream.Type0(0x2318U, {0x00100040U});
    stream.Type0(0x2319U, {output_address + 0x14000U});
    stream.Type0(0x231AU, {0x02D00500U});
    stream.Type0(0x231BU, {0x01000300U});
    stream.Type0(0x231CU, {0xFFFU});
    stream.Type0(0x231DU, {0xFFFFFF00U});
    stream.Type0(0x231EU, {0U, 0U});
    stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, right_resolve_fetch);
    stream.Type0(0x21FCU,
                 {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) |
                  (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kAutoIndex) << 6) |
                  (3U << 16)});
    stream.DrawAuto(0x00030088U);
  };

  append_resolve(destination.physical_address());

  // The required consumer stays on the same attachment. Its opaque CPU
  // texture keeps the sampled input independent from the producer output.
  stream.LoadShader(consumer_vertex.physical_address(), rex::graphics::xenos::ShaderType::kVertex,
                    static_cast<uint32_t>(kConsumerVertexShader.size()));
  stream.LoadShader(consumer_pixel.physical_address(), rex::graphics::xenos::ShaderType::kPixel,
                    static_cast<uint32_t>(kConsumerPixelShader.size()));
  // This is the selected 640x720 guest 2X color-0 state. The three scissored
  // zones exercise blend, depth and stencil admission before the right copy.
  stream.Type0(0x2000U, {0x0A010280U, 0U, 0x530U});
  stream.Type0(0x200FU, {0x20002000U});
  stream.Type0(0x2080U, {0U, 0U});
  stream.Type0(0x2082U, {640U | (720U << 16)});
  stream.Type0(0x2100U, {2U, 0U, 0U});
  stream.Type0(0x2104U, {0xFU});
  stream.Type0(0x210FU, {std::bit_cast<uint32_t>(320.0F), std::bit_cast<uint32_t>(320.0F),
                         std::bit_cast<uint32_t>(-360.0F), std::bit_cast<uint32_t>(360.0F),
                         0x3F800000U, 0U});
  stream.Type0(0x2180U, {0x10010002U});
  stream.Type0(0x2181U, {4U});
  stream.Type0(0x2182U, {1U});
  stream.Type0(0x2201U, {0x00010001U});
  stream.Type0(0x2202U, {0x87000000U});
  stream.Type0(0x2204U, {0x80000U});
  stream.Type0(0x2205U, {0x18000U});
  stream.Type0(0x2206U, {0x43FU});
  stream.Type0(0x2208U, {4U});
  stream.Type0(0x2302U, {4U});
  stream.Type0(0x2307U, {0xFF000U});
  stream.Type0(0x2308U, {0xFF100U});
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X, kConsumerVertexConstants);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X, kConsumerPixelConstant0);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U,
               kConsumerPixelConstant255);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, consumer_source_texture);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U, consumer_fetch);
  // Zone 0 is the color-observable consumer: source-one plus destination-one
  // preserves the seeded attachment color when the source clear is omitted.
  stream.Type0(0x2200U, {0U});
  stream.Type0(0x210DU, {0U});
  stream.Type0(0x2201U, {0x00010101U});
  stream.Type0(0x2081U, {0U});
  stream.Type0(0x2082U, {213U | (720U << 16)});
  stream.Type0(0x21FCU, {static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
                         (static_cast<uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6) |
                         (3U << 16)});
  stream.DrawDma(0x00030004U, index_fetch_address, 3U, rex::graphics::xenos::Endian::k8in16);

  // Zone 1: depth LESS with native z=0.75.
  stream.Type0(0x2200U, {0x24F00712U});
  stream.Type0(0x210DU, {0x00FF0000U});
  stream.Type0(0x2201U, {0x00010001U});
  stream.Type0(0x2081U, {213U});
  stream.Type0(0x2082U, {426U | (720U << 16)});
  stream.DrawDma(0x00030004U, index_fetch_address, 3U, rex::graphics::xenos::Endian::k8in16);

  // Zone 2: stencil EQUAL reference zero.
  stream.Type0(0x2200U, {0x00000201U});
  stream.Type0(0x210DU, {0x00FFFF00U});
  stream.Type0(0x2201U, {0x00010001U});
  stream.Type0(0x2081U, {426U});
  stream.Type0(0x2082U, {640U | (720U << 16)});
  stream.DrawDma(0x00030004U, index_fetch_address, 3U, rex::graphics::xenos::Endian::k8in16);

  append_right_resolve(destination.physical_address());
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, destination_texture);
  stream.Swap(destination.physical_address(), kSurfaceWidth, kSurfaceHeight);

  if (!Require(stream.words().size() < kRingWordCount, "PM4 stream exceeds ring capacity")) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    graphics->Shutdown();
    consumer_pixel.Reset();
    consumer_vertex.Reset();
    clear_pixel.Reset();
    clear_vertex.Reset();
    index_data.Reset();
    consumer_geometry.Reset();
    resolve_geometry.Reset();
    clear_geometry.Reset();
    consumer_source.Reset();
    destination.Reset();
    writeback.Reset();
    ring.Reset();
    runtime.Shutdown();
    return 1;
  }
  WritePacketWords(memory, ring.physical_address(), stream.words());
  PublishWritePointer(memory, static_cast<uint32_t>(stream.words().size()));
  const bool progressed = WaitForWriteback(memory, writeback.physical_address(),
                                           static_cast<uint32_t>(stream.words().size()));

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
  graphics->Shutdown();
  mailbox->UnregisterFrameInputObserver(observer_token);
  const auto observed = observation->TakeSnapshot();

  const auto copy_result = [](const CopyRecord& copy) {
    return MainClearCopyRecord{copy.id,
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

  std::vector<uint8_t> producer_pixels;
  const bool producer_decoded = DecodeRgba8Texture(
      destination_texture,
      std::span<const uint8_t>(destination.physical_bytes(), producer_guest_extent), kSurfaceWidth,
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
  const bool terminal_has_activity =
      producer_alpha == 0.0F || HasMeaningfulRgba8Pixels(terminal_image.data);

  const bool expected_draws =
      observed.draws.size() == 5 && observed.draws[0].original_host_issued &&
      observed.draws[0].execution_status == DrawCaptureExecutionStatus::kIssued &&
      observed.draws[0].execution_reason == DrawCaptureReason::kNone &&
      observed.draws[0].primitive_type ==
          static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) &&
      observed.draws[0].requested_index_count == 3 && observed.draws[1].original_host_issued &&
      observed.draws[0].vertex_word_digest == ShaderWordDigest(kClearVertexShader) &&
      observed.draws[0].pixel_word_digest == ShaderWordDigest(kClearPixelShader) &&
      observed.draws[0].vertex_shader_hash != 0U && observed.draws[0].pixel_shader_hash != 0U &&
      std::all_of(observed.draws.begin() + 1, observed.draws.end(), [](const DrawRecord& draw) {
        return draw.original_host_issued &&
               draw.execution_status == DrawCaptureExecutionStatus::kIssued &&
               draw.execution_reason == DrawCaptureReason::kNone &&
               draw.primitive_type ==
                   static_cast<uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) &&
               draw.requested_index_count == 3 &&
               draw.vertex_word_digest == ShaderWordDigest(kConsumerVertexShader) &&
               draw.pixel_word_digest == ShaderWordDigest(kConsumerPixelShader) &&
               draw.vertex_shader_hash != 0U && draw.pixel_shader_hash != 0U;
      });
  const bool expected_copies =
      observed.copies.size() == 2 &&
      std::all_of(observed.copies.begin(), observed.copies.end(), [](const CopyRecord& copy) {
        return copy.mode == "resolve" && copy.original_succeeded && copy.planned_address != 0U &&
               copy.planned_length != 0U && copy.written_address == copy.planned_address &&
               copy.written_length == copy.planned_length;
      });
  const bool expected_prepared_copies =
      observed.prepared_copies.size() == observed.copies.size() &&
      observed.prepared_copies.size() == 2 && [&] {
        for (std::size_t index = 0; index < observed.copies.size(); ++index) {
          const auto& prepared = observed.prepared_copies[index];
          const auto& original = observed.copies[index];
          if (prepared.id != original.id || prepared.frame != original.frame ||
              prepared.submission != original.submission || prepared.mode != original.mode ||
              !prepared.prepared || !prepared.cpu_data_complete || prepared.original_succeeded ||
              prepared.written_address != 0U || prepared.written_length != 0U ||
              prepared.planned_address == 0U || prepared.planned_length == 0U ||
              original.id != prepared.id || !original.original_succeeded ||
              original.written_address != prepared.planned_address ||
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
      terminal_has_activity && terminal_pixels_written;
  // Use the presenter's captured terminal image as the pixel oracle after the
  // exact copy-range and full-capture checks above. FNV-1a is only a
  // comparison digest: an all-zero buffer still has a nonzero digest.
  const bool expected_pixels =
      terminal_capture && terminal_image.data.size() == kSurfaceLinearBytes &&
      terminal_pixel_hash != 0U && HasMeaningfulRgba8Pixels(terminal_image.data);

  std::cout << "control=installed_prepared_original\n";
  std::cout << "producer_alpha=" << producer_alpha << '\n';
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
  std::cout << "expected_draw_graph=" << (expected_draws ? "true" : "false") << '\n';
  std::cout << "expected_copy_graph=" << (expected_copies ? "true" : "false") << '\n';
  std::cout << "expected_prepared_copy_pairing=" << (expected_prepared_copies ? "true" : "false")
            << '\n';
  std::cout << "expected_terminal=" << (expected_terminal ? "true" : "false") << '\n';
  std::cout << "expected_pixels=" << (expected_pixels ? "true" : "false") << '\n';
  std::cout << "producer_guest_tiled_bytes=" << producer_guest_extent << '\n';
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
  std::cout << "terminal_pixel_sample=";
  PrintHex(std::span<const uint8_t>(terminal_image.data)
               .first(std::min<std::size_t>(terminal_image.data.size(), 64U)));
  std::cout << '\n';

  const bool passed = progressed && completion_observed && expected_draws && expected_copies &&
                      expected_prepared_copies && expected_terminal && expected_pixels;

  consumer_pixel.Reset();
  consumer_vertex.Reset();
  clear_pixel.Reset();
  clear_vertex.Reset();
  index_data.Reset();
  consumer_geometry.Reset();
  resolve_geometry.Reset();
  clear_geometry.Reset();
  consumer_source.Reset();
  destination.Reset();
  writeback.Reset();
  ring.Reset();
  runtime.Shutdown();
  return passed ? 0 : 1;
}

}  // namespace rex::graphics::testing
