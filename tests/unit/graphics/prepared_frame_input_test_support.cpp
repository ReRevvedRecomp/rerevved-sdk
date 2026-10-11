#include "prepared_frame_input_test_support.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/crypto/sha256.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <rex/system/xmemory.h>
#include <xxhash.h>

#include "graphics/d3d12/frame_input_draw_builder.h"
#include "graphics/d3d12/frame_input_copy_plan.h"
#include "graphics/d3d12/frame_input_sampler.h"
#include "graphics/frame_input_buffer_versions.h"
#include "graphics/frame_input_native_sources.h"

namespace rex::graphics::testing {
using namespace d3d12;
namespace {

using diagnostic::DrawCaptureFrameInputDrawView;
using diagnostic::DrawCaptureFrameInputIndexView;
using diagnostic::DrawCaptureFrameInputTextureView;
using diagnostic::DrawCaptureFrameInputVertexFetchView;
using diagnostic::DrawCaptureGuestMemoryReader;
using diagnostic::DrawCaptureGuestMemoryVersion;

class TestSharedMemory final : public SharedMemory {
 public:
  explicit TestSharedMemory(rex::memory::Memory& memory) : SharedMemory(memory) {
    InitializeCommon();
  }

 private:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>&) override { return false; }
};

class TestTextureCache final : public TextureCache {
 public:
  TestTextureCache(const RegisterFile& registers, SharedMemory& memory)
      : TextureCache(registers, memory, 1, 1) {}

 private:
  uint32_t GetHostFormatSwizzle(TextureKey) const override {
    return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension) const override { return 8192; }
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension) const override { return 2048; }
  std::unique_ptr<Texture> CreateTexture(TextureKey) override { return nullptr; }
  bool LoadTextureDataFromResidentMemoryImpl(Texture&, bool, bool) override { return false; }
};

std::vector<uint32_t> VertexShaderWords() {
  // Pinned special-0010/0048 raw guest words. The title catalog maps this
  // exact pair to generated special-0048 DXIL. Fetch 95 carries position and
  // texture coordinates with the 56-byte source stride.
  return {
      0x30052003, 0x00001200, 0xC2000000, 0x00006005, 0x200B1200, 0x12000000, 0x00000000,
      0x100DC400, 0x22000000, 0x2DF82000, 0x00393A88, 0x0000000E, 0x05F80000, 0x40253E78,
      0x0000040E, 0xC8020001, 0x00A7A700, 0xAF020100, 0xC8080001, 0x00A7A700, 0xAF020300,
      0xC8020000, 0x00A7A700, 0xAF020200, 0xB8410101, 0x01A7A741, 0x8F020008, 0xC801803E,
      0x00A7A700, 0xAF010400, 0xC802803E, 0x00A7A700, 0xAF010500, 0xC804803E, 0x00A7A700,
      0xAF010600, 0xC808803E, 0x00A7A700, 0xAF010700, 0xC8038000, 0x00C4C400, 0xE2000000,
      0x4E4A0007, 0x95A570DE, 0x30B52E1D,
  };
}

std::vector<uint32_t> PixelShaderWords() {
  // Pinned special-0010/0048 raw pixel words. The sole effective demand is
  // the unsigned 2D fetch at texture constant 0.
  return {
      0x00000000, 0x2001C400, 0x22000009, 0x10080001, 0x1F1FFFFB, 0x00004000,
      0xA8878000, 0x00C0C000, 0x02FFFF00, 0x4E4A0006, 0x95A570DE, 0x30B52E1D,
  };
}

uint64_t ShaderHash(std::span<const uint32_t> words) {
  // PipelineCache hashes guest bytes before Shader normalizes word order.
  std::vector<uint32_t> guest_words(words.begin(), words.end());
  for (auto& word : guest_words)
    word = std::byteswap(word);
  return XXH3_64bits(guest_words.data(), guest_words.size() * sizeof(uint32_t));
}

std::string WordDigest(std::span<const uint32_t> words) {
  return rex::crypto::sha256(
      std::string_view(reinterpret_cast<const char*>(words.data()), words.size_bytes()));
}

void AppendBigEndianFloat(std::vector<uint8_t>& bytes, float value) {
  const uint32_t word = std::bit_cast<uint32_t>(value);
  bytes.push_back(static_cast<uint8_t>(word >> 24));
  bytes.push_back(static_cast<uint8_t>(word >> 16));
  bytes.push_back(static_cast<uint8_t>(word >> 8));
  bytes.push_back(static_cast<uint8_t>(word));
}

constexpr uint32_t kArenaGuestBase = 0xA0100000U;
constexpr uint32_t kArenaSize = 64U * 1024U * 1024U;
constexpr uint32_t kTextureOffset = 0x00000000U;
constexpr uint32_t kVertexOffset = 0x00700000U;
constexpr uint32_t kIndexOffset = 0x00710000U;
constexpr uint32_t kClearGeometryOffset = 0x00720000U;
constexpr uint32_t kResolveGeometryOffset = 0x00730000U;
constexpr uint32_t kFullResolveGeometryOffset = 0x00740000U;

class PhysicalArena final {
 public:
  explicit PhysicalArena(rex::memory::Memory& memory) {
    heap_ = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(kArenaGuestBase));
    if (!heap_ || !heap_->AllocFixed(
                      kArenaGuestBase, kArenaSize, heap_->page_size(),
                      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
      throw std::runtime_error("prepared support guest physical arena failed to allocate");
    }
    physical_base_ = memory.GetPhysicalAddress(kArenaGuestBase);
  }

  ~PhysicalArena() {
    if (heap_) {
      (void)heap_->Release(kArenaGuestBase, nullptr);
    }
  }

  PhysicalArena(const PhysicalArena&) = delete;
  PhysicalArena& operator=(const PhysicalArena&) = delete;

  uint32_t address(uint32_t offset) const {
    if (offset >= kArenaSize) {
      throw std::out_of_range("prepared support arena offset");
    }
    return physical_base_ + offset;
  }

  bool Contains(uint32_t address, size_t size) const noexcept {
    return address >= physical_base_ && uint64_t(address - physical_base_) + size <= kArenaSize;
  }

 private:
  rex::memory::BaseHeap* heap_ = nullptr;
  uint32_t physical_base_ = 0;
};

std::vector<uint32_t> ClearVertexShaderWords() {
  return {
      0x30052003, 0x00001200, 0xC4000000, 0x00001005, 0x00001200, 0xC2000000, 0x00001006,
      0x10071200, 0x22000000, 0x30081000, 0x00393A88, 0x00000007, 0x00080000, 0x40263688,
      0x00000300, 0xC80F8000, 0x00000000, 0xE2000000, 0xC80F803E, 0x00000000, 0xE2010100,
      0xC8000000, 0x00000000, 0xE2000000, 0x00000000, 0x00000000, 0x00000000,
  };
}

std::vector<uint32_t> ClearPixelShaderWords() {
  return {
      0x00000000, 0x1001C400, 0x22000000, 0xC80F8000, 0x00000000,
      0xE2000000, 0x00000000, 0x00000000, 0x00000000,
  };
}

std::vector<uint8_t> ClearGeometryBytes() {
  std::vector<uint8_t> bytes;
  for (const auto value :
       std::array<std::array<float, 7>, 3>{{{-0.5F, -0.5F, 1.0F, 0.1F, 0.2F, 0.3F, 1.0F},
                                            {255.5F, -0.5F, 1.0F, 0.1F, 0.2F, 0.3F, 1.0F},
                                            {255.5F, 31.5F, 1.0F, 0.1F, 0.2F, 0.3F, 1.0F}}}) {
    for (const auto component : value) {
      AppendBigEndianFloat(bytes, component);
    }
  }
  return bytes;
}

std::vector<uint8_t> ResolveGeometryBytes() {
  std::vector<uint8_t> bytes;
  for (const auto value : std::array<float, 6>{-0.5F, -0.5F, 255.5F, -0.5F, 255.5F, 31.5F}) {
    AppendBigEndianFloat(bytes, value);
  }
  return bytes;
}

std::vector<uint8_t> FullResolveGeometryBytes() {
  std::vector<uint8_t> bytes;
  for (const auto value : std::array<float, 6>{-0.5F, -0.5F, 1279.5F, -0.5F, 1279.5F, 719.5F}) {
    AppendBigEndianFloat(bytes, value);
  }
  return bytes;
}

rex::memory::Memory& InitializeMemory(rex::memory::Memory& memory) {
  rex::InitLogging();
  if (!memory.Initialize())
    throw std::runtime_error("prepared support guest memory failed to initialize");
  return memory;
}

}  // namespace

struct PreparedFrameInputTestSupport::Impl {
  rex::memory::Memory memory;
  TestSharedMemory shared;
  PhysicalArena arena;
  FrameInputBufferVersions versions;
  FrameInputNativeSources native_sources;
  RegisterFile registers;
  RegisterFile clear_registers;
  RegisterFile resolve_registers;
  std::unique_ptr<TestTextureCache> texture_cache;
  std::vector<uint32_t> vertex_words;
  std::vector<uint32_t> pixel_words;
  std::vector<uint32_t> clear_vertex_words;
  std::vector<uint32_t> clear_pixel_words;
  std::unique_ptr<DxbcShader> vertex_shader;
  std::unique_ptr<DxbcShader> pixel_shader;
  std::unique_ptr<DxbcShader> clear_vertex_shader;
  std::unique_ptr<DxbcShader> clear_pixel_shader;
  rex::string::StringBuffer disassembly;
  std::vector<uint8_t> vertex_bytes;
  std::vector<uint8_t> index_bytes;
  std::vector<uint8_t> clear_geometry_bytes;
  std::vector<uint8_t> resolve_geometry_bytes;
  std::vector<uint8_t> full_resolve_geometry_bytes;
  std::string vertex_digest;
  std::string pixel_digest;
  DrawCaptureFrameInputDrawView draw;
  DrawCaptureFrameInputDrawView clear_draw;
  DrawCaptureFrameInputTextureView texture_metadata;
  FrameInputCopyGeometry resolve_geometry;
  FrameInputDrawBuilderResult result;
  FrameInputDrawBuilderResult clear_result;
  std::size_t sampler_calls = 0;
  std::size_t readable_calls = 0;
  std::size_t version_calls = 0;
  uint32_t texture_base_size = 0;
  bool native_source_armed = false;
  bool initialized = false;

  uint8_t* arena_address(uint32_t offset) {
    return memory.TranslatePhysical(arena.address(offset));
  }

  bool Readable(uint32_t address, uint32_t size) {
    if (!size) {
      return true;
    }
    auto* heap = memory.GetPhysicalHeap();
    return arena.Contains(address, size) && heap &&
           heap->QueryRangeAccess(address, address + size - 1) !=
               rex::memory::PageAccess::kNoAccess &&
           !shared.IsRangeWrittenByGpu(address, size) &&
           memory.TranslatePhysical(address) != nullptr;
  }

  bool Dirty(uint32_t address, uint32_t size) const {
    (void)address;
    (void)size;
    return false;
  }

  bool PrepareTextureSourceMetadata() {
    const auto fetch = registers.GetTextureFetch(0);
    return texture_cache->PrepareFrameInputTexture(fetch, texture_metadata);
  }

  Impl()
      : shared(InitializeMemory(memory)),
        arena(memory),
        versions(memory, shared),
        native_sources(memory, shared),
        initialized(true) {
    texture_cache = std::make_unique<TestTextureCache>(registers, shared);
    vertex_words = VertexShaderWords();
    pixel_words = PixelShaderWords();
    clear_vertex_words = ClearVertexShaderWords();
    clear_pixel_words = ClearPixelShaderWords();
    vertex_digest = WordDigest(vertex_words);
    pixel_digest = WordDigest(pixel_words);
    vertex_shader =
        std::make_unique<DxbcShader>(xenos::ShaderType::kVertex, ShaderHash(vertex_words),
                                     vertex_words.data(), vertex_words.size(), std::endian::native);
    pixel_shader =
        std::make_unique<DxbcShader>(xenos::ShaderType::kPixel, ShaderHash(pixel_words),
                                     pixel_words.data(), pixel_words.size(), std::endian::native);
    clear_vertex_shader = std::make_unique<DxbcShader>(
        xenos::ShaderType::kVertex, ShaderHash(clear_vertex_words), clear_vertex_words.data(),
        clear_vertex_words.size(), std::endian::native);
    clear_pixel_shader = std::make_unique<DxbcShader>(
        xenos::ShaderType::kPixel, ShaderHash(clear_pixel_words), clear_pixel_words.data(),
        clear_pixel_words.size(), std::endian::native);
    vertex_shader->AnalyzeUcode(disassembly);
    pixel_shader->AnalyzeUcode(disassembly);
    clear_vertex_shader->AnalyzeUcode(disassembly);
    clear_pixel_shader->AnalyzeUcode(disassembly);

    // The cataloged VS consumes position at offset 0 and UV at offset 16 in
    // a 56-byte fetch-95 record. Keep the unused bytes zero but preserve the
    // exact source stride required by the translated interface.
    for (const auto value :
         std::array<std::array<float, 5>, 3>{{{-1.0F, -1.0F, 0.0F, 0.0F, 0.0F},
                                              {-1.0F, 3.0F, 0.0F, 0.0F, 1.0F},
                                              {3.0F, -1.0F, 0.0F, 1.0F, 0.0F}}}) {
      AppendBigEndianFloat(vertex_bytes, value[0]);
      AppendBigEndianFloat(vertex_bytes, value[1]);
      AppendBigEndianFloat(vertex_bytes, value[2]);
      AppendBigEndianFloat(vertex_bytes, 0.0F);
      AppendBigEndianFloat(vertex_bytes, value[3]);
      AppendBigEndianFloat(vertex_bytes, value[4]);
      for (int padding = 0; padding < 8; ++padding)
        AppendBigEndianFloat(vertex_bytes, 0.0F);
    }
    index_bytes = {0, 0, 0, 1, 0, 2};

    clear_geometry_bytes = ClearGeometryBytes();
    resolve_geometry_bytes = ResolveGeometryBytes();
    full_resolve_geometry_bytes = FullResolveGeometryBytes();
    std::copy(vertex_bytes.begin(), vertex_bytes.end(), arena_address(kVertexOffset));
    std::copy(index_bytes.begin(), index_bytes.end(), arena_address(kIndexOffset));
    std::copy(clear_geometry_bytes.begin(), clear_geometry_bytes.end(),
              arena_address(kClearGeometryOffset));
    std::copy(resolve_geometry_bytes.begin(), resolve_geometry_bytes.end(),
              arena_address(kResolveGeometryOffset));
    std::copy(full_resolve_geometry_bytes.begin(), full_resolve_geometry_bytes.end(),
              arena_address(kFullResolveGeometryOffset));

    xenos::xe_gpu_vertex_fetch_t vertex_fetch{};
    vertex_fetch.type = xenos::FetchConstantType::kVertex;
    vertex_fetch.address = arena.address(kVertexOffset) / sizeof(uint32_t);
    vertex_fetch.endian = xenos::Endian::k8in32;
    vertex_fetch.size = static_cast<uint32_t>(vertex_bytes.size() / sizeof(uint32_t));
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U], &vertex_fetch,
                sizeof(vertex_fetch));

    xenos::xe_gpu_texture_fetch_t texture_fetch{};
    texture_fetch.type = xenos::FetchConstantType::kTexture;
    texture_fetch.sign_x = texture_fetch.sign_y = texture_fetch.sign_z = texture_fetch.sign_w =
        xenos::TextureSign::kUnsigned;
    texture_fetch.pitch = 256U / 32U;
    texture_fetch.format = xenos::TextureFormat::k_8_8_8_8;
    texture_fetch.endianness = xenos::Endian::kNone;
    texture_fetch.base_address = arena.address(kTextureOffset) >> 12;
    texture_fetch.size_2d.width = 256U - 1U;
    texture_fetch.size_2d.height = 32U - 1U;
    texture_fetch.tiled = true;
    texture_fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    texture_fetch.mag_filter = xenos::TextureFilter::kLinear;
    texture_fetch.min_filter = xenos::TextureFilter::kLinear;
    texture_fetch.mip_filter = xenos::TextureFilter::kPoint;
    texture_fetch.dimension = xenos::DataDimension::k2DOrStacked;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &texture_fetch,
                sizeof(texture_fetch));

    if (!texture_cache->PrepareFrameInputTexture(texture_fetch, texture_metadata)) {
      throw std::runtime_error("prepared support texture metadata failed to prepare");
    }
    texture_base_size = texture_metadata.base_size;

    clear_registers.values[0] = 0;
    clear_registers.values[0x2000] = 0x04000140;
    clear_registers.values[0x2002] = 0x530;
    clear_registers.values[0x200F] = clear_registers.values[0x2082] = 0x20002000;
    clear_registers.values[0x2104] = 0xF;
    clear_registers.values[0x2100] = 2;
    clear_registers.values[0x210D] = 0xFF0000;
    clear_registers.values[0x2200] = 0x8777;
    clear_registers.values[0x2201] = 0x00010001;
    clear_registers.values[0x2204] = clear_registers.values[0x2205] = 0x10000;
    clear_registers.values[0x2206] = 0x300;
    clear_registers.values[0x2208] = 4;
    xenos::xe_gpu_vertex_fetch_t clear_fetch{};
    clear_fetch.type = xenos::FetchConstantType::kVertex;
    clear_fetch.address = arena.address(kClearGeometryOffset) / sizeof(uint32_t);
    clear_fetch.endian = xenos::Endian::k8in32;
    clear_fetch.size = static_cast<uint32_t>(clear_geometry_bytes.size() / sizeof(uint32_t));
    std::memcpy(&clear_registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &clear_fetch,
                sizeof(clear_fetch));
    clear_registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1U] |= 0x10000000U;

    resolve_registers = registers;
    resolve_registers.values[0x2000] = 0x04000140;
    resolve_registers.values[0x2001] = 0;
    resolve_registers.values[0x2002] = 0x530;
    resolve_registers.values[0x200F] = 0x20002000;
    resolve_registers.values[0x2080] = 0;
    resolve_registers.values[0x2081] = 0;
    resolve_registers.values[0x2082] = 256U | (32U << 16);
    resolve_registers.values[0x2205] = 0x10000;
    resolve_registers.values[0x2208] = 6;
    resolve_registers.values[0x2318] = 0x100000;
    resolve_registers.values[0x231A] = 256U | (32U << 16);
    resolve_registers.values[0x231B] = 0x300;
    resolve_registers.values[0x231C] = 0xFFF;
    resolve_registers.values[0x231D] = 0xFFFFFF00;
    resolve_registers.values[0x231E] = resolve_registers.values[0x231F] = 0;
    xenos::xe_gpu_vertex_fetch_t resolve_fetch{};
    resolve_fetch.type = xenos::FetchConstantType::kVertex;
    resolve_fetch.address = arena.address(kResolveGeometryOffset) / sizeof(uint32_t);
    resolve_fetch.endian = xenos::Endian::k8in32;
    resolve_fetch.size = static_cast<uint32_t>(resolve_geometry_bytes.size() / sizeof(uint32_t));
    std::memcpy(&resolve_registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &resolve_fetch,
                sizeof(resolve_fetch));

    // Native gameplay validation consumes these guest draw controls. They are
    // the ordinary color-0 sample state used by the existing native stream.
    registers.values[0x2000] = 0x14000500;
    registers.values[0x2002] = 0x530;
    registers.values[0x200F] = 0x20002000;
    registers.values[0x2082] = 1280U | (720U << 16);
    registers.values[0x2100] = 2;
    registers.values[0x2101] = 0;
    registers.values[0x2102] = 0;
    registers.values[0x2103] = 0;
    registers.values[0x2104] = 0xF;
    registers.values[0x210F] = registers.values[0x2110] = std::bit_cast<uint32_t>(640.0F);
    registers.values[0x2111] = std::bit_cast<uint32_t>(-360.0F);
    registers.values[0x2112] = std::bit_cast<uint32_t>(360.0F);
    registers.values[0x2180] = 0x10010002;
    registers.values[0x2181] = 4;
    registers.values[0x2182] = 1;
    // The recovered VS uses two standard identity matrices in c0..c7 and a
    // zero c8 to preserve the canonical clip-space mapping. The recovered PS
    // supplies RGB from c255 and alpha from the sampled texture, so the
    // producer alpha remains an observable input.
    const auto setFloat = [&](std::size_t base, std::size_t index, float value) {
      registers.values[base + index] = std::bit_cast<std::uint32_t>(value);
    };
    for (std::size_t index = 0; index < 36; ++index)
      setFloat(0x4000, index, 0.0F);
    for (const auto index : std::array<std::size_t, 8>{0, 5, 10, 15, 16, 21, 26, 31})
      setFloat(0x4000, index, 1.0F);
    setFloat(0x4400, 0U, 0.0F);
    setFloat(0x4400, 1U, 0.0F);
    setFloat(0x4400, 2U, 0.0F);
    setFloat(0x4400, 3U, 1.0F);
    setFloat(0x4400, 255U * 4U + 0U, 0.1F);
    setFloat(0x4400, 255U * 4U + 1U, 0.2F);
    setFloat(0x4400, 255U * 4U + 2U, 0.3F);
    setFloat(0x4400, 255U * 4U + 3U, 1.0F);
    // Make the native consumer's RGB depend on the sampled alpha. The title
    // decoder maps source-alpha color blending (factor 6) with zero
    // destination color to the ordinary source-alpha blend used by the
    // producer-change control.
    registers.values[0x2201] = 0x00010006;
    registers.values[0x2202] = 0x87000000;
    registers.values[0x2204] = 0x80000;
    registers.values[0x2205] = 0x18000;
    registers.values[0x2206] = 0x43F;
    registers.values[0x2208] = 4;
    registers.values[0x2302] = 4;
    registers.values[0x2307] = 0xFF000;
    registers.values[0x2308] = 0xFF100;
  }

  static bool ReadGuestBytes(void* context, uint32_t address,
                             std::span<uint8_t> destination) noexcept;
  DrawCaptureGuestMemoryVersion Version(uint32_t address, uint32_t size);
};

bool PreparedFrameInputTestSupport::Impl::ReadGuestBytes(void* context, uint32_t address,
                                                         std::span<uint8_t> destination) noexcept {
  auto& self = *static_cast<Impl*>(context);
  if (!destination.size() || !self.Readable(address, static_cast<uint32_t>(destination.size()))) {
    return false;
  }
  const auto* source = self.memory.TranslatePhysical(address);
  if (!source) {
    return false;
  }
  std::memcpy(destination.data(), source, destination.size());
  return true;
}

DrawCaptureGuestMemoryVersion PreparedFrameInputTestSupport::Impl::Version(uint32_t address,
                                                                           uint32_t size) {
  ++version_calls;
  return versions.Get(address, size);
}

PreparedFrameInputTestSupport::PreparedFrameInputTestSupport() : impl_(std::make_unique<Impl>()) {}

PreparedFrameInputTestSupport::~PreparedFrameInputTestSupport() = default;

bool PreparedFrameInputTestSupport::BuildConsumerDraw(std::string& error,
                                                      bool native_source_ready) {
  error.clear();
  if (!impl_->initialized) {
    error = "prepared support guest memory failed to initialize";
    return false;
  }

  if (native_source_ready && !impl_->native_source_armed) {
    if (!ArmNativeSource(error) || !CompleteNativeSource(true, error)) {
      return false;
    }
  }

  impl_->draw = {};
  impl_->draw.id = 11;
  impl_->draw.frame = 2;
  impl_->draw.submission = 2;
  impl_->draw.registers = impl_->registers.values;
  impl_->draw.vertex_shader.present = true;
  impl_->draw.vertex_shader.hash = impl_->vertex_shader->ucode_data_hash();
  impl_->draw.vertex_shader.words = impl_->vertex_words;
  impl_->draw.pixel_shader.present = true;
  impl_->draw.pixel_shader.hash = impl_->pixel_shader->ucode_data_hash();
  impl_->draw.pixel_shader.words = impl_->pixel_words;
  impl_->draw.primitive_type = 4;
  impl_->draw.requested_index_count = 3;
  impl_->draw.index.present = true;
  impl_->draw.index.format = 0;
  impl_->draw.index.endianness = 1;
  impl_->draw.index.count = 3;
  impl_->draw.index.physical_address = impl_->arena.address(kIndexOffset);
  impl_->draw.index.size = static_cast<uint32_t>(impl_->index_bytes.size());
  impl_->draw.index.memory = {impl_.get(), &Impl::ReadGuestBytes};
  impl_->draw.index.memory_version = {};
  impl_->draw.indexed = true;
  impl_->draw.original_host_issued = false;
  impl_->draw.execution_status = diagnostic::DrawCaptureExecutionStatus::kUnfinished;

  FrameInputDrawBuilderInput input{
      impl_->registers,
      *impl_->texture_cache,
      impl_->vertex_shader.get(),
      impl_->pixel_shader.get(),
      impl_->vertex_shader.get(),
      impl_->pixel_shader.get(),
      {impl_.get(), &Impl::ReadGuestBytes},
      [this](uint32_t address, uint32_t size) { return impl_->Dirty(address, size); },
      [this](uint32_t address, uint32_t size) {
        return impl_->shared.IsRangeWrittenByGpu(address, size);
      },
      [this](uint32_t address, uint32_t size) {
        ++impl_->readable_calls;
        return impl_->Readable(address, size);
      },
      [this](uint32_t address, uint32_t size) { return impl_->Version(address, size); },
      [this](const Shader::PreparedSamplerBinding& binding, uint32_t& value) {
        ++impl_->sampler_calls;
        D3D12Shader::SamplerBinding d3d_binding{0,
                                                binding.fetch_constant,
                                                binding.sampler.mag_filter,
                                                binding.sampler.min_filter,
                                                binding.sampler.mip_filter,
                                                binding.sampler.aniso_filter};
        value = GetFrameInputSamplerParameters(impl_->registers, d3d_binding, -1).value;
        return true;
      },
      [this](uint32_t address, uint32_t size) {
        switch (impl_->native_sources.Classify(address, size)) {
          case FrameInputNativeSources::State::kUnresolved:
            return diagnostic::DrawCaptureReason::kTextureUnresolved;
          case FrameInputNativeSources::State::kRejected:
            return diagnostic::DrawCaptureReason::kTextureSource;
          case FrameInputNativeSources::State::kNone:
            return diagnostic::DrawCaptureReason::kNone;
        }
        return diagnostic::DrawCaptureReason::kTextureSource;
      },
      3};
  if (!FrameInputDrawBuilder::BuildPrepared(input, impl_->result, impl_->draw)) {
    error = "prepared support builder rejected the analyzed draw";
    return false;
  }
  return impl_->draw.cpu_data_complete ||
         impl_->draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureUnresolved;
}

bool PreparedFrameInputTestSupport::BuildClearDraw(std::string& error) {
  error.clear();
  if (!impl_->initialized) {
    error = "prepared support guest memory failed to initialize";
    return false;
  }

  impl_->clear_draw = {};
  impl_->clear_draw.id = 2;
  impl_->clear_draw.frame = impl_->clear_draw.submission = 1;
  impl_->clear_draw.registers = impl_->clear_registers.values;
  impl_->clear_draw.vertex_shader = {true, impl_->clear_vertex_shader->ucode_data_hash(),
                                     impl_->clear_vertex_words};
  impl_->clear_draw.pixel_shader = {true, impl_->clear_pixel_shader->ucode_data_hash(),
                                    impl_->clear_pixel_words};
  impl_->clear_draw.primitive_type = 8;
  impl_->clear_draw.requested_index_count = 3;
  impl_->clear_draw.indexed = false;
  impl_->clear_draw.original_host_issued = false;
  impl_->clear_draw.execution_status = diagnostic::DrawCaptureExecutionStatus::kUnfinished;

  FrameInputDrawBuilderInput input{
      impl_->clear_registers,
      *impl_->texture_cache,
      impl_->clear_vertex_shader.get(),
      impl_->clear_pixel_shader.get(),
      impl_->clear_vertex_shader.get(),
      impl_->clear_pixel_shader.get(),
      {impl_.get(), &Impl::ReadGuestBytes},
      [this](uint32_t address, uint32_t size) { return impl_->Dirty(address, size); },
      [this](uint32_t address, uint32_t size) {
        return impl_->shared.IsRangeWrittenByGpu(address, size);
      },
      [this](uint32_t address, uint32_t size) {
        ++impl_->readable_calls;
        return impl_->Readable(address, size);
      },
      [this](uint32_t address, uint32_t size) { return impl_->Version(address, size); },
      [this](const Shader::PreparedSamplerBinding& binding, uint32_t& value) {
        ++impl_->sampler_calls;
        D3D12Shader::SamplerBinding d3d_binding{0,
                                                binding.fetch_constant,
                                                binding.sampler.mag_filter,
                                                binding.sampler.min_filter,
                                                binding.sampler.mip_filter,
                                                binding.sampler.aniso_filter};
        value = GetFrameInputSamplerParameters(impl_->clear_registers, d3d_binding, -1).value;
        return true;
      },
      [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
      impl_->clear_draw.requested_index_count};
  if (!FrameInputDrawBuilder::BuildPrepared(input, impl_->clear_result, impl_->clear_draw)) {
    error = "prepared support clear builder rejected the analyzed clear";
    return false;
  }
  return impl_->clear_draw.cpu_data_complete;
}

bool PreparedFrameInputTestSupport::ArmNativeSource(std::string& error) {
  error.clear();
  if (!impl_->PrepareTextureSourceMetadata()) {
    error = "prepared support texture metadata failed to prepare native source";
    return false;
  }
  if (impl_->native_source_armed) {
    return true;
  }
  if (!impl_->native_sources.Arm(impl_->texture_metadata.base_address,
                                 impl_->texture_metadata.base_size)) {
    error = "prepared support native source sideband rejected the texture range";
    return false;
  }
  impl_->native_source_armed = true;
  return true;
}

bool PreparedFrameInputTestSupport::CompleteNativeSource(bool original_succeeded,
                                                         std::string& error) {
  error.clear();
  if (!impl_->native_source_armed) {
    error = "prepared support native source was not armed";
    return false;
  }
  impl_->native_sources.Complete(impl_->texture_metadata.base_address,
                                 impl_->texture_metadata.base_size, original_succeeded);
  return impl_->native_sources.Classify(impl_->texture_metadata.base_address,
                                        impl_->texture_metadata.base_size) ==
         FrameInputNativeSources::State::kUnresolved;
}

void PreparedFrameInputTestSupport::SetClearAlpha(float alpha) {
  if (!impl_->initialized || !std::isfinite(alpha)) {
    return;
  }
  const auto bits = std::bit_cast<std::uint32_t>(alpha);
  for (std::size_t vertex = 0; vertex < 3; ++vertex) {
    const auto offset = vertex * 7U * sizeof(float) + 6U * sizeof(float);
    impl_->clear_geometry_bytes[offset + 0] = static_cast<std::uint8_t>(bits >> 24);
    impl_->clear_geometry_bytes[offset + 1] = static_cast<std::uint8_t>(bits >> 16);
    impl_->clear_geometry_bytes[offset + 2] = static_cast<std::uint8_t>(bits >> 8);
    impl_->clear_geometry_bytes[offset + 3] = static_cast<std::uint8_t>(bits);
    std::copy(impl_->clear_geometry_bytes.begin() + offset,
              impl_->clear_geometry_bytes.begin() + offset + sizeof(float),
              impl_->arena_address(kClearGeometryOffset) + offset);
  }
}

bool PreparedFrameInputTestSupport::BuildResolveCopy(
    std::uint64_t id, std::uint64_t frame, std::uint64_t submission, std::uint32_t destination,
    bool small_source, diagnostic::DrawCaptureFrameInputCopyView& copy, std::string& error) {
  error.clear();
  if (!impl_->initialized || !destination || destination >= SharedMemory::kBufferSize) {
    error = "prepared support resolve destination is invalid";
    return false;
  }
  const auto geometry_offset = small_source ? kResolveGeometryOffset : kFullResolveGeometryOffset;
  const auto& geometry_bytes =
      small_source ? impl_->resolve_geometry_bytes : impl_->full_resolve_geometry_bytes;
  impl_->resolve_registers.values[0x2000] = small_source ? 0x04000140U : 0x14000500U;
  impl_->resolve_registers.values[0x2082] =
      small_source ? 256U | (32U << 16) : 1280U | (720U << 16);
  impl_->resolve_registers.values[0x231A] = small_source ? 256U | (32U << 16) : 0x02D00500U;
  xenos::xe_gpu_vertex_fetch_t resolve_fetch{};
  resolve_fetch.type = xenos::FetchConstantType::kVertex;
  resolve_fetch.address = impl_->arena.address(geometry_offset) / sizeof(uint32_t);
  resolve_fetch.endian = xenos::Endian::k8in32;
  resolve_fetch.size = static_cast<uint32_t>(geometry_bytes.size() / sizeof(uint32_t));
  std::memcpy(&impl_->resolve_registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
              &resolve_fetch, sizeof(resolve_fetch));
  impl_->resolve_registers.values[0x2319] = destination;
  copy = {};
  copy.id = id;
  copy.frame = frame;
  copy.submission = submission;
  copy.copy_mode = "resolve";
  copy.source_geometry.memory = {impl_.get(), &Impl::ReadGuestBytes};
  PrepareFrameInputCopyPlan(copy, impl_->resolve_registers, impl_->memory, 1, 1, false,
                            impl_->resolve_geometry, true);
  if (!copy.cpu_data_complete) {
    error = "prepared support resolve copy plan rejected the analyzed geometry";
    return false;
  }
  copy.source_geometry.memory = {impl_.get(), &Impl::ReadGuestBytes};
  copy.source_geometry.memory_version = impl_->Version(
      impl_->arena.address(geometry_offset), static_cast<uint32_t>(geometry_bytes.size()));
  return copy.source_geometry.memory_version.IsCurrent();
}

const diagnostic::DrawCaptureFrameInputDrawView& PreparedFrameInputTestSupport::draw() const {
  return impl_->draw;
}

const diagnostic::DrawCaptureFrameInputDrawView& PreparedFrameInputTestSupport::clear_draw() const {
  return impl_->clear_draw;
}

std::span<const std::uint32_t> PreparedFrameInputTestSupport::registers() const {
  return impl_->registers.values;
}

std::span<const std::uint32_t> PreparedFrameInputTestSupport::vertex_words() const {
  return impl_->vertex_words;
}

std::span<const std::uint32_t> PreparedFrameInputTestSupport::pixel_words() const {
  return impl_->pixel_words;
}

std::uint64_t PreparedFrameInputTestSupport::vertex_shader_hash() const {
  return impl_->vertex_shader ? impl_->vertex_shader->ucode_data_hash() : 0;
}

std::uint64_t PreparedFrameInputTestSupport::pixel_shader_hash() const {
  return impl_->pixel_shader ? impl_->pixel_shader->ucode_data_hash() : 0;
}

const std::string& PreparedFrameInputTestSupport::vertex_word_digest() const {
  static const std::string empty;
  return impl_->vertex_words.empty() ? empty : impl_->vertex_digest;
}

const std::string& PreparedFrameInputTestSupport::pixel_word_digest() const {
  static const std::string empty;
  return impl_->pixel_words.empty() ? empty : impl_->pixel_digest;
}

std::uint32_t PreparedFrameInputTestSupport::texture_fetch_constant() const {
  return 0;
}

std::size_t PreparedFrameInputTestSupport::sampler_calls() const {
  return impl_->sampler_calls;
}

std::size_t PreparedFrameInputTestSupport::readable_calls() const {
  return impl_->readable_calls;
}

std::size_t PreparedFrameInputTestSupport::version_calls() const {
  return impl_->version_calls;
}

std::uint32_t PreparedFrameInputTestSupport::texture_base() const {
  return impl_->texture_metadata.base_address;
}

std::uint32_t PreparedFrameInputTestSupport::texture_base_size() const {
  return impl_->texture_base_size;
}

}  // namespace rex::graphics::testing
