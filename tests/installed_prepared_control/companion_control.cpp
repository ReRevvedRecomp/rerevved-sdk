#include "companion_control.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

#include <rex/cvar.h>
#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

namespace rex::graphics::testing {
namespace {

using diagnostic::DrawCaptureFrameInputCopyView;
using diagnostic::DrawCaptureFrameInputDrawView;
using diagnostic::DrawCaptureFrameInputObserver;
using diagnostic::DrawCaptureFrameInputSwapView;

constexpr std::uint32_t kMmioBaseAddress = 0x7FC80000U;
constexpr std::uint32_t kCpRbWptr = 0x01C5U;
constexpr std::uint32_t kWptrAddress = kMmioBaseAddress + kCpRbWptr * 4U;
constexpr std::uint32_t kRingSizeLog2 = 12U;
constexpr std::uint32_t kRingWordCount = 1U << (kRingSizeLog2 + 1U);
constexpr std::uint32_t kWritebackBlockSizeLog2 = 2U;
constexpr std::uint32_t kTargetWidth = 1280U;
constexpr std::uint32_t kTargetHeight = 720U;
constexpr std::uint32_t kSourceWidth = 256U;
constexpr std::uint32_t kSourceHeight = 32U;
constexpr std::uint32_t kSourcePitch = kSourceWidth;
constexpr std::uint32_t kSourceBytes = kSourceWidth * kSourceHeight * 4U;
constexpr std::uint32_t kBc1Width = 128U;
constexpr std::uint32_t kBc1Height = 128U;
constexpr std::uint32_t kBc1BaseBytes = 0x2000U;
constexpr std::uint32_t kBc1MipBytes = 0x4340U;
constexpr std::uint32_t kBc1AddressBits = 0xFFFFF000U;
constexpr std::array<std::uint32_t, 6> kSelectedBc1FetchWords = {
    0x01000002U, 0x155CB052U, 0x000FE07FU, 0x00A80D10U, 0x000001C3U, 0x155C5A18U};

constexpr std::array<std::uint32_t, 45> kConsumerVertexShader = {
    // Raw words retained from the committed installed_prepared_control/control.cpp
    // specialization-0048 source pair.
    0x30052003U, 0x00001200U, 0xC2000000U, 0x00006005U, 0x200B1200U, 0x12000000U, 0x00000000U,
    0x100DC400U, 0x22000000U, 0x2DF82000U, 0x00393A88U, 0x0000000EU, 0x05F80000U, 0x40253E78U,
    0x0000040EU, 0xC8020001U, 0x00A7A700U, 0xAF020100U, 0xC8080001U, 0x00A7A700U, 0xAF020300U,
    0xC8020000U, 0x00A7A700U, 0xAF020200U, 0xB8410101U, 0x01A7A741U, 0x8F020008U, 0xC801803EU,
    0x00A7A700U, 0xAF010400U, 0xC802803EU, 0x00A7A700U, 0xAF010500U, 0xC804803EU, 0x00A7A700U,
    0xAF010600U, 0xC808803EU, 0x00A7A700U, 0xAF010700U, 0xC8038000U, 0x00C4C400U, 0xE2000000U,
    0x4E4A0007U, 0x95A570DEU, 0x30B52E1DU};

constexpr std::array<std::uint32_t, 12> kConsumerPixelShader = {
    // Raw words retained from the committed installed_prepared_control/control.cpp
    // specialization-0048 source pair. It samples texture fetch constant 0.
    0x00000000U, 0x2001C400U, 0x22000009U, 0x10080001U, 0x1F1FFFFBU, 0x00004000U,
    0xA8878000U, 0x00C0C000U, 0x02FFFF00U, 0x4E4A0006U, 0x95A570DEU, 0x30B52E1DU};

constexpr std::array<std::uint32_t, 36> kConsumerVertexConstants = {
    0x3F800000U, 0U,          0U, 0U, 0U, 0x3F800000U, 0U,          0U, 0U, 0U,
    0x3F800000U, 0U,          0U, 0U, 0U, 0x3F800000U, 0x3F800000U, 0U, 0U, 0U,
    0U,          0x3F800000U, 0U, 0U, 0U, 0U,          0x3F800000U, 0U, 0U, 0U,
    0U,          0x3F800000U, 0U, 0U, 0U, 0U};
constexpr std::array<std::uint32_t, 4> kConsumerPixelConstant0 = {0U, 0U, 0U, 0x3F800000U};
constexpr std::array<std::uint32_t, 4> kConsumerPixelConstant255 = {0x3DCCCCCDU, 0x3E4CCCCDU,
                                                                    0x3E99999AU, 0x3F800000U};

struct CompanionSnapshot {
  std::size_t begin_count = 0;
  std::size_t end_count = 0;
  std::vector<CompanionDrawRecord> original_draws;
  std::vector<CompanionDrawRecord> prepared_draws;
  std::vector<CompanionSwapRecord> swaps;
  std::vector<CompanionSwapRecord> prepared_swaps;
  std::uint64_t last_end_frame = 0;
  std::uint64_t last_end_submission = 0;
  bool last_end_succeeded = false;
};

class CompanionObservation final : public DrawCaptureFrameInputObserver {
 public:
  explicit CompanionObservation(std::shared_ptr<DrawCaptureFrameInputObserver> forwarded)
      : forwarded_(std::move(forwarded)) {}

  bool OnFrameBegin(const DrawCaptureFrameInputSwapView& boundary) override {
    {
      std::lock_guard lock(mutex_);
      ++begin_count_;
    }
    condition_.notify_all();
    return !forwarded_ || forwarded_->OnFrameBegin(boundary);
  }

  bool OnDraw(const DrawCaptureFrameInputDrawView& draw) override {
    {
      std::lock_guard lock(mutex_);
      original_draws_.push_back(MakeDrawRecord(draw, false));
    }
    condition_.notify_all();
    return !forwarded_ || forwarded_->OnDraw(draw);
  }

  bool OnCopy(const DrawCaptureFrameInputCopyView& copy) override {
    (void)copy;
    condition_.notify_all();
    return !forwarded_ || forwarded_->OnCopy(copy);
  }

  bool WantsPreparedDraws() const noexcept override { return true; }

  bool OnDrawPrepared(const DrawCaptureFrameInputDrawView& draw) override {
    {
      std::lock_guard lock(mutex_);
      prepared_draws_.push_back(MakeDrawRecord(draw, true));
    }
    condition_.notify_all();
    return !forwarded_ || !forwarded_->WantsPreparedDraws() || forwarded_->OnDrawPrepared(draw);
  }

  bool WantsPreparedCopies() const noexcept override { return false; }

  bool OnCopyPrepared(const DrawCaptureFrameInputCopyView& copy) override {
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

  bool WantsPreparedSwaps() const noexcept override {
    return forwarded_ && forwarded_->WantsPreparedSwaps();
  }

  bool OnSwapPrepared(const DrawCaptureFrameInputSwapView& swap) override {
    {
      std::lock_guard lock(mutex_);
      prepared_swaps_.push_back({swap.id, swap.frame, swap.submission, swap.frontbuffer_ptr,
                                 swap.frontbuffer_width, swap.frontbuffer_height,
                                 swap.original_succeeded});
    }
    condition_.notify_all();
    return !forwarded_ || !forwarded_->WantsPreparedSwaps() || forwarded_->OnSwapPrepared(swap);
  }

  bool OnFrameEnd(std::uint64_t frame, std::uint64_t submission, bool original_succeeded) override {
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

  bool WaitForCompletion(std::uint64_t terminal_address) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(20), [&] {
      const bool final_swap =
          std::any_of(swaps_.begin(), swaps_.end(), [&](const CompanionSwapRecord& swap) {
            // OnSwap is emitted before the surrounding original issue is
            // finalized; OnFrameEnd below carries the completed result.
            return swap.frontbuffer_ptr == terminal_address;
          });
      const bool final_prepared_swap = std::any_of(
          prepared_swaps_.begin(), prepared_swaps_.end(), [&](const CompanionSwapRecord& swap) {
            return swap.frontbuffer_ptr == terminal_address;
          });
      return (final_swap || final_prepared_swap) && end_count_ != 0 && last_end_succeeded_;
    });
  }

  CompanionSnapshot TakeSnapshot() const {
    std::lock_guard lock(mutex_);
    return {begin_count_,    end_count_,      original_draws_,      prepared_draws_,    swaps_,
            prepared_swaps_, last_end_frame_, last_end_submission_, last_end_succeeded_};
  }

 private:
  static CompanionTextureRecord MakeTextureRecord(
      const diagnostic::DrawCaptureFrameInputTextureView& texture) {
    CompanionTextureRecord result;
    const auto memory_version = texture.memory_version;
    result.memory_version_current_before = memory_version.IsCurrent();
    result.stage = texture.stage;
    result.shader_binding_index = texture.shader_binding_index;
    result.fetch_constant = texture.fetch_constant;
    result.fetch_words.assign(texture.fetch_words.begin(), texture.fetch_words.end());
    result.format = texture.format;
    result.endianness = texture.endianness;
    result.dimension = texture.dimension;
    result.width = texture.width;
    result.height = texture.height;
    result.depth = texture.depth;
    result.pitch = texture.pitch;
    result.mip_min_level = texture.mip_min_level;
    result.mip_max_level = texture.mip_max_level;
    result.tiled = texture.tiled;
    result.packed_mips = texture.packed_mips;
    result.base_address = texture.base_address;
    result.base_size = texture.base_size;
    result.mip_address = texture.mip_address;
    result.mip_size = texture.mip_size;
    result.sampler_value = texture.sampler_value;
    result.component_mapping = texture.component_mapping;
    result.swizzled_signs = texture.swizzled_signs;
    result.is_signed = texture.is_signed;
    result.required = texture.required;
    result.available = texture.available;
    result.gpu_written = texture.gpu_written;
    result.unavailable_reason = static_cast<std::uint32_t>(texture.unavailable_reason);
    result.memory_version_identity = memory_version.identity;
    result.memory_version_generation = memory_version.generation;
    result.memory_version_current_after = memory_version.IsCurrent();
    return result;
  }

  static CompanionDrawRecord MakeDrawRecord(const DrawCaptureFrameInputDrawView& draw,
                                            bool prepared) {
    CompanionDrawRecord result;
    result.id = draw.id;
    result.frame = draw.frame;
    result.submission = draw.submission;
    result.prepared = prepared;
    result.original_host_issued = draw.original_host_issued;
    result.cpu_data_complete = draw.cpu_data_complete;
    result.unavailable_reason = static_cast<std::uint32_t>(draw.unavailable_reason);
    result.execution_status = static_cast<std::uint32_t>(draw.execution_status);
    result.execution_reason = static_cast<std::uint32_t>(draw.execution_reason);
    result.primitive_type = draw.primitive_type;
    result.requested_index_count = draw.requested_index_count;
    result.vertex_shader_hash = draw.vertex_shader.hash;
    result.pixel_shader_hash = draw.pixel_shader.hash;
    result.registers.assign(draw.registers.begin(), draw.registers.end());
    result.vertex_shader_words.assign(draw.vertex_shader.words.begin(),
                                      draw.vertex_shader.words.end());
    result.pixel_shader_words.assign(draw.pixel_shader.words.begin(),
                                     draw.pixel_shader.words.end());
    result.textures.reserve(draw.textures.size());
    for (const auto& texture : draw.textures) {
      result.textures.push_back(MakeTextureRecord(texture));
    }
    return result;
  }

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t begin_count_ = 0;
  std::size_t end_count_ = 0;
  std::vector<CompanionDrawRecord> original_draws_;
  std::vector<CompanionDrawRecord> prepared_draws_;
  std::vector<CompanionSwapRecord> swaps_;
  std::vector<CompanionSwapRecord> prepared_swaps_;
  std::uint64_t last_end_frame_ = 0;
  std::uint64_t last_end_submission_ = 0;
  bool last_end_succeeded_ = false;
  std::shared_ptr<DrawCaptureFrameInputObserver> forwarded_;
};

class GuestAllocation final {
 public:
  GuestAllocation() = default;
  GuestAllocation(rex::memory::Memory& memory, std::uint32_t bytes, std::uint32_t alignment)
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
  std::uint32_t physical_address() const noexcept { return physical_address_; }
  std::uint8_t* physical_bytes() const noexcept {
    return memory_ ? memory_->TranslatePhysical<std::uint8_t*>(physical_address_) : nullptr;
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
  std::uint32_t virtual_address_ = 0;
  std::uint32_t physical_address_ = 0;
};

class PacketStream final {
 public:
  void Push(std::uint32_t value) { words_.push_back(value); }

  void Type0(std::uint32_t index, std::span<const std::uint32_t> values) {
    if (values.empty()) {
      return;
    }
    Push(rex::graphics::xenos::MakePacketType0(static_cast<std::uint16_t>(index),
                                               static_cast<std::uint16_t>(values.size())));
    words_.insert(words_.end(), values.begin(), values.end());
  }

  void Type0(std::uint32_t index, std::initializer_list<std::uint32_t> values) {
    Type0(index, std::span<const std::uint32_t>(values.begin(), values.size()));
  }

  void LoadShader(std::uint32_t physical_address, rex::graphics::xenos::ShaderType type,
                  std::uint32_t dword_count) {
    Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_IM_LOAD, 2));
    Push(physical_address | static_cast<std::uint32_t>(type));
    Push(dword_count);
  }

  void DrawDma(std::uint32_t initiator, std::uint32_t index_address, std::uint32_t index_word_count,
               rex::graphics::xenos::Endian index_endian) {
    Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_DRAW_INDX, 4));
    Push(0);
    Push(initiator);
    Push(index_address);
    Push(index_word_count | (static_cast<std::uint32_t>(index_endian) << 30));
  }

  void Swap(std::uint32_t frontbuffer, std::uint32_t width, std::uint32_t height) {
    Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_XE_SWAP, 4));
    Push(rex::graphics::xenos::kSwapSignature);
    Push(frontbuffer);
    Push(width);
    Push(height);
  }

  std::span<const std::uint32_t> words() const noexcept { return words_; }

 private:
  std::vector<std::uint32_t> words_;
};

void WriteShaderWords(GuestAllocation& allocation, std::span<const std::uint32_t> words) {
  auto* bytes = allocation.physical_bytes();
  for (std::size_t index = 0; index < words.size(); ++index) {
    rex::memory::store_and_swap<std::uint32_t>(bytes + index * sizeof(std::uint32_t), words[index]);
  }
}

void WritePacketWords(rex::memory::Memory& memory, std::uint32_t ring_address,
                      std::span<const std::uint32_t> words) {
  auto* ring = memory.TranslatePhysical<std::uint8_t*>(ring_address);
  for (std::size_t index = 0; index < words.size(); ++index) {
    rex::memory::store_and_swap<std::uint32_t>(ring + index * sizeof(std::uint32_t), words[index]);
  }
}

void PublishWritePointer(rex::memory::Memory& memory, std::uint32_t word_count) {
  auto* write_pointer = memory.TranslateVirtual<volatile std::uint32_t*>(kWptrAddress);
  *write_pointer = rex::byte_swap(word_count);
}

std::uint32_t ReadWriteback(rex::memory::Memory& memory, std::uint32_t writeback_address) {
  return rex::memory::load_and_swap<std::uint32_t>(memory.TranslatePhysical(writeback_address));
}

bool WaitForWriteback(rex::memory::Memory& memory, std::uint32_t writeback_address,
                      std::uint32_t expected_word_count) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ReadWriteback(memory, writeback_address) == expected_word_count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

bool Require(bool condition, std::string_view message, std::string& error) {
  if (condition) {
    return true;
  }
  error = std::string(message);
  std::cerr << "installed_prepared_companion: " << error << '\n';
  return false;
}

std::uint32_t AlignToPage(std::uint32_t bytes) {
  if (bytes == 0 || bytes > std::numeric_limits<std::uint32_t>::max() - 0xFFFU) {
    return 0;
  }
  return (bytes + 0xFFFU) & ~0xFFFU;
}

std::uint32_t GuestTextureExtent(std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                                 bool tiled) {
  const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
      rex::graphics::xenos::DataDimension::k2DOrStacked, pitch / 32U, width, height, 1U, tiled,
      rex::graphics::xenos::TextureFormat::k_8_8_8_8, false, true, 0U);
  return layout.base.level_data_extent_bytes;
}

std::array<std::uint32_t, 6> VertexFetch(std::uint32_t physical_address, std::uint32_t bytes) {
  rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch{};
  fetch.type = rex::graphics::xenos::FetchConstantType::kVertex;
  fetch.address = physical_address / sizeof(std::uint32_t);
  fetch.endian = rex::graphics::xenos::Endian::k8in32;
  fetch.size = bytes / sizeof(std::uint32_t);
  return {fetch.dword_0, fetch.dword_1, 0, 0, 0, 0};
}

std::array<std::uint32_t, 6> TextureFetch(std::uint32_t physical_address, std::uint32_t width,
                                          std::uint32_t height, std::uint32_t pitch, bool tiled,
                                          rex::graphics::xenos::TextureFormat format,
                                          rex::graphics::xenos::Endian endianness,
                                          std::uint32_t swizzle,
                                          rex::graphics::xenos::TextureSign sign) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = rex::graphics::xenos::FetchConstantType::kTexture;
  fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w = sign;
  fetch.pitch = pitch / 32U;
  fetch.format = format;
  fetch.endianness = endianness;
  fetch.base_address = physical_address >> 12;
  fetch.size_2d.width = width - 1U;
  fetch.size_2d.height = height - 1U;
  fetch.tiled = tiled;
  fetch.swizzle = swizzle;
  fetch.mag_filter = rex::graphics::xenos::TextureFilter::kLinear;
  fetch.min_filter = rex::graphics::xenos::TextureFilter::kLinear;
  fetch.mip_filter = rex::graphics::xenos::TextureFilter::kPoint;
  fetch.dimension = rex::graphics::xenos::DataDimension::k2DOrStacked;
  return {fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5};
}

std::array<std::uint32_t, 6> Bc1Fetch(std::uint32_t base_address, std::uint32_t mip_address) {
  // Rebase only the address fields; keep the selected BC1 descriptor bits intact.
  auto words = kSelectedBc1FetchWords;
  words[1] = (words[1] & ~kBc1AddressBits) | (base_address & kBc1AddressBits);
  words[5] = (words[5] & ~kBc1AddressBits) | (mip_address & kBc1AddressBits);
  return words;
}

bool HasSelectedBc1FetchWords(std::span<const std::uint32_t> words) {
  if (words.size() != kSelectedBc1FetchWords.size()) {
    return false;
  }
  for (std::size_t index = 0; index < words.size(); ++index) {
    const std::uint32_t address_mask = (index == 1U || index == 5U) ? kBc1AddressBits : 0U;
    if ((words[index] & ~address_mask) != (kSelectedBc1FetchWords[index] & ~address_mask)) {
      return false;
    }
  }
  return true;
}

bool HasSelectedBc1FetchMetadata(std::span<const std::uint32_t> words) {
  if (!HasSelectedBc1FetchWords(words)) {
    return false;
  }
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, words.data(), sizeof(fetch));
  return fetch.type == rex::graphics::xenos::FetchConstantType::kTexture && fetch.pitch == 4U &&
         !fetch.tiled && !fetch.stacked &&
         fetch.format == rex::graphics::xenos::TextureFormat::k_DXT1 &&
         fetch.endianness == rex::graphics::xenos::Endian::k8in16 &&
         fetch.size_2d.width + 1U == kBc1Width && fetch.size_2d.height + 1U == kBc1Height &&
         fetch.size_2d.stack_depth == 0U && fetch.swizzle == 0x688U && fetch.mip_min_level == 0U &&
         fetch.mip_max_level == 7U &&
         fetch.dimension == rex::graphics::xenos::DataDimension::k2DOrStacked && fetch.packed_mips;
}

std::vector<std::uint8_t> ConsumerGeometry() {
  constexpr std::array<std::array<float, 14>, 3> values = {{
      {{-1.0F, -1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}},
      {{-1.0F, 3.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}},
      {{3.0F, -1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}},
  }};
  std::vector<std::uint8_t> bytes;
  bytes.reserve(values.size() * values.front().size() * sizeof(float));
  for (const auto& value : values) {
    for (const float component : value) {
      std::uint32_t bits = 0;
      std::memcpy(&bits, &component, sizeof(bits));
      bytes.push_back(static_cast<std::uint8_t>(bits >> 24));
      bytes.push_back(static_cast<std::uint8_t>(bits >> 16));
      bytes.push_back(static_cast<std::uint8_t>(bits >> 8));
      bytes.push_back(static_cast<std::uint8_t>(bits));
    }
  }
  return bytes;
}

std::vector<std::uint8_t> ResolveGeometry() {
  constexpr std::array<float, 6> values = {-0.5F, -0.5F, 1279.5F, -0.5F, 1279.5F, 719.5F};
  std::vector<std::uint8_t> bytes;
  bytes.reserve(values.size() * sizeof(float));
  for (const float value : values) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 24));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 16));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 8));
    bytes.push_back(static_cast<std::uint8_t>(bits));
  }
  return bytes;
}

std::string CompanionMode() {
  const char* value = std::getenv("REREVVED_PREPARED_COMPANION_CASE");
  if (!value || *value == '\0') {
    return "unsigned";
  }
  const std::string mode(value);
  if (mode == "unsigned" || mode == "signed" || mode == "mixed" || mode == "bc1") {
    return mode;
  }
  return {};
}

bool HasExpectedCompanionOrder(const CompanionDrawRecord& draw, std::string_view mode) {
  if (draw.textures.size() != 2U) {
    return false;
  }
  const auto& unsigned_view = draw.textures[0];
  const auto& signed_view = draw.textures[1];
  const bool ordered_shape =
      unsigned_view.stage == 1U && signed_view.stage == 1U &&
      unsigned_view.shader_binding_index == 0U && signed_view.shader_binding_index == 1U &&
      unsigned_view.fetch_constant == 0U && signed_view.fetch_constant == 0U &&
      unsigned_view.fetch_words == signed_view.fetch_words;
  if (!ordered_shape) {
    return false;
  }
  const bool available_pair = unsigned_view.available && signed_view.available &&
                              unsigned_view.unavailable_reason == 0U &&
                              signed_view.unavailable_reason == 0U;
  if (mode == "unsigned") {
    return available_pair && !unsigned_view.is_signed && unsigned_view.required &&
           signed_view.is_signed && !signed_view.required;
  }
  if (mode == "signed") {
    return available_pair && unsigned_view.is_signed == false && !unsigned_view.required &&
           signed_view.is_signed && signed_view.required;
  }
  if (mode == "mixed") {
    return available_pair && !unsigned_view.is_signed && unsigned_view.required &&
           signed_view.is_signed && signed_view.required;
  }
  const auto unavailable_reason =
      static_cast<std::uint32_t>(diagnostic::DrawCaptureReason::kTextureSource);
  return !unsigned_view.is_signed && unsigned_view.required && !unsigned_view.available &&
         signed_view.is_signed && !signed_view.required && !signed_view.available &&
         unsigned_view.unavailable_reason == unavailable_reason &&
         signed_view.unavailable_reason == unavailable_reason && unsigned_view.width == kBc1Width &&
         unsigned_view.height == kBc1Height &&
         HasSelectedBc1FetchMetadata(unsigned_view.fetch_words);
}

}  // namespace

int RunCompanionControl(
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& forwarded,
    CompanionControlResult& result, std::string& error) {
  result = {};
  error.clear();
  result.mode = CompanionMode();
  if (!Require(!result.mode.empty(),
               "REREVVED_PREPARED_COMPANION_CASE must be unsigned, signed, mixed, or bc1", error)) {
    return 1;
  }
#if !defined(_WIN32)
  error = "installed companion control requires Windows D3D12";
  return 1;
#else
  struct LoggingScope {
    LoggingScope() {
      rex::LogConfig config;
      config.log_to_console = true;
      rex::InitLogging(config);
    }
    ~LoggingScope() { rex::ShutdownLogging(); }
  } logging;

  auto graphics = rex::system::LoadGpuPlugin("xenos", "d3d12");
  if (!Require(graphics != nullptr, "xenos d3d12 plugin was not loaded", error)) {
    return 1;
  }
  rex::Runtime runtime(std::filesystem::path{});
  if (!Require(runtime.Setup(rex::RuntimeConfig{
                   .kernel_init = rex::kernel::InitializeKernel,
                   .tool_mode = true,
               }) == static_cast<rex::X_STATUS>(0),
               "runtime setup failed", error)) {
    return 1;
  }
  if (!Require(rex::cvar::SetFlagByName("readback_resolve", "none"),
               "readback_resolve=none was rejected", error) ||
      !Require(rex::cvar::SetFlagByName("async_shader_compilation", "false"),
               "synchronous shader compilation was rejected", error) ||
      !Require(graphics->SetupPresentation(nullptr) == static_cast<rex::X_STATUS>(0),
               "offscreen D3D12 presenter setup failed", error) ||
      !Require(graphics->SetupGuestGpu(runtime.function_dispatcher(), runtime.kernel_state()) ==
                   static_cast<rex::X_STATUS>(0),
               "guest GPU setup failed", error)) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  auto mailbox = diagnostic::GetDrawCaptureMailbox();
  auto observation = std::make_shared<CompanionObservation>(forwarded);
  const auto observer_token = mailbox->RegisterFrameInputObserver(observation);
  if (!Require(static_cast<bool>(observer_token), "mailbox observer registration failed", error)) {
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  rex::memory::Memory& memory = *runtime.memory();
  const std::uint32_t target_extent =
      GuestTextureExtent(kTargetWidth, kTargetHeight, kTargetWidth, true);
  const bool bc1_case = result.mode == "bc1";
  const std::uint32_t source_bytes = bc1_case ? kBc1BaseBytes : kSourceBytes;
  const std::uint32_t source_width = bc1_case ? kBc1Width : kSourceWidth;
  const std::uint32_t source_height = bc1_case ? kBc1Height : kSourceHeight;
  const std::uint32_t source_pitch = bc1_case ? 4U : kSourcePitch;
  const std::uint32_t source_allocation_bytes = AlignToPage(source_bytes);
  const std::uint32_t mip_allocation_bytes = bc1_case ? AlignToPage(kBc1MipBytes) : 0x1000U;
  if (!Require(target_extent != 0U && source_allocation_bytes != 0U, "texture layout was empty",
               error)) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  GuestAllocation ring(memory, kRingWordCount * sizeof(std::uint32_t), 0x1000U);
  GuestAllocation writeback(memory, 0x1000U, 0x1000U);
  GuestAllocation source(memory, source_allocation_bytes, 0x1000U);
  GuestAllocation mip(memory, mip_allocation_bytes, 0x1000U);
  GuestAllocation terminal(memory, AlignToPage(target_extent), 0x1000U);
  GuestAllocation geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation resolve_geometry(memory, 0x1000U, 0x1000U);
  GuestAllocation index_data(memory, 0x1000U, 0x1000U);
  GuestAllocation vertex_shader(
      memory, static_cast<std::uint32_t>(kConsumerVertexShader.size() * 4U), 0x1000U);
  GuestAllocation pixel_shader(memory, static_cast<std::uint32_t>(kConsumerPixelShader.size() * 4U),
                               0x1000U);
  const auto reset_allocations = [&] {
    source.Reset();
    mip.Reset();
    terminal.Reset();
    geometry.Reset();
    resolve_geometry.Reset();
    index_data.Reset();
    vertex_shader.Reset();
    pixel_shader.Reset();
    writeback.Reset();
    ring.Reset();
  };
  const bool allocations_ok = ring && writeback && source && terminal && geometry &&
                              resolve_geometry && index_data && vertex_shader && pixel_shader &&
                              (!bc1_case || mip);
  if (!Require(allocations_ok, "guest physical test allocations failed", error)) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    reset_allocations();
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  std::memset(source.physical_bytes(), 0, source_allocation_bytes);
  for (std::uint32_t offset = 0; offset < source_bytes; offset += 4U) {
    source.physical_bytes()[offset + 0U] = static_cast<std::uint8_t>(offset / 4U);
    source.physical_bytes()[offset + 1U] = 0x40U;
    source.physical_bytes()[offset + 2U] = 0x80U;
    source.physical_bytes()[offset + 3U] = 0xFFU;
  }
  if (mip) {
    std::memset(mip.physical_bytes(), 0, mip_allocation_bytes);
  }
  std::memset(terminal.physical_bytes(), 0, AlignToPage(target_extent));
  const auto geometry_bytes = ConsumerGeometry();
  const auto resolve_geometry_bytes = ResolveGeometry();
  std::memcpy(geometry.physical_bytes(), geometry_bytes.data(), geometry_bytes.size());
  std::memcpy(resolve_geometry.physical_bytes(), resolve_geometry_bytes.data(),
              resolve_geometry_bytes.size());
  const std::array<std::uint8_t, 6> indices = {0, 0, 0, 1, 0, 2};
  std::memcpy(index_data.physical_bytes(), indices.data(), indices.size());
  WriteShaderWords(vertex_shader, kConsumerVertexShader);
  WriteShaderWords(pixel_shader, kConsumerPixelShader);

  graphics->InitializeRingBuffer(ring.physical_address(), kRingSizeLog2);
  graphics->EnableReadPointerWriteBack(writeback.physical_address(), kWritebackBlockSizeLog2);

  const auto terminal_texture = TextureFetch(
      terminal.physical_address(), kTargetWidth, kTargetHeight, kTargetWidth, true,
      rex::graphics::xenos::TextureFormat::k_8_8_8_8, rex::graphics::xenos::Endian::kNone,
      rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
      rex::graphics::xenos::TextureSign::kUnsigned);
  std::array<std::uint32_t, 6> source_texture{};
  if (bc1_case) {
    source_texture = Bc1Fetch(source.physical_address(), mip.physical_address());
  } else {
    auto sign = rex::graphics::xenos::TextureSign::kUnsigned;
    if (result.mode == "signed") {
      sign = rex::graphics::xenos::TextureSign::kSigned;
    }
    source_texture = TextureFetch(
        source.physical_address(), source_width, source_height, source_pitch, false,
        rex::graphics::xenos::TextureFormat::k_8_8_8_8, rex::graphics::xenos::Endian::kNone,
        rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, sign);
    if (result.mode == "mixed") {
      rex::graphics::xenos::xe_gpu_texture_fetch_t mixed{};
      std::memcpy(&mixed, source_texture.data(), sizeof(mixed));
      mixed.sign_x = rex::graphics::xenos::TextureSign::kSigned;
      mixed.sign_y = rex::graphics::xenos::TextureSign::kUnsigned;
      mixed.sign_z = rex::graphics::xenos::TextureSign::kUnsignedBiased;
      mixed.sign_w = rex::graphics::xenos::TextureSign::kSigned;
      source_texture = {mixed.dword_0, mixed.dword_1, mixed.dword_2,
                        mixed.dword_3, mixed.dword_4, mixed.dword_5};
    }
  }
  result.source_width = source_width;
  result.source_height = source_height;
  result.source_pitch = source_pitch;
  result.source_format =
      static_cast<std::uint32_t>(bc1_case ? rex::graphics::xenos::TextureFormat::k_DXT1
                                          : rex::graphics::xenos::TextureFormat::k_8_8_8_8);
  result.source_endianness = static_cast<std::uint32_t>(
      bc1_case ? rex::graphics::xenos::Endian::k8in16 : rex::graphics::xenos::Endian::kNone);
  result.source_swizzle = bc1_case ? 0x688U : rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  result.source_mip_min_level = bc1_case ? 0U : 0U;
  result.source_mip_max_level = bc1_case ? 7U : 0U;
  result.source_tiled = false;
  result.source_packed_mips = bc1_case;
  result.source_fetch_words.assign(source_texture.begin(), source_texture.end());

  const auto consumer_fetch =
      VertexFetch(geometry.physical_address(), static_cast<std::uint32_t>(geometry_bytes.size()));
  const auto resolve_fetch = VertexFetch(resolve_geometry.physical_address(),
                                         static_cast<std::uint32_t>(resolve_geometry_bytes.size()));
  PacketStream stream;
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, terminal_texture);
  stream.Swap(terminal.physical_address(), kTargetWidth, kTargetHeight);
  stream.LoadShader(vertex_shader.physical_address(), rex::graphics::xenos::ShaderType::kVertex,
                    static_cast<std::uint32_t>(kConsumerVertexShader.size()));
  stream.LoadShader(pixel_shader.physical_address(), rex::graphics::xenos::ShaderType::kPixel,
                    static_cast<std::uint32_t>(kConsumerPixelShader.size()));
  stream.Type0(0x2000U, {0x14000500U, 0U, 0x530U});
  stream.Type0(0x200FU, {0x20002000U});
  stream.Type0(0x2080U, {0U, 0U});
  stream.Type0(0x2082U, {kTargetWidth | (kTargetHeight << 16)});
  stream.Type0(0x2100U, {2U});
  stream.Type0(0x2104U, {0xFU});
  stream.Type0(0x210FU,
               {std::bit_cast<std::uint32_t>(640.0F), std::bit_cast<std::uint32_t>(640.0F),
                std::bit_cast<std::uint32_t>(-360.0F), std::bit_cast<std::uint32_t>(360.0F)});
  stream.Type0(0x2180U, {0x10010002U});
  stream.Type0(0x2181U, {4U});
  stream.Type0(0x2182U, {1U});
  stream.Type0(0x2201U, {0x00010006U});
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
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, source_texture);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U, consumer_fetch);
  stream.Type0(
      0x21FCU,
      {static_cast<std::uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
       (static_cast<std::uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6) | (3U << 16)});
  stream.DrawDma(0x00030004U, index_data.physical_address(), 3U,
                 rex::graphics::xenos::Endian::k8in16);
  stream.Type0(0x2000U, {0x14000500U, 0U, 0x530U});
  stream.Type0(0x200FU, {0x20002000U});
  stream.Type0(0x2080U, {0U, 0U, kTargetWidth | (kTargetHeight << 16)});
  stream.Type0(0x2205U, {0x10000U});
  stream.Type0(0x2208U, {6U});
  stream.Type0(0x2318U, {0x00100000U});
  stream.Type0(0x2319U, {terminal.physical_address()});
  stream.Type0(0x231AU, {0x02D00500U});
  stream.Type0(0x231BU, {0x300U});
  stream.Type0(0x231CU, {0xFFFU});
  stream.Type0(0x231DU, {0xFFFFFF00U});
  stream.Type0(0x231EU, {0U, 0U});
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, resolve_fetch);
  stream.Type0(0x21FCU,
               {static_cast<std::uint32_t>(rex::graphics::xenos::PrimitiveType::kRectangleList) |
                (static_cast<std::uint32_t>(rex::graphics::xenos::SourceSelect::kAutoIndex) << 6) |
                (3U << 16)});
  stream.Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_DRAW_INDX, 2));
  stream.Push(0U);
  stream.Push(0x00030088U);
  stream.Type0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, terminal_texture);
  stream.Swap(terminal.physical_address(), kTargetWidth, kTargetHeight);
  if (!Require(stream.words().size() < kRingWordCount, "PM4 stream exceeds ring capacity", error)) {
    mailbox->UnregisterFrameInputObserver(observer_token);
    reset_allocations();
    graphics->Shutdown();
    runtime.Shutdown();
    return 1;
  }

  WritePacketWords(memory, ring.physical_address(), stream.words());
  PublishWritePointer(memory, static_cast<std::uint32_t>(stream.words().size()));
  result.progressed = WaitForWriteback(memory, writeback.physical_address(),
                                       static_cast<std::uint32_t>(stream.words().size()));
  result.completion_observed =
      result.progressed && observation->WaitForCompletion(terminal.physical_address());

  graphics->Shutdown();
  mailbox->UnregisterFrameInputObserver(observer_token);
  // All borrowed spans were copied by CompanionObservation before returning
  // from each callback. Take this snapshot only after shutdown to exercise the
  // promised lifetime of the saved descriptor and shader banks.
  const auto observed = observation->TakeSnapshot();
  const std::uint32_t terminal_address = terminal.physical_address();
  reset_allocations();
  runtime.Shutdown();

  result.original_draws = observed.original_draws;
  result.prepared_draws = observed.prepared_draws;
  result.original_swaps = observed.swaps;
  result.prepared_swaps = observed.prepared_swaps;
  const auto issued = [](const CompanionDrawRecord& draw) {
    return draw.original_host_issued && draw.execution_status == 1U && draw.execution_reason == 0U;
  };
  const bool matching_draw =
      observed.original_draws.size() == 1U && observed.prepared_draws.size() == 1U &&
      observed.original_draws[0].id == observed.prepared_draws[0].id &&
      observed.original_draws[0].frame == observed.prepared_draws[0].frame &&
      observed.original_draws[0].submission == observed.prepared_draws[0].submission &&
      issued(observed.original_draws[0]);
  const bool complete_prepared =
      observed.prepared_draws.size() == 1U && observed.prepared_draws[0].cpu_data_complete;
  const auto valid_swap_sequence = [&](const std::vector<CompanionSwapRecord>& swaps) {
    return swaps.size() == 2U && swaps.front().frontbuffer_ptr == terminal_address &&
           swaps.back().frontbuffer_ptr == terminal_address &&
           swaps.front().width == kTargetWidth && swaps.front().height == kTargetHeight &&
           swaps.back().width == kTargetWidth && swaps.back().height == kTargetHeight;
  };
  const bool valid_swaps =
      valid_swap_sequence(observed.swaps) &&
      (observed.prepared_swaps.empty() || valid_swap_sequence(observed.prepared_swaps));
  const bool expected_companions =
      observed.prepared_draws.size() == 1U &&
      HasExpectedCompanionOrder(observed.prepared_draws[0], result.mode);
  if (result.mode == "unsigned") {
    if (!Require(
            matching_draw && complete_prepared && valid_swaps && expected_companions,
            "unsigned companion draw was not complete, ordered, or matched to an issued original",
            error)) {
      return 1;
    }
  }
  if (result.mode == "bc1") {
    if (!Require(matching_draw && valid_swaps && expected_companions,
                 "BC1 boundary descriptor was not ordered or preserved as unavailable", error)) {
      return 1;
    }
  }
  if (!result.progressed) {
    error = "PM4 writeback did not complete";
  } else if (!result.completion_observed) {
    error = "terminal swap and frame completion were not observed";
  } else if (!matching_draw) {
    error = "Prepared and original draw identities did not match";
  } else if (!valid_swaps) {
    error = "initial/final swap boundaries did not match the terminal target";
  }
  return error.empty() ? 0 : 1;
#endif
}

}  // namespace rex::graphics::testing
