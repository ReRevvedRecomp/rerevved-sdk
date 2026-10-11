#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <limits>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/info.h>
#ifdef REX_TEST_DXBC_TRANSLATION
#include <rex/graphics/pipeline/shader/dxbc.h>
#endif

#include "graphics/d3d12/frame_input_copy_plan.h"
#include "test_memory.h"

namespace {
using namespace rex::graphics;
using diagnostic::DrawCaptureFrameInputCopyView;
using diagnostic::DrawCaptureFrameInputTextureView;

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
  using TextureCache::BindingInfoFromFetchConstant;
  using TextureCache::TextureKey;
  uint32_t create_count = 0;
  uint32_t load_count = 0;
  uint32_t host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;

 private:
  uint32_t GetHostFormatSwizzle(TextureKey) const override { return host_swizzle; }
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension) const override { return 8192; }
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension) const override { return 2048; }
  std::unique_ptr<Texture> CreateTexture(TextureKey) override {
    ++create_count;
    return nullptr;
  }
  bool LoadTextureDataFromResidentMemoryImpl(Texture&, bool, bool) override {
    ++load_count;
    return false;
  }
};

Shader::TextureBinding Sample(uint32_t fetch) {
  Shader::TextureBinding binding{};
  binding.fetch_constant = fetch;
  auto& instruction = binding.fetch_instr;
  instruction.opcode = ucode::FetchOpcode::kTextureFetch;
  instruction.dimension = xenos::FetchOpDimension::k2D;
  instruction.operand_count = 2;
  instruction.operands[1].storage_source = InstructionStorageSource::kTextureFetchConstant;
  instruction.operands[1].storage_index = fetch;
  instruction.result.storage_target = InstructionStorageTarget::kRegister;
  instruction.result.original_write_mask = 15;
  return binding;
}

TEST_CASE("Prepared texture demand preserves descriptor ordering and rejects unsupported shapes",
          "[graphics][prepared]") {
  auto first = Sample(7);
  auto repeated = first;
  repeated.fetch_instr.attributes.offset_x = 0.5f;
  auto second = Sample(2);
  std::vector<Shader::PreparedTextureBinding> bindings;
  std::vector<Shader::PreparedSamplerBinding> samplers;
  REQUIRE(Shader::PrepareTextureBindings({first, second, repeated}, true, bindings, samplers));
  REQUIRE(bindings.size() == 4);
  CHECK(bindings[0].fetch_constant == 7);
  CHECK_FALSE(bindings[0].is_signed);
  CHECK(bindings[1].is_signed);
  CHECK(bindings[2].fetch_constant == 2);
  CHECK(samplers.size() == 2);
  repeated.fetch_instr.attributes.min_filter = xenos::TextureFilter::kPoint;
  REQUIRE(Shader::PrepareTextureBindings({first, second, repeated}, true, bindings, samplers));
  CHECK(bindings.size() == 4);
  CHECK(samplers.size() == 3);

  for (const auto dimension : {xenos::FetchOpDimension::k1D, xenos::FetchOpDimension::k3DOrStacked,
                               xenos::FetchOpDimension::kCube}) {
    second.fetch_instr.dimension = dimension;
    CHECK_FALSE(Shader::PrepareTextureBindings({first, second}, true, bindings, samplers));
    CHECK(bindings.empty());
    CHECK(samplers.empty());
  }
  second = Sample(2);
  second.fetch_instr.opcode = ucode::FetchOpcode::kGetTextureComputedLod;
  CHECK_FALSE(Shader::PrepareTextureBindings({first, second}, true, bindings, samplers));
  first.fetch_instr.result.components[0] = SwizzleSource::k1;
  first.fetch_instr.result.original_write_mask = 1;
  REQUIRE(Shader::PrepareTextureBindings({first}, true, bindings, samplers));
  CHECK(bindings.empty());
  CHECK(samplers.empty());
}

#ifdef REX_TEST_DXBC_TRANSLATION
// Synthetic microcode uses the named field positions in ucode.h. Both analysis
// and real DXBC translation consume these words, independently of CPU metadata.
std::vector<uint32_t> SampleUcode(bool computed_lod) {
  constexpr uint32_t kFetchSequence = 1 | (1 << 2) | (1 << 4);
  std::vector<uint32_t> words{1 | (3 << 12) | (kFetchSequence << 16),
                              uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12, 0};
  for (uint32_t fetch : {7u, 2u, 7u}) {
    words.push_back(uint32_t(ucode::FetchOpcode::kTextureFetch) | (1 << 19) | (fetch << 20) |
                    (0x24u << 26));
    words.push_back(0x688u | (uint32_t(xenos::TextureFilter::kPoint) << 12) |
                    (uint32_t(xenos::TextureFilter::kUseFetchConst) << 14) |
                    (uint32_t(xenos::TextureFilter::kLinear) << 16) |
                    (uint32_t(xenos::AnisoFilter::kMax_8_1) << 18) | (3u << 24) | (3u << 26) |
                    (uint32_t(computed_lod) << 28));
    words.push_back(uint32_t(xenos::FetchOpDimension::k2D) << 14);
  }
  return words;
}

TEST_CASE("Prepared binding metadata matches actual DXBC translation",
          "[graphics][prepared][dxbc]") {
  for (const auto stage : {xenos::ShaderType::kVertex, xenos::ShaderType::kPixel}) {
    for (const bool computed : {false, true}) {
      const auto words = SampleUcode(computed);
      DxbcShader shader(stage, 1, words.data(), words.size(), std::endian::native);
      rex::string::StringBuffer disassembly;
      shader.AnalyzeUcode(disassembly);
      std::vector<Shader::PreparedTextureBinding> bindings;
      std::vector<Shader::PreparedSamplerBinding> samplers;
      REQUIRE(Shader::PrepareTextureBindings(
          shader.texture_bindings(), stage == xenos::ShaderType::kPixel, bindings, samplers));
      DxbcShaderTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kMicrosoft, false,
                                      false);
      const auto modification = stage == xenos::ShaderType::kPixel
                                    ? translator.GetDefaultPixelShaderModification(4)
                                    : translator.GetDefaultVertexShaderModification(4);
      auto* translation = shader.GetOrCreateTranslation(modification);
      REQUIRE(translator.TranslateAnalyzedShader(*translation));
      REQUIRE(translation->is_valid());
      const auto& translated = shader.GetTextureBindingsAfterTranslation();
      REQUIRE(bindings.size() == translated.size());
      REQUIRE(bindings.size() == 4);
      for (size_t i = 0; i < bindings.size(); ++i) {
        CHECK(bindings[i].fetch_constant == translated[i].fetch_constant);
        CHECK(bindings[i].is_signed == translated[i].is_signed);
        CHECK(translated[i].dimension == xenos::FetchOpDimension::k2D);
      }
      const auto& translated_samplers = shader.GetSamplerBindingsAfterTranslation();
      REQUIRE(samplers.size() == translated_samplers.size());
      for (size_t i = 0; i < samplers.size(); ++i) {
        CHECK(samplers[i].fetch_constant == translated_samplers[i].fetch_constant);
        CHECK(samplers[i].sampler.mag_filter == translated_samplers[i].mag_filter);
        CHECK(samplers[i].sampler.min_filter == translated_samplers[i].min_filter);
        CHECK(samplers[i].sampler.mip_filter == translated_samplers[i].mip_filter);
        CHECK(samplers[i].sampler.aniso_filter == translated_samplers[i].aniso_filter);
      }
    }
  }
}
#endif

TEST_CASE("Prepared fetch metadata matches active cache interpretation without upload",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  for (const bool tiled : {false, true}) {
    for (const uint32_t swizzle : {uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA),
                                   uint32_t(XE_GPU_MAKE_TEXTURE_SWIZZLE(B, G, 0, 1))}) {
      cache.host_swizzle = swizzle;
      for (const auto sign : {xenos::TextureSign::kUnsigned, xenos::TextureSign::kSigned,
                              xenos::TextureSign::kUnsignedBiased}) {
        xenos::xe_gpu_texture_fetch_t fetch{};
        fetch.type = xenos::FetchConstantType::kTexture;
        fetch.dimension = xenos::DataDimension::k2DOrStacked;
        fetch.size_2d.width = 63;
        fetch.size_2d.height = 31;
        fetch.pitch = 2;
        fetch.base_address = 16;
        fetch.format = xenos::TextureFormat::k_8_8_8_8;
        fetch.tiled = tiled;
        fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
        fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w = sign;
        DrawCaptureFrameInputTextureView view;
        REQUIRE(cache.PrepareFrameInputTexture(fetch, view));
        CHECK(cache.create_count == 0);
        CHECK(cache.load_count == 0);
        TextureInfo legacy;
        REQUIRE(TextureInfo::Prepare(fetch, &legacy));
        TestTextureCache::TextureKey key;
        TestTextureCache::BindingInfoFromFetchConstant(fetch, key, nullptr);
        CHECK(view.base_address == legacy.memory.base_address);
        CHECK(view.base_size == legacy.memory.base_size);
        CHECK(view.base_size >= key.GetGuestLayout().base.level_data_extent_bytes);
        std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch,
                    sizeof(fetch));
        cache.TextureFetchConstantWritten(0);
        cache.RequestTextures(1);
        CHECK(view.swizzled_signs == cache.GetActiveTextureSwizzledSigns(0));
        CHECK(view.component_mapping == cache.GetActiveTextureHostSwizzle(0));
        cache.ClearCache();
        cache.create_count = cache.load_count = 0;
      }
    }
  }
}

TEST_CASE("Prepared texture ranges cover canonical ordinary mip storage", "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.size_2d.width = fetch.size_2d.height = 63;
  fetch.pitch = 2;
  fetch.base_address = 16;
  fetch.mip_address = 32;
  fetch.mip_max_level = 1;
  fetch.tiled = true;
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  fetch.sign_x = xenos::TextureSign::kSigned;
  DrawCaptureFrameInputTextureView unsigned_view;
  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  REQUIRE(cache.PrepareFrameInputTexture(fetch, unsigned_view));
  REQUIRE(cache.PrepareFrameInputTexture(fetch, signed_view));
  CHECK(unsigned_view.required);
  CHECK(signed_view.required);
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  CHECK(unsigned_view.base_address == key.base_page << 12);
  CHECK(unsigned_view.base_size == layout.base.level_data_extent_bytes);
  CHECK(unsigned_view.mip_address == key.mip_page << 12);
  CHECK(unsigned_view.mip_size == layout.mips_total_extent_bytes);
  CHECK_FALSE(unsigned_view.memory_version.IsCurrent());
  CHECK(cache.create_count == 0);
}

TEST_CASE("Prepared fetch metadata rejects malformed and uncovered layouts",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.size_2d.width = fetch.size_2d.height = 31;
  fetch.pitch = 1;
  fetch.base_address = 16;
  fetch.mip_address = 32;
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  fetch.tiled = true;
  fetch.packed_mips = true;
  fetch.mip_max_level = 5;
  DrawCaptureFrameInputTextureView view;
  CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, view));
  CHECK_FALSE(view.available);
  fetch.packed_mips = false;
  fetch.mip_min_level = 1;
  CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, view));
  fetch.mip_min_level = 0;
  fetch.pitch = 0;
  CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, view));
  fetch.pitch = 1;
  fetch.stacked = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, view));
  CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureDimension);
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);
}

struct ResolveFixture {
  rex::memory::Memory& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared{memory};
  RegisterFile registers;
  d3d12::FrameInputCopyGeometry geometry;
  rex::memory::BaseHeap* heap = nullptr;
  uint32_t guest_address = 0;
  uint32_t address = 0;
  bool mutate_after_read = false;

  ResolveFixture() {
    heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
    REQUIRE(heap);
    const uint32_t page = uint32_t(rex::memory::page_size());
    REQUIRE(heap->Alloc(
        page, page, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &guest_address));
    address = guest_address & 0x1FFFFFFF;
    const std::array<float, 6> vertices{0, 0, 64, 0, 0, 32};
    std::memcpy(memory.TranslatePhysical(address), vertices.data(), sizeof(vertices));
    xenos::xe_gpu_vertex_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kVertex;
    fetch.address = address / 4;
    fetch.size = 6;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
    registers[XE_GPU_REG_RB_SURFACE_INFO] = 64;
    registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 64 | (32 << 16);
    registers[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 8192 | (8192 << 16);
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = uint32_t(xenos::ColorFormat::k_8_8_8_8) << 7;
    registers[XE_GPU_REG_RB_COPY_DEST_PITCH] = 64 | (32 << 16);
    registers[XE_GPU_REG_RB_COPY_DEST_BASE] = 0x100000;
  }
  ~ResolveFixture() { CHECK(heap->Release(guest_address, nullptr)); }
  static bool Read(void* context, uint32_t address, std::span<uint8_t> output) noexcept {
    auto& fixture = *static_cast<ResolveFixture*>(context);
    address &= 0x1FFFFFFF;
    auto* physical = fixture.memory.GetPhysicalHeap();
    if (address > SharedMemory::kBufferSize || output.empty() ||
        output.size() > SharedMemory::kBufferSize - address ||
        fixture.shared.IsRangeWrittenByGpu(address, uint32_t(output.size())) || !physical ||
        physical->QueryRangeAccess(address, address + uint32_t(output.size()) - 1) ==
            rex::memory::PageAccess::kNoAccess) {
      return false;
    }
    std::memcpy(output.data(), fixture.memory.TranslatePhysical(address), output.size());
    if (fixture.mutate_after_read) {
      *fixture.memory.TranslatePhysical<float*>(fixture.address + 8) = 8;
    }
    return true;
  }
  DrawCaptureFrameInputCopyView Plan() {
    DrawCaptureFrameInputCopyView copy;
    copy.source_geometry.memory = {this, &Read};
    d3d12::PrepareFrameInputCopyPlan(copy, registers, memory, 1, 1, false, geometry);
    if (geometry.present) {
      copy.source_geometry.memory = {&geometry, &d3d12::FrameInputCopyGeometry::Read};
    }
    return copy;
  }
};

TEST_CASE("Prepared resolve plan matches original normalization and owns scalar values",
          "[graphics][prepared]") {
  ResolveFixture fixture;
  for (const auto command : {xenos::CopyCommand::kRaw, xenos::CopyCommand::kConvert}) {
    fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = uint32_t(command) << 20;
    auto copy = fixture.Plan();
    REQUIRE(copy.cpu_data_complete);
    draw_util::ResolveInfo original;
    REQUIRE(
        draw_util::GetResolveInfo(fixture.registers, fixture.memory, 1, 1, false, false, original));
    CHECK(copy.planned_address == original.copy_dest_extent_start);
    CHECK(copy.planned_length == original.copy_dest_extent_length);
    CHECK(copy.source_width == original.coordinate_info.width_div_8 * 8);
    CHECK(copy.source_height == original.height_div_8 * 8);
    CHECK_FALSE(copy.original_succeeded);
    CHECK(copy.written_address == 0);
    CHECK(copy.written_length == 0);
  }
  const auto owned = fixture.Plan();
  fixture.registers[XE_GPU_REG_RB_COPY_DEST_BASE] = 0;
  *fixture.memory.TranslatePhysical<float*>(fixture.address + 8) = 8;
  CHECK(owned.planned_address == 0x100000);
  CHECK(owned.source_width == 64);
}

TEST_CASE("Prepared resolve shares half-pixel window scissor and MSAA normalization",
          "[graphics][prepared]") {
  ResolveFixture fixture;
  for (const uint32_t samples : {0u, 1u, 2u}) {
    for (const uint32_t pixel_center : {0u, 1u}) {
      fixture.registers[XE_GPU_REG_RB_SURFACE_INFO] = 64 | (samples << 16);
      fixture.registers[XE_GPU_REG_PA_SU_VTX_CNTL] = pixel_center;
      fixture.registers[XE_GPU_REG_PA_SC_WINDOW_OFFSET] = 8 | (8 << 16);
      fixture.registers[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 1 << 16;
      fixture.registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 48 | (24 << 16);
      const auto copy = fixture.Plan();
      REQUIRE(copy.cpu_data_complete);
      draw_util::ResolveInfo original;
      REQUIRE(draw_util::GetResolveInfo(fixture.registers, fixture.memory, 1, 1, false, false,
                                        original));
      CHECK(copy.source_x == original.coordinate_info.edram_offset_x_div_8 * 8);
      CHECK(copy.source_y == original.coordinate_info.edram_offset_y_div_8 * 8);
      CHECK(copy.source_width == original.coordinate_info.width_div_8 * 8);
      CHECK(copy.source_height == original.height_div_8 * 8);
      CHECK(copy.planned_address == original.copy_dest_extent_start);
      CHECK(copy.planned_length == original.copy_dest_extent_length);
    }
  }
}

TEST_CASE("Prepared resolve rejects unsupported effects and unowned geometry",
          "[graphics][prepared]") {
  {
    ResolveFixture gpu_owned;
    gpu_owned.shared.RangeWrittenByGpu(gpu_owned.address, 24);
    CHECK_FALSE(gpu_owned.Plan().cpu_data_complete);
  }
  ResolveFixture fixture;
  fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = 1 << 8;
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
  fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = 1 << 9;
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
  fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = 0;
  fixture.registers[XE_GPU_REG_RB_COPY_DEST_BASE] = 0xFFFFF000;
  auto copy = fixture.Plan();
  CHECK_FALSE(copy.cpu_data_complete);
  CHECK(copy.planned_length == 0);
  fixture.registers[XE_GPU_REG_RB_COPY_DEST_BASE] = 0x100000;
  copy = fixture.Plan();
  copy.copy_mode = "resolve_readback";
  copy.source_geometry.memory = {&fixture, &ResolveFixture::Read};
  d3d12::PrepareFrameInputCopyPlan(copy, fixture.registers, fixture.memory, 1, 1, false,
                                   fixture.geometry);
  CHECK_FALSE(copy.cpu_data_complete);
  CHECK(copy.unavailable_reason == diagnostic::DrawCaptureReason::kReadback);
  CHECK(copy.source_rect_present);
  fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = 5;
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
  fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = 0;
  fixture.registers[XE_GPU_REG_RB_COPY_DEST_INFO] |= 1 << 3;
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
  copy = {};
  copy.source_geometry.memory = {&fixture, &ResolveFixture::Read};
  d3d12::PrepareFrameInputCopyPlan(copy, fixture.registers, fixture.memory, 1, 1, false,
                                   fixture.geometry, false);
  CHECK_FALSE(copy.cpu_data_complete);
  CHECK(copy.source_rect_present);
  fixture.registers[XE_GPU_REG_RB_COPY_DEST_INFO] &= ~(1 << 3);
  fixture.registers[XE_GPU_REG_RB_COPY_DEST_PITCH] = 0;
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
  fixture.registers[XE_GPU_REG_RB_COPY_DEST_PITCH] = 64 | (32 << 16);
  fixture.registers[XE_GPU_REG_RB_SURFACE_INFO] = 64 | (3 << 16);
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
  fixture.registers[XE_GPU_REG_RB_SURFACE_INFO] = 64;
  fixture.registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 0;
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
}

TEST_CASE("Prepared resolve rejects clear-only formats while legacy views retain rectangles",
          "[graphics][prepared]") {
  ResolveFixture fixture;
  fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = 1 << 8;
  fixture.registers[XE_GPU_REG_RB_COPY_DEST_INFO] = uint32_t(xenos::TextureFormat::k_1_REVERSE)
                                                    << 7;
  const auto prepared = fixture.Plan();
  CHECK_FALSE(prepared.cpu_data_complete);
  CHECK(prepared.unavailable_reason == diagnostic::DrawCaptureReason::kTextureFormat);
  CHECK_FALSE(prepared.source_rect_present);

  draw_util::ResolveInfo original;
  REQUIRE(
      draw_util::GetResolveInfo(fixture.registers, fixture.memory, 1, 1, false, false, original));
  REQUIRE(original.copy_dest_extent_length == 0);
  DrawCaptureFrameInputCopyView issued;
  issued.original_succeeded = true;
  issued.source_geometry.memory = {&fixture, &ResolveFixture::Read};
  d3d12::PrepareFrameInputCopyPlan(issued, fixture.registers, fixture.memory, 1, 1, false,
                                   fixture.geometry, false);
  CHECK_FALSE(issued.cpu_data_complete);
  CHECK(issued.source_rect_present);
  CHECK(issued.source_x == original.coordinate_info.edram_offset_x_div_8 * 8);
  CHECK(issued.source_y == original.coordinate_info.edram_offset_y_div_8 * 8);
  CHECK(issued.source_width == original.coordinate_info.width_div_8 * 8);
  CHECK(issued.source_height == original.height_div_8 * 8);
  CHECK(issued.planned_length == 0);
  CHECK(issued.original_succeeded);
  CHECK(issued.written_address == 0);
  CHECK(issued.written_length == 0);
}

TEST_CASE("Prepared geometry rejection preserves legacy resolve normalization",
          "[graphics][prepared]") {
  ResolveFixture fixture;
  SECTION("nonaligned degenerate vertices expand to a legacy rectangle") {
    const std::array<float, 6> vertices{1, 0, 1, 0, 1, 32};
    std::memcpy(fixture.memory.TranslatePhysical(fixture.address), vertices.data(),
                sizeof(vertices));
  }
  SECTION("nonaligned empty scissor expands to a legacy rectangle") {
    fixture.registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 1;
    fixture.registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 1 | (32 << 16);
  }
  SECTION("legacy fixed-point conversion tolerates a nonfinite coordinate") {
    *fixture.memory.TranslatePhysical<float*>(fixture.address) =
        std::numeric_limits<float>::quiet_NaN();
  }
  SECTION("legacy source selection treats selector five as depth") {
    fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = 5;
  }
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
  draw_util::ResolveInfo original;
  REQUIRE(
      draw_util::GetResolveInfo(fixture.registers, fixture.memory, 1, 1, false, false, original));
  DrawCaptureFrameInputCopyView issued;
  issued.source_geometry.memory = {&fixture, &ResolveFixture::Read};
  d3d12::PrepareFrameInputCopyPlan(issued, fixture.registers, fixture.memory, 1, 1, false,
                                   fixture.geometry, false);
  CHECK_FALSE(issued.cpu_data_complete);
  CHECK(issued.source_rect_present);
  CHECK(issued.source_x == original.coordinate_info.edram_offset_x_div_8 * 8);
  CHECK(issued.source_y == original.coordinate_info.edram_offset_y_div_8 * 8);
  CHECK(issued.source_width == original.coordinate_info.width_div_8 * 8);
  CHECK(issued.source_height == original.height_div_8 * 8);
}

TEST_CASE("Prepared resolve keeps planning and callback geometry on one snapshot",
          "[graphics][prepared]") {
  ResolveFixture fixture;
  fixture.mutate_after_read = true;
  const auto copy = fixture.Plan();
  REQUIRE(copy.cpu_data_complete);
  CHECK(copy.source_width == 64);
  std::array<uint8_t, 24> payload;
  REQUIRE(copy.source_geometry.memory.Copy(fixture.address, payload));
  std::array<float, 6> vertices;
  std::memcpy(vertices.data(), payload.data(), payload.size());
  CHECK(vertices[2] == 64);
  CHECK(*fixture.memory.TranslatePhysical<float*>(fixture.address + 8) == 8);
  CHECK_FALSE(copy.source_geometry.memory_version.IsCurrent());
}

TEST_CASE("Prepared resolve preserves physical fetch aliases in the borrowed geometry",
          "[graphics][prepared]") {
  ResolveFixture fixture;
  auto fetch = fixture.registers.GetVertexFetch(0);
  fetch.address = (fixture.address | 0xA0000000u) / 4;
  std::memcpy(&fixture.registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch,
              sizeof(fetch));
  const auto copy = fixture.Plan();
  REQUIRE(copy.cpu_data_complete);
  CHECK(copy.source_geometry.physical_address == (fixture.address | 0xA0000000u));
  std::array<uint8_t, 24> bytes;
  REQUIRE(copy.source_geometry.memory.Copy(copy.source_geometry.physical_address, bytes));
  CHECK(copy.source_width == 64);
}

TEST_CASE("Prepared resolve rejects inaccessible and clipped-empty geometry",
          "[graphics][prepared]") {
  ResolveFixture fixture;
  const uint32_t page = uint32_t(rex::memory::page_size());
  REQUIRE(fixture.heap->Protect(fixture.guest_address, page, rex::memory::kMemoryProtectNoAccess));
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
  REQUIRE(
      fixture.heap->Protect(fixture.guest_address, page,
                            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
  fixture.registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 80;
  fixture.registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 100 | (32 << 16);
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
  CHECK_FALSE(fixture.Plan().source_rect_present);
}
}  // namespace
