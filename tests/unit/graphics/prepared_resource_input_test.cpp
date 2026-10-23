#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/info.h>
#ifdef REX_TEST_DXBC_TRANSLATION
#include <rex/graphics/pipeline/shader/dxbc.h>
#endif

#include "graphics/d3d12/frame_input_copy_plan.h"
#include "graphics/d3d12/frame_input_draw_builder.h"
#include "graphics/frame_input_native_sources.h"
#include "test_memory.h"

namespace {
using namespace rex::graphics;
using namespace rex::graphics::d3d12;
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

  second.fetch_instr.dimension = xenos::FetchOpDimension::kCube;
  REQUIRE(Shader::PrepareTextureBindings({first, second}, true, bindings, samplers));
  CHECK(bindings.size() == 4);
  CHECK(samplers.size() == 2);
  for (const auto dimension :
       {xenos::FetchOpDimension::k1D, xenos::FetchOpDimension::k3DOrStacked}) {
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

std::vector<uint32_t> NoTextureUcode() {
  return {1 | (3 << 12), uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12, 0};
}

TEST_CASE("Prepared omitted host shader with no texture demand remains complete",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto no_texture_words = NoTextureUcode();
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, no_texture_words.data(), no_texture_words.size(),
                    std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, no_texture_words.data(), no_texture_words.size(),
                   std::endian::native);
  const auto textured_words = SampleUcode(false);
  DxbcShader textured_pixel(xenos::ShaderType::kPixel, 3, textured_words.data(),
                            textured_words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);
  textured_pixel.AnalyzeUcode(disassembly);
  REQUIRE(vertex.is_ucode_analyzed());
  REQUIRE(pixel.is_ucode_analyzed());
  REQUIRE(pixel.texture_bindings().empty());
  REQUIRE(!textured_pixel.texture_bindings().empty());

  FrameInputDrawBuilderInput input{
      registers,
      cache,
      &vertex,
      nullptr,
      &vertex,
      &pixel,
      {},
      [](uint32_t, uint32_t) { return false; },
      [](uint32_t, uint32_t) { return false; },
      [](uint32_t, uint32_t) { return true; },
      [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
      [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
        value = 0x1000;
        return true;
      },
      [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
      0};
  diagnostic::DrawCaptureFrameInputDrawView draw;
  draw.index.present = false;
  FrameInputDrawBuilderResult result;
  REQUIRE(FrameInputDrawBuilder::BuildPrepared(input, result, draw));
  CHECK(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  CHECK(result.textures.empty());

  input.guest_pixel_shader = &textured_pixel;
  draw = {};
  draw.index.present = false;
  REQUIRE_FALSE(FrameInputDrawBuilder::BuildPrepared(input, result, draw));
  CHECK_FALSE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
  CHECK_FALSE(result.texture_bindings_complete);
  CHECK_FALSE(result.textures.empty());
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

TEST_CASE("Prepared native source sideband rejects writes and overlapping plans",
          "[graphics][prepared][native]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(heap);
  const uint32_t page = uint32_t(rex::memory::page_size());
  uint32_t guest = 0;
  REQUIRE(heap->Alloc(
      page, page, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &guest));
  const uint32_t address = memory.GetPhysicalAddress(guest);
  {
    rex::graphics::FrameInputNativeSources sources(memory, shared);
    REQUIRE(sources.Arm(address, 16));
    CHECK_FALSE(sources.Complete(address, 16, false));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    sources.Reset();

    REQUIRE(sources.Arm(address, 16));
    CHECK_FALSE(sources.Arm(address + 8, 16));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    sources.Reset();

    REQUIRE(sources.Arm(address, 16));
    REQUIRE(sources.Complete(address, 16, true));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kUnresolved);
    sources.Reset();

    REQUIRE(sources.Arm(address, 16));
    REQUIRE(sources.Complete(address, 16, true));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kUnresolved);
    *memory.TranslateVirtual<volatile uint32_t*>(guest) = 1;
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    sources.Reset();

    REQUIRE(sources.Arm(address, 16));
    REQUIRE(sources.Complete(address, 16, true));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kUnresolved);
    shared.RangeWrittenByGpu(address, 16);
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    sources.Reset();

    REQUIRE(sources.Arm(address, 16));
    REQUIRE(sources.Complete(address, 16, true));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kUnresolved);
    REQUIRE(heap->Protect(guest, page, rex::memory::kMemoryProtectNoAccess));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    REQUIRE(heap->Protect(guest, page,
                          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    sources.Reset();

    REQUIRE(sources.Arm(address, 16));
    REQUIRE(sources.Complete(address, 16, true));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kUnresolved);
    shared.ClearCache();
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);

    sources.Reset();
    for (size_t index = 0; index < rex::graphics::FrameInputNativeSources::kMaxRanges; ++index) {
      REQUIRE(sources.Arm(address + uint32_t(index * 32), 16));
    }
    CHECK_FALSE(sources.Arm(
        address + uint32_t(rex::graphics::FrameInputNativeSources::kMaxRanges * 32), 16));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    // Once the sideband reaches its sticky cap, unrelated ranges must fail
    // closed rather than fall through to stale CPU bytes.
    CHECK(sources.Classify(
              address + uint32_t(rex::graphics::FrameInputNativeSources::kMaxRanges * 32), 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    sources.Reset();
    REQUIRE(sources.Arm(address, 16));
    REQUIRE(sources.Complete(address, 16, true));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kUnresolved);
    sources.Reset();
    REQUIRE(sources.Arm(address, 16));
    REQUIRE(sources.Complete(address, 16, true));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kUnresolved);
    REQUIRE(heap->Decommit(guest, page));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
  }
  REQUIRE(heap->Release(guest, nullptr));
}

TEST_CASE("Prepared native rejection coverage survives source replacement",
          "[graphics][prepared][native]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(heap);
  const uint32_t page = uint32_t(rex::memory::page_size());
  uint32_t guest = 0;
  REQUIRE(heap->Alloc(
      page, page, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &guest));
  const uint32_t address = memory.GetPhysicalAddress(guest);

  rex::graphics::FrameInputNativeSourceRejections rejections;
  {
    rex::graphics::FrameInputNativeSources sources(memory, shared, &rejections);
    REQUIRE(sources.Arm(address, 16));
    REQUIRE(sources.Complete(address, 16, true));
    CHECK(sources.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kUnresolved);
  }
  // Retiring the borrowed source records its range. A changed-size fetch is
  // rejected even though no new source owner has been constructed yet.
  CHECK(rejections.Overlaps(address, 16));
  CHECK(rejections.Overlaps(address, 32));

  {
    rex::graphics::FrameInputNativeSources replacement(memory, shared, &rejections);
    CHECK(replacement.Classify(address, 32) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    REQUIRE(replacement.Arm(address, 16));
    REQUIRE(replacement.Complete(address, 16, true));
    // The active exact generation is still eligible; the persistent bank does
    // not turn a mismatched alias into a native source.
    CHECK_FALSE(rejections.Overlaps(address, 16));
    CHECK(replacement.Classify(address, 16) ==
          rex::graphics::FrameInputNativeSources::State::kUnresolved);
    CHECK(replacement.Classify(address + 4, 4) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
  }
  // The successful generation's exact temporary rejection is retired again
  // before its owner disappears, so a future observer cannot use CPU bytes.
  CHECK(rejections.Overlaps(address, 16));

  rex::graphics::FrameInputNativeSourceRejections sticky;
  {
    rex::graphics::FrameInputNativeSources sources(memory, shared, &sticky);
    REQUIRE(sources.Arm(address, 16));
    CHECK_FALSE(sources.Arm(address + 8, 16));
  }
  // A crossing plan rejects only the overlapping ranges; an unrelated range
  // stays usable, while the crossed bytes never fall back to CPU bytes.
  CHECK(sticky.Overlaps(address + 8, 16));
  CHECK_FALSE(sticky.Overlaps(address + page, 16));

  // A later failed plan can extend an earlier rejection. Its non-overlapping
  // tail must remain closed when a new observer changes its fetch range.
  rex::graphics::FrameInputNativeSourceRejections failed_plans;
  REQUIRE(failed_plans.Add(address, 16));
  REQUIRE(failed_plans.Add(address + 8, 32));
  {
    rex::graphics::FrameInputNativeSources replacement(memory, shared, &failed_plans);
    CHECK(replacement.Classify(address + 32, 4) ==
          rex::graphics::FrameInputNativeSources::State::kRejected);
    CHECK(replacement.Classify(address + page, 16) ==
          rex::graphics::FrameInputNativeSources::State::kNone);
  }

  REQUIRE(heap->Release(guest, nullptr));
}

#ifdef REX_TEST_DXBC_TRANSLATION
TEST_CASE("Prepared constructor emits an unresolved native source after analysis",
          "[graphics][prepared][native]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  for (const uint32_t fetch_constant : {2U, 7U}) {
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    fetch.size_2d.width = 31;
    fetch.size_2d.height = 31;
    fetch.pitch = 1;
    fetch.base_address = 16;
    fetch.format = xenos::TextureFormat::k_8_8_8_8;
    fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w = xenos::TextureSign::kUnsigned;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &fetch, sizeof(fetch));
  }

  diagnostic::DrawCaptureFrameInputDrawView draw;
  FrameInputDrawBuilder::PopulateShaderViews(&vertex, &pixel, draw);
  REQUIRE(draw.vertex_shader.present);
  REQUIRE(draw.pixel_shader.present);
  CHECK(draw.vertex_shader.hash == vertex.ucode_data_hash());
  CHECK(draw.pixel_shader.hash == pixel.ucode_data_hash());
  CHECK(draw.vertex_shader.words.size() == vertex.ucode_dword_count());
  CHECK(draw.pixel_shader.words.size() == pixel.ucode_dword_count());
  draw.index.present = false;
  uint32_t sampler_calls = 0;
  FrameInputDrawBuilderInput input{
      registers,
      cache,
      &vertex,
      &pixel,
      &vertex,
      &pixel,
      {},
      [](uint32_t, uint32_t) { return false; },
      [](uint32_t, uint32_t) { return false; },
      [](uint32_t, uint32_t) { return true; },
      [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
      [&sampler_calls](const Shader::PreparedSamplerBinding& binding, uint32_t& value) {
        ++sampler_calls;
        value = 0x1000 + binding.fetch_constant;
        return true;
      },
      [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kTextureUnresolved; },
      0};
  FrameInputDrawBuilderResult result;
  REQUIRE(FrameInputDrawBuilder::BuildPrepared(input, result, draw));
  CHECK(sampler_calls != 0);
  CHECK_FALSE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureUnresolved);
  CHECK_FALSE(result.textures.empty());
  const auto unresolved =
      std::find_if(result.textures.begin(), result.textures.end(), [](const auto& view) {
        return view.required && !view.available &&
               view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureUnresolved;
      });
  REQUIRE(unresolved != result.textures.end());
  CHECK_FALSE(unresolved->gpu_written);
  CHECK_FALSE(unresolved->memory_version.identity);
  CHECK_FALSE(unresolved->memory_version.generation);
  CHECK(unresolved->fetch_words.size() == 6);
  CHECK(unresolved->width == 32);
  CHECK(unresolved->height == 32);
  CHECK(unresolved->depth == 1);
  CHECK(unresolved->pitch == 1);
  CHECK(unresolved->swizzled_signs == 0);

  bool actual_gpu_written = true;
  input.gpu_written = [&actual_gpu_written](uint32_t, uint32_t) { return actual_gpu_written; };
  diagnostic::DrawCaptureFrameInputDrawView gpu_draw;
  gpu_draw.vertex_shader.present = true;
  gpu_draw.pixel_shader.present = true;
  gpu_draw.index.present = false;
  FrameInputDrawBuilderResult gpu_result;
  REQUIRE(FrameInputDrawBuilder::BuildPrepared(input, gpu_result, gpu_draw));
  const auto gpu_unresolved =
      std::find_if(gpu_result.textures.begin(), gpu_result.textures.end(), [](const auto& view) {
        return view.required && !view.available &&
               view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureUnresolved;
      });
  REQUIRE(gpu_unresolved != gpu_result.textures.end());
  CHECK(gpu_unresolved->gpu_written);
  CHECK_FALSE(gpu_unresolved->memory_version.identity);
  CHECK_FALSE(gpu_unresolved->memory_version.generation);

  const auto unsigned_registers = registers;
  for (const uint32_t fetch_constant : {2U, 7U}) {
    auto fetch = registers.GetTextureFetch(fetch_constant);
    fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w = xenos::TextureSign::kSigned;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &fetch, sizeof(fetch));
  }
  actual_gpu_written = false;
  diagnostic::DrawCaptureFrameInputDrawView signed_draw;
  signed_draw.vertex_shader.present = true;
  signed_draw.pixel_shader.present = true;
  FrameInputDrawBuilderResult signed_result;
  CHECK_FALSE(FrameInputDrawBuilder::BuildPrepared(input, signed_result, signed_draw));
  CHECK(signed_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);

  // Changing the sign interpretation does not make rejected native bytes a
  // CPU payload, even when the original resolve never set a GPU page flag.
  rex::graphics::FrameInputNativeSourceRejections rejected_ranges;
  for (const auto& view : result.textures) {
    if (view.required) {
      REQUIRE(rejected_ranges.Add(view.base_address, view.base_size));
    }
  }
  rex::graphics::FrameInputNativeSources replacement(memory, shared, &rejected_ranges);
  input.native_source = [&replacement](uint32_t address, uint32_t size) {
    return replacement.Classify(address, size) ==
                   rex::graphics::FrameInputNativeSources::State::kRejected
               ? diagnostic::DrawCaptureReason::kTextureSource
               : diagnostic::DrawCaptureReason::kNone;
  };
  CHECK_FALSE(FrameInputDrawBuilder::BuildPrepared(input, signed_result, signed_draw));
  CHECK(signed_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
  registers = unsigned_registers;
}

TEST_CASE("Prepared optional signed companion failure preserves unsigned completeness",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  for (const uint32_t fetch_constant : {2U, 7U}) {
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    fetch.size_2d.width = 31;
    fetch.size_2d.height = 31;
    fetch.pitch = 1;
    fetch.base_address = 16;
    fetch.format = xenos::TextureFormat::k_8_8_8_8;
    fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w = xenos::TextureSign::kUnsigned;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &fetch, sizeof(fetch));
  }

  uint32_t prepare_calls = 0;
  uint32_t optional_prepare_failures = 0;
  uint32_t gpu_written_calls = 0;
  uint32_t readable_calls = 0;
  uint32_t version_calls = 0;
  uint32_t native_source_calls = 0;
  FrameInputDrawBuilderInput input{registers,
                                   cache,
                                   &vertex,
                                   &pixel,
                                   &vertex,
                                   &pixel,
                                   {},
                                   [](uint32_t, uint32_t) { return false; },
                                   [&gpu_written_calls](uint32_t, uint32_t) {
                                     ++gpu_written_calls;
                                     return false;
                                   },
                                   [&readable_calls](uint32_t, uint32_t) {
                                     ++readable_calls;
                                     return true;
                                   },
                                   [&version_calls](uint32_t, uint32_t) {
                                     ++version_calls;
                                     return diagnostic::DrawCaptureGuestMemoryVersion{};
                                   },
                                   [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
                                     value = 0x1000;
                                     return true;
                                   },
                                   [&native_source_calls](uint32_t, uint32_t) {
                                     ++native_source_calls;
                                     return diagnostic::DrawCaptureReason::kNone;
                                   },
                                   0};
  input.prepare_texture = [&cache, &prepare_calls, &optional_prepare_failures](
                              const xenos::xe_gpu_texture_fetch_t& fetch,
                              diagnostic::DrawCaptureFrameInputTextureView& view) {
    ++prepare_calls;
    if (view.is_signed && !view.required) {
      ++optional_prepare_failures;
      view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureFormat;
      return false;
    }
    return cache.PrepareFrameInputTexture(fetch, view);
  };

  diagnostic::DrawCaptureFrameInputDrawView draw;
  draw.index.present = false;
  FrameInputDrawBuilderResult result;
  REQUIRE(FrameInputDrawBuilder::BuildPrepared(input, result, draw));
  CHECK(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  CHECK_FALSE(result.non_native_texture_incomplete);
  CHECK(prepare_calls == 8);
  CHECK(optional_prepare_failures == 4);
  CHECK(gpu_written_calls == 8);
  CHECK(readable_calls == 8);
  CHECK(version_calls == 0);
  CHECK(native_source_calls == 4);
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);
  REQUIRE(result.textures.size() == 8);

  const std::array<uint32_t, 4> expected_fetches{7, 7, 2, 2};
  for (size_t stage = 0; stage < 2; ++stage) {
    for (size_t binding = 0; binding < expected_fetches.size(); ++binding) {
      const auto& view = result.textures[stage * expected_fetches.size() + binding];
      CHECK(view.stage == stage);
      CHECK(view.shader_binding_index == binding);
      CHECK(view.fetch_constant == expected_fetches[binding]);
      CHECK(view.fetch_words.size() == 6);
      if (view.is_signed) {
        CHECK_FALSE(view.required);
        CHECK_FALSE(view.available);
        CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureFormat);
        CHECK_FALSE(view.memory_version.identity);
        CHECK_FALSE(view.memory_version.generation);
      } else {
        CHECK(view.required);
        CHECK(view.available);
        CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
      }
    }
  }
}
#endif

#ifdef REX_TEST_DXBC_TRANSLATION
TEST_CASE("Prepared signed RG16 owner bypasses only its exact dirty range",
          "[graphics][prepared][native]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  for (const uint32_t fetch_constant : {2U, 7U}) {
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    fetch.size_2d.width = 1279;
    fetch.size_2d.height = 719;
    fetch.pitch = 40;
    fetch.base_address = 16;
    fetch.format = xenos::TextureFormat::k_16_16_FLOAT;
    fetch.tiled = true;
    fetch.swizzle = 0xB48;
    fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w = xenos::TextureSign::kSigned;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &fetch, sizeof(fetch));
  }

  auto build = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                   std::function<diagnostic::DrawCaptureReason(uint32_t, uint32_t)> native_source,
                   diagnostic::DrawCaptureFrameInputDrawView& draw,
                   FrameInputDrawBuilderResult& result) {
    draw = {};
    draw.vertex_shader.present = true;
    draw.pixel_shader.present = true;
    draw.index.present = false;
    FrameInputDrawBuilderInput input{
        registers,
        cache,
        &vertex,
        &pixel,
        &vertex,
        &pixel,
        {},
        std::move(dirty),
        [](uint32_t, uint32_t) { return false; },
        [](uint32_t, uint32_t) { return true; },
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
        [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0x1000;
          return true;
        },
        std::move(native_source),
        0};
    return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
  };

  diagnostic::DrawCaptureFrameInputDrawView native_draw;
  FrameInputDrawBuilderResult native_result;
  const bool native_build =
      build([](uint32_t, uint32_t) { return true; },
            [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kTextureUnresolved; },
            native_draw, native_result);
  REQUIRE(native_build);
  CHECK_FALSE(native_draw.cpu_data_complete);
  CHECK(native_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureUnresolved);
  CHECK(native_result.native_texture_unresolved);
  const auto selected = std::find_if(
      native_result.textures.begin(), native_result.textures.end(), [](const auto& view) {
        return view.format == static_cast<uint32_t>(xenos::TextureFormat::k_16_16_FLOAT);
      });
  REQUIRE(selected != native_result.textures.end());
  CHECK(selected->base_size == 0x398000);
  CHECK(selected->swizzled_signs == 0x55);

  // Shader::PrepareTextureBindings emits signed/unsigned candidates per fetch.
  // The unsigned candidate is optional only when the actual selected fetch is
  // complete signed RG16; unsigned/mixed-sign fetches require it.
  size_t signed_owners = 0;
  size_t unsigned_companions = 0;
  for (const auto& view : native_result.textures) {
    if (view.format != static_cast<uint32_t>(xenos::TextureFormat::k_16_16_FLOAT)) {
      continue;
    }
    if (view.is_signed) {
      ++signed_owners;
      CHECK(view.required);
    } else {
      ++unsigned_companions;
      CHECK_FALSE(view.required);
      CHECK(view.base_size == 0x398000);
    }
  }
  CHECK(signed_owners != 0);
  CHECK(unsigned_companions != 0);

  const auto signed_registers = registers;
  for (const uint32_t fetch_constant : {2U, 7U}) {
    auto fetch = registers.GetTextureFetch(fetch_constant);
    fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w = xenos::TextureSign::kUnsigned;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &fetch, sizeof(fetch));
  }
  diagnostic::DrawCaptureFrameInputDrawView unsigned_draw;
  FrameInputDrawBuilderResult unsigned_result;
  CHECK_FALSE(build([](uint32_t, uint32_t) { return false; },
                    [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
                    unsigned_draw, unsigned_result));
  CHECK(unsigned_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
  const auto required_unsigned = std::find_if(
      unsigned_result.textures.begin(), unsigned_result.textures.end(),
      [](const auto& view) { return !view.is_signed && view.required && !view.available; });
  REQUIRE(required_unsigned != unsigned_result.textures.end());
  const auto optional_signed = std::find_if(
      unsigned_result.textures.begin(), unsigned_result.textures.end(),
      [](const auto& view) { return view.is_signed && !view.required && !view.available; });
  REQUIRE(optional_signed != unsigned_result.textures.end());
  CHECK(optional_signed->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);

  registers = signed_registers;
  for (const uint32_t fetch_constant : {2U, 7U}) {
    auto fetch = registers.GetTextureFetch(fetch_constant);
    fetch.sign_y = xenos::TextureSign::kUnsignedBiased;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &fetch, sizeof(fetch));
  }
  diagnostic::DrawCaptureFrameInputDrawView mixed_draw;
  FrameInputDrawBuilderResult mixed_result;
  CHECK_FALSE(build([](uint32_t, uint32_t) { return false; },
                    [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
                    mixed_draw, mixed_result));
  CHECK(mixed_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
  const auto required_mixed = std::find_if(
      mixed_result.textures.begin(), mixed_result.textures.end(),
      [](const auto& view) { return !view.is_signed && view.required && !view.available; });
  REQUIRE(required_mixed != mixed_result.textures.end());
  const auto required_mixed_signed =
      std::find_if(mixed_result.textures.begin(), mixed_result.textures.end(),
                   [](const auto& view) { return view.is_signed && view.required; });
  REQUIRE(required_mixed_signed != mixed_result.textures.end());
  registers = signed_registers;

  diagnostic::DrawCaptureFrameInputDrawView dirty_draw;
  FrameInputDrawBuilderResult dirty_result;
  CHECK_FALSE(build([](uint32_t, uint32_t) { return true; },
                    [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
                    dirty_draw, dirty_result));
  CHECK(dirty_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);

  // A fail-closed dirty-all ledger is reported as a rejected native source;
  // retaining the old bytes must not turn that state into an RG16 owner.
  diagnostic::DrawCaptureFrameInputDrawView dirty_all_draw;
  FrameInputDrawBuilderResult dirty_all_result;
  CHECK_FALSE(
      build([](uint32_t, uint32_t) { return true; },
            [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kTextureSource; },
            dirty_all_draw, dirty_all_result));
  CHECK(dirty_all_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);

  // A signed binding with biased metadata is not admitted as the float owner.
  for (const uint32_t fetch_constant : {2U, 7U}) {
    auto fetch = registers.GetTextureFetch(fetch_constant);
    fetch.sign_y = xenos::TextureSign::kUnsignedBiased;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &fetch, sizeof(fetch));
  }
  diagnostic::DrawCaptureFrameInputDrawView mismatch_draw;
  FrameInputDrawBuilderResult mismatch_result;
  CHECK_FALSE(
      build([](uint32_t, uint32_t) { return true; },
            [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kTextureUnresolved; },
            mismatch_draw, mismatch_result));
  CHECK(mismatch_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
}

TEST_CASE("Prepared unsigned 64x64 BC1 admits its canonical packed mip footprint",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  constexpr std::array<uint32_t, 6> kSelectedBc1Fetch{0x01000002, 0x155CB052, 0x000FE07F,
                                                      0x00A80D10, 0x000001C3, 0x155C5A18};
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                kSelectedBc1Fetch.data(), sizeof(kSelectedBc1Fetch));
  }
  auto selected_fetch = registers.GetTextureFetch(2);
  selected_fetch.size_2d.width = 63;
  selected_fetch.size_2d.height = 63;
  selected_fetch.pitch = 4;
  selected_fetch.mip_max_level = 6;
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &selected_fetch, sizeof(selected_fetch));
  }
  selected_fetch = registers.GetTextureFetch(2);

  TextureInfo legacy;
  REQUIRE(TextureInfo::Prepare(selected_fetch, &legacy));
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(selected_fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  CHECK(legacy.memory.base_size == 0x1000);
  CHECK(legacy.memory.mip_size == 0x2000);
  CHECK(layout.base.level_data_extent_bytes == 0xF80);
  CHECK(layout.base.row_pitch_bytes == 256);
  CHECK(layout.mips_total_extent_bytes == 0x2340);
  CHECK(layout.packed_level == 2);
  CHECK(layout.mip_offsets_bytes[1] == 0);
  CHECK(layout.mip_offsets_bytes[2] == 0x2000);
  CHECK(layout.mips[1].array_slice_stride_bytes == 0x2000);
  CHECK(layout.mips[1].array_slice_data_extent_bytes == 0x740);
  CHECK(layout.mips[2].row_pitch_bytes == 256);
  CHECK(layout.mips[2].x_extent_blocks == 8);
  CHECK(layout.mips[2].y_extent_blocks == 4);
  CHECK(layout.mips[2].array_slice_data_extent_bytes == 0x340);
  CHECK(layout.mips[2].level_data_extent_bytes == 0x340);
  for (uint32_t level = 0; level <= 6; ++level) {
    const auto& level_layout =
        level == 0 ? layout.base : (level == 1 ? layout.mips[1] : layout.mips[layout.packed_level]);
    CHECK(level_layout.row_pitch_bytes == 256);
  }
  constexpr std::array<uint32_t, 7> kMipOffsets{0, 0, 0x2020, 0x2010, 0x2008, 0x2200, 0x2100};
  for (uint32_t level = 0; level < layout.packed_level; ++level) {
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    const uint32_t location = legacy.GetMipLocation(level, &offset_x, &offset_y, true);
    const uint32_t bank_address =
        level == 0 ? legacy.memory.base_address : legacy.memory.mip_address;
    CHECK(location - bank_address == kMipOffsets[level]);
    CHECK(offset_x == 0);
    CHECK(offset_y == 0);
  }
  constexpr std::array<uint32_t, 5> kPackedX{4, 2, 1, 0, 0};
  constexpr std::array<uint32_t, 5> kPackedY{0, 0, 0, 2, 1};
  for (uint32_t level = layout.packed_level; level < kMipOffsets.size(); ++level) {
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    uint32_t offset_z = 0;
    REQUIRE(texture_util::GetPackedMipOffset(64, 64, 1, xenos::TextureFormat::k_DXT1, level,
                                             offset_x, offset_y, offset_z));
    CHECK(offset_z == 0);
    CHECK(layout.mip_offsets_bytes[layout.packed_level] +
              offset_y * layout.mips[layout.packed_level].row_pitch_bytes + offset_x * 8 ==
          kMipOffsets[level]);
    CHECK(offset_x == kPackedX[level - layout.packed_level]);
    CHECK(offset_y == kPackedY[level - layout.packed_level]);
  }

  auto build = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                   std::function<bool(uint32_t, uint32_t)> gpu_written,
                   std::function<bool(uint32_t, uint32_t)> readable,
                   diagnostic::DrawCaptureFrameInputDrawView& draw,
                   FrameInputDrawBuilderResult& result) {
    FrameInputDrawBuilderInput input{
        registers,
        cache,
        &vertex,
        &pixel,
        &vertex,
        &pixel,
        {},
        std::move(dirty),
        std::move(gpu_written),
        std::move(readable),
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
        [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0xF880;
          return true;
        },
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
        0};
    draw = {};
    draw.index.present = false;
    return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
  };

  DrawCaptureFrameInputTextureView prepared_view;
  REQUIRE(cache.PrepareFrameInputTexture(selected_fetch, prepared_view));
  CHECK(prepared_view.available);
  CHECK_FALSE(prepared_view.is_signed);
  CHECK(prepared_view.required);
  CHECK(prepared_view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT1));
  CHECK(prepared_view.endianness == static_cast<uint32_t>(xenos::Endian::k8in16));
  CHECK(prepared_view.dimension == static_cast<uint32_t>(xenos::DataDimension::k2DOrStacked));
  CHECK(prepared_view.width == selected_fetch.size_2d.width);
  CHECK(prepared_view.height == selected_fetch.size_2d.height);
  CHECK(prepared_view.pitch == selected_fetch.pitch << 5);
  CHECK(prepared_view.mip_min_level == 0);
  CHECK(prepared_view.mip_max_level == 6);
  CHECK_FALSE(prepared_view.tiled);
  CHECK(prepared_view.packed_mips);
  CHECK(prepared_view.base_address == key.base_page << 12);
  CHECK(prepared_view.base_size == 0x1000);
  CHECK(prepared_view.mip_address == key.mip_page << 12);
  CHECK(prepared_view.mip_size == 0x2340);
  CHECK(prepared_view.swizzled_signs == 0);
  CHECK_FALSE(prepared_view.memory_version.identity);
  CHECK_FALSE(prepared_view.memory_version.generation);
  CHECK_FALSE(prepared_view.memory_version.check);

  diagnostic::DrawCaptureFrameInputDrawView draw;
  FrameInputDrawBuilderResult result;
  REQUIRE(build([](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
                [](uint32_t, uint32_t) { return true; }, draw, result));
  REQUIRE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  REQUIRE(result.textures.size() == 8);

  size_t required_unsigned_count = 0;
  size_t optional_signed_count = 0;
  for (const auto& view : result.textures) {
    if (!view.is_signed && view.required) {
      ++required_unsigned_count;
      CHECK(view.available);
      CHECK(view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT1));
      CHECK(view.base_address == key.base_page << 12);
      CHECK(view.base_size == 0x1000);
      CHECK(view.mip_address == key.mip_page << 12);
      CHECK(view.mip_size == 0x2340);
      CHECK(view.width == 64);
      CHECK(view.height == 64);
      CHECK(view.pitch == 4);
      CHECK(view.mip_max_level == 6);
      CHECK(view.fetch_words.size() == 6);
      CHECK(view.swizzled_signs == 0);
      CHECK_FALSE(view.memory_version.identity);
      CHECK_FALSE(view.memory_version.generation);
      CHECK_FALSE(view.memory_version.check);
    } else if (view.is_signed && !view.required) {
      ++optional_signed_count;
      CHECK_FALSE(view.available);
      CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
      CHECK_FALSE(view.memory_version.identity);
      CHECK_FALSE(view.memory_version.generation);
      CHECK_FALSE(view.memory_version.check);
    }
  }
  CHECK(required_unsigned_count == 4);
  CHECK(optional_signed_count == 4);
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);

  const uint32_t base_address = key.base_page << 12;
  const uint32_t mip_address = key.mip_page << 12;
  const uint32_t tail_address = mip_address + 0x2000;
  const uint32_t tail_end = mip_address + 0x2340;
  const auto overlaps_tail = [tail_address, tail_end](uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    return end > tail_address && address < tail_end;
  };
  CHECK_FALSE(overlaps_tail(mip_address, 0x2000));
  CHECK(overlaps_tail(mip_address, 0x2340));
  const auto expect_texture_source_rejection =
      [&](std::function<bool(uint32_t, uint32_t)> dirty,
          std::function<bool(uint32_t, uint32_t)> gpu_written,
          std::function<bool(uint32_t, uint32_t)> readable) {
        diagnostic::DrawCaptureFrameInputDrawView rejected_draw;
        FrameInputDrawBuilderResult rejected_result;
        CHECK_FALSE(build(std::move(dirty), std::move(gpu_written), std::move(readable),
                          rejected_draw, rejected_result));
        CHECK_FALSE(rejected_draw.cpu_data_complete);
        CHECK(rejected_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
        const auto required =
            std::find_if(rejected_result.textures.begin(), rejected_result.textures.end(),
                         [](const auto& view) { return !view.is_signed && view.required; });
        REQUIRE(required != rejected_result.textures.end());
        CHECK_FALSE(required->available);
        CHECK(required->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
        CHECK(required->base_address == base_address);
        CHECK(required->base_size == 0x1000);
        CHECK(required->mip_address == mip_address);
        CHECK(required->mip_size == 0x2340);
      };
  expect_texture_source_rejection(
      [base_address](uint32_t address, uint32_t) { return address == base_address; },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  expect_texture_source_rejection(
      [mip_address](uint32_t address, uint32_t) { return address == mip_address; },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  expect_texture_source_rejection(
      [](uint32_t, uint32_t) { return false; },
      [base_address](uint32_t address, uint32_t) { return address == base_address; },
      [](uint32_t, uint32_t) { return true; });
  expect_texture_source_rejection(
      [](uint32_t, uint32_t) { return false; },
      [mip_address](uint32_t address, uint32_t) { return address == mip_address; },
      [](uint32_t, uint32_t) { return true; });
  expect_texture_source_rejection(
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
      [mip_address](uint32_t address, uint32_t) { return address != mip_address; });

  const auto expect_tail_rejection = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                                         std::function<bool(uint32_t, uint32_t)> gpu_written,
                                         std::function<bool(uint32_t, uint32_t)> readable) {
    diagnostic::DrawCaptureFrameInputDrawView rejected_draw;
    FrameInputDrawBuilderResult rejected_result;
    CHECK_FALSE(build(std::move(dirty), std::move(gpu_written), std::move(readable), rejected_draw,
                      rejected_result));
    CHECK_FALSE(rejected_draw.cpu_data_complete);
    CHECK(rejected_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    const auto required =
        std::find_if(rejected_result.textures.begin(), rejected_result.textures.end(),
                     [](const auto& view) { return !view.is_signed && view.required; });
    REQUIRE(required != rejected_result.textures.end());
    CHECK_FALSE(required->available);
    CHECK(required->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    CHECK(required->base_size == 0x1000);
    CHECK(required->mip_size == 0x2340);
  };
  bool dirty_tail_seen = false;
  bool dirty_tail_size_valid = true;
  expect_tail_rejection(
      [&dirty_tail_seen, &dirty_tail_size_valid, &overlaps_tail, mip_address](uint32_t address,
                                                                              uint32_t size) {
        if (address == mip_address && overlaps_tail(address, size)) {
          dirty_tail_seen = true;
          dirty_tail_size_valid &= size == 0x2340;
          return true;
        }
        return false;
      },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  CHECK(dirty_tail_seen);
  CHECK(dirty_tail_size_valid);
  bool gpu_tail_seen = false;
  bool gpu_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [&gpu_tail_seen, &gpu_tail_size_valid, &overlaps_tail, mip_address](
                            uint32_t address, uint32_t size) {
                          if (address == mip_address && overlaps_tail(address, size)) {
                            gpu_tail_seen = true;
                            gpu_tail_size_valid &= size == 0x2340;
                            return true;
                          }
                          return false;
                        },
                        [](uint32_t, uint32_t) { return true; });
  CHECK(gpu_tail_seen);
  CHECK(gpu_tail_size_valid);
  bool unreadable_tail_seen = false;
  bool unreadable_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [](uint32_t, uint32_t) { return false; },
                        [&unreadable_tail_seen, &unreadable_tail_size_valid, &overlaps_tail,
                         mip_address](uint32_t address, uint32_t size) {
                          if (address == mip_address && overlaps_tail(address, size)) {
                            unreadable_tail_seen = true;
                            unreadable_tail_size_valid &= size == 0x2340;
                            return false;
                          }
                          return true;
                        });
  CHECK(unreadable_tail_seen);
  CHECK(unreadable_tail_size_valid);

  const auto reject_fetch = [&](const char* control, auto mutate) {
    CAPTURE(control);
    auto near_miss = selected_fetch;
    mutate(near_miss);
    DrawCaptureFrameInputTextureView view;
    CHECK_FALSE(cache.PrepareFrameInputTexture(near_miss, view));
    CHECK_FALSE(view.available);
  };
  reject_fetch("format", [](auto& fetch) { fetch.format = xenos::TextureFormat::k_DXN; });
  reject_fetch("width", [](auto& fetch) { fetch.size_2d.width = 127; });
  reject_fetch("height", [](auto& fetch) { fetch.size_2d.height = 127; });
  reject_fetch("pitch", [](auto& fetch) { fetch.pitch = 8; });
  reject_fetch("mip_max_level", [](auto& fetch) { fetch.mip_max_level = 7; });
  reject_fetch("endianness", [](auto& fetch) { fetch.endianness = xenos::Endian::kNone; });
  reject_fetch("swizzle", [](auto& fetch) { fetch.swizzle = 0x680; });
  reject_fetch("signed component", [](auto& fetch) { fetch.sign_y = xenos::TextureSign::kSigned; });
  reject_fetch("tiled", [](auto& fetch) { fetch.tiled = true; });
  reject_fetch("cube", [](auto& fetch) { fetch.dimension = xenos::DataDimension::kCube; });
  reject_fetch("stacked", [](auto& fetch) { fetch.stacked = true; });
  reject_fetch("base_address", [](auto& fetch) { fetch.base_address = 0; });
  reject_fetch("mip_min_level", [](auto& fetch) { fetch.mip_min_level = 1; });
  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(selected_fetch, signed_view));

  const auto ordinary_fetch = [&] {
    auto fetch = selected_fetch;
    fetch.packed_mips = false;
    return fetch;
  }();
  DrawCaptureFrameInputTextureView ordinary_view;
  REQUIRE(cache.PrepareFrameInputTexture(ordinary_fetch, ordinary_view));
  TextureInfo ordinary_info;
  REQUIRE(TextureInfo::Prepare(ordinary_fetch, &ordinary_info));
  TestTextureCache::TextureKey ordinary_key;
  TestTextureCache::BindingInfoFromFetchConstant(ordinary_fetch, ordinary_key, nullptr);
  const auto ordinary_layout = ordinary_key.GetGuestLayout();
  CHECK(ordinary_view.available);
  CHECK_FALSE(ordinary_view.packed_mips);
  CHECK(ordinary_view.base_size == ordinary_info.memory.base_size);
  CHECK(ordinary_view.mip_size == ordinary_info.memory.mip_size);
  CHECK(ordinary_view.base_size >= ordinary_layout.base.level_data_extent_bytes);
  CHECK(ordinary_view.mip_size >= ordinary_layout.mips_total_extent_bytes);
}

TEST_CASE("Prepared unsigned BC1 admits its canonical packed mip footprint",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  constexpr std::array<uint32_t, 6> kSelectedBc1Fetch{0x01000002, 0x155CB052, 0x000FE07F,
                                                      0x00A80D10, 0x000001C3, 0x155C5A18};
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                kSelectedBc1Fetch.data(), sizeof(kSelectedBc1Fetch));
  }

  const auto selected_fetch = registers.GetTextureFetch(2);
  TextureInfo legacy;
  REQUIRE(TextureInfo::Prepare(selected_fetch, &legacy));
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(selected_fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  CHECK(legacy.memory.base_size == 0x2000);
  CHECK(legacy.memory.mip_size == 0x4000);
  CHECK(layout.base.level_data_extent_bytes == 0x2000);
  CHECK(layout.mips_total_extent_bytes == 0x4340);

  auto build = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                   std::function<bool(uint32_t, uint32_t)> gpu_written,
                   std::function<bool(uint32_t, uint32_t)> readable,
                   diagnostic::DrawCaptureFrameInputDrawView& draw,
                   FrameInputDrawBuilderResult& result) {
    FrameInputDrawBuilderInput input{
        registers,
        cache,
        &vertex,
        &pixel,
        &vertex,
        &pixel,
        {},
        std::move(dirty),
        std::move(gpu_written),
        std::move(readable),
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
        [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0xF880;
          return true;
        },
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
        0};
    draw = {};
    draw.index.present = false;
    return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
  };

  DrawCaptureFrameInputTextureView prepared_view;
  REQUIRE(cache.PrepareFrameInputTexture(selected_fetch, prepared_view));
  CHECK(prepared_view.available);
  CHECK_FALSE(prepared_view.is_signed);
  CHECK(prepared_view.required);
  CHECK(prepared_view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT1));
  CHECK(prepared_view.endianness == static_cast<uint32_t>(xenos::Endian::k8in16));
  CHECK(prepared_view.width == selected_fetch.size_2d.width);
  CHECK(prepared_view.height == selected_fetch.size_2d.height);
  CHECK(prepared_view.pitch == selected_fetch.pitch << 5);
  CHECK(prepared_view.mip_min_level == 0);
  CHECK(prepared_view.mip_max_level == 7);
  CHECK_FALSE(prepared_view.tiled);
  CHECK(prepared_view.packed_mips);
  CHECK(prepared_view.base_address == key.base_page << 12);
  CHECK(prepared_view.base_size == 0x2000);
  CHECK(prepared_view.mip_address == key.mip_page << 12);
  CHECK(prepared_view.mip_size == 0x4340);
  CHECK(prepared_view.swizzled_signs == 0);

  diagnostic::DrawCaptureFrameInputDrawView draw;
  FrameInputDrawBuilderResult result;
  REQUIRE(build([](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
                [](uint32_t, uint32_t) { return true; }, draw, result));
  REQUIRE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  REQUIRE(result.textures.size() == 8);

  size_t required_unsigned_count = 0;
  size_t optional_signed_count = 0;
  for (const auto& view : result.textures) {
    if (!view.is_signed && view.required) {
      ++required_unsigned_count;
      CHECK(view.available);
      CHECK(view.base_size == 0x2000);
      CHECK(view.mip_size == 0x4340);
      CHECK(view.width == 128);
      CHECK(view.height == 128);
      CHECK(view.pitch == 4);
      CHECK(view.fetch_words.size() == 6);
      CHECK(view.swizzled_signs == 0);
    } else if (view.is_signed && !view.required) {
      ++optional_signed_count;
      CHECK_FALSE(view.available);
      CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
      CHECK_FALSE(view.memory_version.identity);
      CHECK_FALSE(view.memory_version.generation);
    }
  }
  CHECK(required_unsigned_count == 4);
  CHECK(optional_signed_count == 4);

  const auto expect_texture_source_rejection =
      [&](std::function<bool(uint32_t, uint32_t)> dirty,
          std::function<bool(uint32_t, uint32_t)> gpu_written,
          std::function<bool(uint32_t, uint32_t)> readable) {
        diagnostic::DrawCaptureFrameInputDrawView rejected_draw;
        FrameInputDrawBuilderResult rejected_result;
        CHECK_FALSE(build(std::move(dirty), std::move(gpu_written), std::move(readable),
                          rejected_draw, rejected_result));
        CHECK_FALSE(rejected_draw.cpu_data_complete);
        CHECK(rejected_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
        const auto required =
            std::find_if(rejected_result.textures.begin(), rejected_result.textures.end(),
                         [](const auto& view) { return !view.is_signed && view.required; });
        REQUIRE(required != rejected_result.textures.end());
        CHECK_FALSE(required->available);
        CHECK(required->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
      };
  const uint32_t base_address = key.base_page << 12;
  const uint32_t mip_address = key.mip_page << 12;
  expect_texture_source_rejection(
      [base_address](uint32_t address, uint32_t) { return address == base_address; },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  expect_texture_source_rejection(
      [mip_address](uint32_t address, uint32_t) { return address == mip_address; },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  expect_texture_source_rejection(
      [](uint32_t, uint32_t) { return false; },
      [base_address](uint32_t address, uint32_t) { return address == base_address; },
      [](uint32_t, uint32_t) { return true; });
  expect_texture_source_rejection(
      [](uint32_t, uint32_t) { return false; },
      [mip_address](uint32_t address, uint32_t) { return address == mip_address; },
      [](uint32_t, uint32_t) { return true; });
  expect_texture_source_rejection(
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
      [mip_address](uint32_t address, uint32_t) { return address != mip_address; });

  const auto reject_fetch = [&](auto mutate) {
    auto near_miss = selected_fetch;
    mutate(near_miss);
    DrawCaptureFrameInputTextureView view;
    CHECK_FALSE(cache.PrepareFrameInputTexture(near_miss, view));
    CHECK_FALSE(view.available);
  };
  reject_fetch([](auto& fetch) { fetch.tiled = true; });
  reject_fetch([](auto& fetch) { fetch.endianness = xenos::Endian::kNone; });
  reject_fetch([](auto& fetch) { fetch.swizzle = 0x680; });
  reject_fetch([](auto& fetch) { fetch.format = xenos::TextureFormat::k_8_8_8_8; });
  reject_fetch([](auto& fetch) { fetch.sign_y = xenos::TextureSign::kSigned; });
  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(selected_fetch, signed_view));
  reject_fetch([](auto& fetch) { fetch.dimension = xenos::DataDimension::kCube; });
  reject_fetch([](auto& fetch) { fetch.stacked = true; });
  reject_fetch([](auto& fetch) { fetch.base_address = 0; });
  reject_fetch([](auto& fetch) { fetch.mip_min_level = 1; });
  reject_fetch([](auto& fetch) { fetch.mip_max_level = 8; });
}

TEST_CASE("Prepared unsigned 256x256 BC1 admits its canonical packed mip footprint",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  constexpr std::array<uint32_t, 6> kSelectedBc1Fetch{0x01000002, 0x155CB052, 0x000FE07F,
                                                      0x00A80D10, 0x000001C3, 0x155C5A18};
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                kSelectedBc1Fetch.data(), sizeof(kSelectedBc1Fetch));
  }
  auto selected_fetch = registers.GetTextureFetch(2);
  selected_fetch.size_2d.width = 255;
  selected_fetch.size_2d.height = 255;
  selected_fetch.pitch = 8;
  selected_fetch.mip_max_level = 8;
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &selected_fetch, sizeof(selected_fetch));
  }
  selected_fetch = registers.GetTextureFetch(2);

  TextureInfo legacy;
  REQUIRE(TextureInfo::Prepare(selected_fetch, &legacy));
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(selected_fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  CHECK(legacy.memory.base_size == 0x8000);
  CHECK(legacy.memory.mip_size == 0x6000);
  CHECK(layout.base.level_data_extent_bytes == 0x8000);
  CHECK(layout.base.row_pitch_bytes == 512);
  CHECK(layout.mips_total_extent_bytes == 0x6340);
  CHECK(layout.packed_level == 4);
  CHECK(layout.mip_offsets_bytes[1] == 0);
  CHECK(layout.mip_offsets_bytes[2] == 0x2000);
  CHECK(layout.mip_offsets_bytes[3] == 0x4000);
  CHECK(layout.mip_offsets_bytes[4] == 0x6000);
  CHECK(layout.mip_offsets_bytes[5] == 0);
  CHECK(layout.mip_offsets_bytes[6] == 0);
  CHECK(layout.mip_offsets_bytes[7] == 0);
  CHECK(layout.mip_offsets_bytes[8] == 0);
  constexpr std::array<uint32_t, 9> kMipOffsets{0,      0,      0x2000, 0x4000, 0x6020,
                                                0x6010, 0x6008, 0x6200, 0x6100};
  for (uint32_t level = 0; level < 4; ++level) {
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    const uint32_t location = legacy.GetMipLocation(level, &offset_x, &offset_y, true);
    const uint32_t bank_address =
        level == 0 ? legacy.memory.base_address : legacy.memory.mip_address;
    CHECK(location - bank_address == kMipOffsets[level]);
    CHECK(offset_x == 0);
    CHECK(offset_y == 0);
  }
  for (uint32_t level = layout.packed_level; level < kMipOffsets.size(); ++level) {
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    uint32_t offset_z = 0;
    REQUIRE(texture_util::GetPackedMipOffset(256, 256, 1, xenos::TextureFormat::k_DXT1, level,
                                             offset_x, offset_y, offset_z));
    CHECK(offset_z == 0);
    CHECK(layout.mip_offsets_bytes[layout.packed_level] +
              offset_y * layout.mips[layout.packed_level].row_pitch_bytes + offset_x * 8 ==
          kMipOffsets[level]);
  }
  CHECK(layout.mips[1].row_pitch_bytes == 256);
  CHECK(layout.mips[1].array_slice_stride_bytes == 0x2000);
  CHECK(layout.mips[1].array_slice_data_extent_bytes == 0x2000);
  CHECK(layout.mips[2].row_pitch_bytes == 256);
  CHECK(layout.mips[2].array_slice_stride_bytes == 0x2000);
  CHECK(layout.mips[2].array_slice_data_extent_bytes == 0xF80);
  CHECK(layout.mips[3].row_pitch_bytes == 256);
  CHECK(layout.mips[3].array_slice_stride_bytes == 0x2000);
  CHECK(layout.mips[3].array_slice_data_extent_bytes == 0x740);
  CHECK(layout.mips[4].row_pitch_bytes == 256);
  CHECK(layout.mips[4].x_extent_blocks == 8);
  CHECK(layout.mips[4].y_extent_blocks == 4);
  CHECK(layout.mips[4].array_slice_data_extent_bytes == 0x340);
  CHECK(layout.mips[4].level_data_extent_bytes == 0x340);

  auto build = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                   std::function<bool(uint32_t, uint32_t)> gpu_written,
                   std::function<bool(uint32_t, uint32_t)> readable,
                   diagnostic::DrawCaptureFrameInputDrawView& draw,
                   FrameInputDrawBuilderResult& result) {
    FrameInputDrawBuilderInput input{
        registers,
        cache,
        &vertex,
        &pixel,
        &vertex,
        &pixel,
        {},
        std::move(dirty),
        std::move(gpu_written),
        std::move(readable),
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
        [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0xF880;
          return true;
        },
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
        0};
    draw = {};
    draw.index.present = false;
    return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
  };

  DrawCaptureFrameInputTextureView prepared_view;
  REQUIRE(cache.PrepareFrameInputTexture(selected_fetch, prepared_view));
  CHECK(prepared_view.available);
  CHECK_FALSE(prepared_view.is_signed);
  CHECK(prepared_view.required);
  CHECK(prepared_view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT1));
  CHECK(prepared_view.endianness == static_cast<uint32_t>(xenos::Endian::k8in16));
  CHECK(prepared_view.width == selected_fetch.size_2d.width);
  CHECK(prepared_view.height == selected_fetch.size_2d.height);
  CHECK(prepared_view.pitch == selected_fetch.pitch << 5);
  CHECK(prepared_view.mip_min_level == 0);
  CHECK(prepared_view.mip_max_level == 8);
  CHECK_FALSE(prepared_view.tiled);
  CHECK(prepared_view.packed_mips);
  CHECK(prepared_view.base_address == key.base_page << 12);
  CHECK(prepared_view.base_size == 0x8000);
  CHECK(prepared_view.mip_address == key.mip_page << 12);
  CHECK(prepared_view.mip_size == 0x6340);
  CHECK(prepared_view.swizzled_signs == 0);

  diagnostic::DrawCaptureFrameInputDrawView draw;
  FrameInputDrawBuilderResult result;
  REQUIRE(build([](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
                [](uint32_t, uint32_t) { return true; }, draw, result));
  REQUIRE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  REQUIRE(result.textures.size() == 8);
  size_t required_unsigned_count = 0;
  size_t optional_signed_count = 0;
  for (const auto& view : result.textures) {
    if (!view.is_signed && view.required) {
      ++required_unsigned_count;
      CHECK(view.available);
      CHECK(view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT1));
      CHECK(view.width == 256);
      CHECK(view.height == 256);
      CHECK(view.pitch == 8);
      CHECK(view.mip_max_level == 8);
      CHECK(view.base_size == 0x8000);
      CHECK(view.mip_size == 0x6340);
      CHECK(view.fetch_words.size() == 6);
      CHECK(view.swizzled_signs == 0);
    } else if (view.is_signed && !view.required) {
      ++optional_signed_count;
      CHECK_FALSE(view.available);
      CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
      CHECK_FALSE(view.memory_version.identity);
      CHECK_FALSE(view.memory_version.generation);
    }
  }
  CHECK(required_unsigned_count == 4);
  CHECK(optional_signed_count == 4);
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);

  const uint32_t mip_address = key.mip_page << 12;
  const uint32_t tail_address = mip_address + 0x6000;
  const uint32_t tail_end = mip_address + 0x6340;
  const auto overlaps_tail = [tail_address, tail_end](uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    return end > tail_address && address < tail_end;
  };
  CHECK_FALSE(overlaps_tail(mip_address, 0x6000));
  CHECK(overlaps_tail(mip_address, 0x6340));
  const auto expect_tail_rejection = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                                         std::function<bool(uint32_t, uint32_t)> gpu_written,
                                         std::function<bool(uint32_t, uint32_t)> readable) {
    diagnostic::DrawCaptureFrameInputDrawView rejected_draw;
    FrameInputDrawBuilderResult rejected_result;
    CHECK_FALSE(build(std::move(dirty), std::move(gpu_written), std::move(readable), rejected_draw,
                      rejected_result));
    CHECK_FALSE(rejected_draw.cpu_data_complete);
    CHECK(rejected_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    const auto required =
        std::find_if(rejected_result.textures.begin(), rejected_result.textures.end(),
                     [](const auto& view) { return !view.is_signed && view.required; });
    REQUIRE(required != rejected_result.textures.end());
    CHECK_FALSE(required->available);
    CHECK(required->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    CHECK(required->base_size == 0x8000);
    CHECK(required->mip_size == 0x6340);
  };
  bool dirty_tail_seen = false;
  bool dirty_tail_size_valid = true;
  expect_tail_rejection(
      [&dirty_tail_seen, &dirty_tail_size_valid, &overlaps_tail, mip_address](uint32_t address,
                                                                              uint32_t size) {
        if (address == mip_address && overlaps_tail(address, size)) {
          dirty_tail_seen = true;
          dirty_tail_size_valid &= address == mip_address && size == 0x6340;
          return true;
        }
        return false;
      },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  CHECK(dirty_tail_seen);
  CHECK(dirty_tail_size_valid);
  bool gpu_tail_seen = false;
  bool gpu_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [&gpu_tail_seen, &gpu_tail_size_valid, &overlaps_tail, mip_address](
                            uint32_t address, uint32_t size) {
                          if (address == mip_address && overlaps_tail(address, size)) {
                            gpu_tail_seen = true;
                            gpu_tail_size_valid &= address == mip_address && size == 0x6340;
                            return true;
                          }
                          return false;
                        },
                        [](uint32_t, uint32_t) { return true; });
  CHECK(gpu_tail_seen);
  CHECK(gpu_tail_size_valid);
  bool unreadable_tail_seen = false;
  bool unreadable_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [](uint32_t, uint32_t) { return false; },
                        [&unreadable_tail_seen, &unreadable_tail_size_valid, &overlaps_tail,
                         mip_address](uint32_t address, uint32_t size) {
                          if (address == mip_address && overlaps_tail(address, size)) {
                            unreadable_tail_seen = true;
                            unreadable_tail_size_valid &= address == mip_address && size == 0x6340;
                            return false;
                          }
                          return true;
                        });
  CHECK(unreadable_tail_seen);
  CHECK(unreadable_tail_size_valid);

  const auto reject_fetch = [&](const char* control, auto mutate) {
    CAPTURE(control);
    auto near_miss = selected_fetch;
    mutate(near_miss);
    DrawCaptureFrameInputTextureView view;
    CHECK_FALSE(cache.PrepareFrameInputTexture(near_miss, view));
    CHECK_FALSE(view.available);
  };
  reject_fetch("format", [](auto& fetch) { fetch.format = xenos::TextureFormat::k_DXN; });
  reject_fetch("width", [](auto& fetch) { fetch.size_2d.width = 127; });
  reject_fetch("height", [](auto& fetch) { fetch.size_2d.height = 63; });
  reject_fetch("pitch", [](auto& fetch) { fetch.pitch = 4; });
  reject_fetch("mip_max_level", [](auto& fetch) { fetch.mip_max_level = 7; });
  reject_fetch("endianness", [](auto& fetch) { fetch.endianness = xenos::Endian::kNone; });
  reject_fetch("swizzle", [](auto& fetch) { fetch.swizzle = 0x680; });
  reject_fetch("signed component", [](auto& fetch) { fetch.sign_y = xenos::TextureSign::kSigned; });
  reject_fetch("tiled", [](auto& fetch) { fetch.tiled = true; });
  const auto expect_ordinary_fetch = [&](const char* control, auto mutate) {
    CAPTURE(control);
    auto ordinary_fetch = selected_fetch;
    mutate(ordinary_fetch);
    DrawCaptureFrameInputTextureView ordinary_view;
    REQUIRE(cache.PrepareFrameInputTexture(ordinary_fetch, ordinary_view));
    TextureInfo ordinary_info;
    REQUIRE(TextureInfo::Prepare(ordinary_fetch, &ordinary_info));
    TestTextureCache::TextureKey ordinary_key;
    TestTextureCache::BindingInfoFromFetchConstant(ordinary_fetch, ordinary_key, nullptr);
    const auto ordinary_layout = ordinary_key.GetGuestLayout();
    CHECK(ordinary_view.available);
    CHECK_FALSE(ordinary_view.packed_mips);
    CHECK(ordinary_view.base_size == ordinary_info.memory.base_size);
    CHECK(ordinary_view.mip_size == ordinary_info.memory.mip_size);
    CHECK(ordinary_view.base_size >= ordinary_layout.base.level_data_extent_bytes);
    CHECK(ordinary_view.mip_size >= ordinary_layout.mips_total_extent_bytes);
  };
  expect_ordinary_fetch("packed_mips", [](auto& fetch) { fetch.packed_mips = false; });
  reject_fetch("cube", [](auto& fetch) { fetch.dimension = xenos::DataDimension::kCube; });
  reject_fetch("stacked", [](auto& fetch) { fetch.stacked = true; });
  reject_fetch("base_address", [](auto& fetch) { fetch.base_address = 0; });
  reject_fetch("mip_min_level", [](auto& fetch) { fetch.mip_min_level = 1; });
  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(selected_fetch, signed_view));
}

TEST_CASE("Prepared unsigned 256x128 BC1 admits its canonical packed mip footprint",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  constexpr std::array<uint32_t, 6> kSelectedBc1Fetch{0x01000002, 0x155CB052, 0x000FE07F,
                                                      0x00A80D10, 0x000001C3, 0x155C5A18};
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                kSelectedBc1Fetch.data(), sizeof(kSelectedBc1Fetch));
  }
  auto selected_fetch = registers.GetTextureFetch(2);
  selected_fetch.size_2d.width = 255;
  selected_fetch.size_2d.height = 127;
  selected_fetch.pitch = 8;
  selected_fetch.mip_max_level = 8;
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                &selected_fetch, sizeof(selected_fetch));
  }
  selected_fetch = registers.GetTextureFetch(2);

  TextureInfo legacy;
  REQUIRE(TextureInfo::Prepare(selected_fetch, &legacy));
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(selected_fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  CHECK(legacy.memory.base_size == 0x4000);
  CHECK(legacy.memory.mip_size == 0x4000);
  CHECK(layout.base.level_data_extent_bytes == 0x4000);
  CHECK(layout.base.row_pitch_bytes == 512);
  CHECK(layout.mips_total_extent_bytes == 0x4740);
  CHECK(layout.packed_level == 3);
  CHECK(layout.mip_offsets_bytes[1] == 0);
  CHECK(layout.mip_offsets_bytes[2] == 0x2000);
  CHECK(layout.mip_offsets_bytes[3] == 0x4000);
  CHECK(layout.mip_offsets_bytes[4] == 0);
  CHECK(layout.mip_offsets_bytes[5] == 0);
  CHECK(layout.mip_offsets_bytes[6] == 0);
  CHECK(layout.mip_offsets_bytes[7] == 0);
  CHECK(layout.mip_offsets_bytes[8] == 0);
  constexpr std::array<uint32_t, 9> kMipOffsets{0,      0,      0x2000, 0x4400, 0x4200,
                                                0x4100, 0x4020, 0x4010, 0x4008};
  for (uint32_t level = 0; level < layout.packed_level; ++level) {
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    const uint32_t location = legacy.GetMipLocation(level, &offset_x, &offset_y, true);
    const uint32_t bank_address =
        level == 0 ? legacy.memory.base_address : legacy.memory.mip_address;
    CHECK(location - bank_address == kMipOffsets[level]);
    CHECK(offset_x == 0);
    CHECK(offset_y == 0);
  }
  constexpr std::array<uint32_t, 6> kPackedX{0, 0, 0, 4, 2, 1};
  constexpr std::array<uint32_t, 6> kPackedY{4, 2, 1, 0, 0, 0};
  for (uint32_t level = layout.packed_level; level < kMipOffsets.size(); ++level) {
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    uint32_t offset_z = 0;
    REQUIRE(texture_util::GetPackedMipOffset(256, 128, 1, xenos::TextureFormat::k_DXT1, level,
                                             offset_x, offset_y, offset_z));
    CHECK(offset_z == 0);
    CHECK(layout.mip_offsets_bytes[layout.packed_level] +
              offset_y * layout.mips[layout.packed_level].row_pitch_bytes + offset_x * 8 ==
          kMipOffsets[level]);
    CHECK(offset_x == kPackedX[level - layout.packed_level]);
    CHECK(offset_y == kPackedY[level - layout.packed_level]);
  }
  CHECK(layout.mips[1].row_pitch_bytes == 256);
  CHECK(layout.mips[1].array_slice_stride_bytes == 0x2000);
  CHECK(layout.mips[1].array_slice_data_extent_bytes == 0x1000);
  CHECK(layout.mips[2].row_pitch_bytes == 256);
  CHECK(layout.mips[2].array_slice_stride_bytes == 0x2000);
  CHECK(layout.mips[2].array_slice_data_extent_bytes == 0x780);
  CHECK(layout.mips[3].row_pitch_bytes == 256);
  CHECK(layout.mips[3].x_extent_blocks == 8);
  CHECK(layout.mips[3].y_extent_blocks == 8);
  CHECK(layout.mips[3].array_slice_data_extent_bytes == 0x740);
  CHECK(layout.mips[3].level_data_extent_bytes == 0x740);

  auto build = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                   std::function<bool(uint32_t, uint32_t)> gpu_written,
                   std::function<bool(uint32_t, uint32_t)> readable,
                   diagnostic::DrawCaptureFrameInputDrawView& draw,
                   FrameInputDrawBuilderResult& result) {
    FrameInputDrawBuilderInput input{
        registers,
        cache,
        &vertex,
        &pixel,
        &vertex,
        &pixel,
        {},
        std::move(dirty),
        std::move(gpu_written),
        std::move(readable),
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
        [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0xF880;
          return true;
        },
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
        0};
    draw = {};
    draw.index.present = false;
    return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
  };

  DrawCaptureFrameInputTextureView prepared_view;
  REQUIRE(cache.PrepareFrameInputTexture(selected_fetch, prepared_view));
  CHECK(prepared_view.available);
  CHECK_FALSE(prepared_view.is_signed);
  CHECK(prepared_view.required);
  CHECK(prepared_view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT1));
  CHECK(prepared_view.endianness == static_cast<uint32_t>(xenos::Endian::k8in16));
  CHECK(prepared_view.width == selected_fetch.size_2d.width);
  CHECK(prepared_view.height == selected_fetch.size_2d.height);
  CHECK(prepared_view.pitch == selected_fetch.pitch << 5);
  CHECK(prepared_view.mip_min_level == 0);
  CHECK(prepared_view.mip_max_level == 8);
  CHECK_FALSE(prepared_view.tiled);
  CHECK(prepared_view.packed_mips);
  CHECK(prepared_view.base_address == key.base_page << 12);
  CHECK(prepared_view.base_size == 0x4000);
  CHECK(prepared_view.mip_address == key.mip_page << 12);
  CHECK(prepared_view.mip_size == 0x4740);
  CHECK(prepared_view.swizzled_signs == 0);

  diagnostic::DrawCaptureFrameInputDrawView draw;
  FrameInputDrawBuilderResult result;
  REQUIRE(build([](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
                [](uint32_t, uint32_t) { return true; }, draw, result));
  REQUIRE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  REQUIRE(result.textures.size() == 8);
  size_t required_unsigned_count = 0;
  size_t optional_signed_count = 0;
  for (const auto& view : result.textures) {
    if (!view.is_signed && view.required) {
      ++required_unsigned_count;
      CHECK(view.available);
      CHECK(view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT1));
      CHECK(view.width == 256);
      CHECK(view.height == 128);
      CHECK(view.pitch == 8);
      CHECK(view.mip_max_level == 8);
      CHECK(view.base_size == 0x4000);
      CHECK(view.mip_size == 0x4740);
      CHECK(view.fetch_words.size() == 6);
      CHECK(view.swizzled_signs == 0);
    } else if (view.is_signed && !view.required) {
      ++optional_signed_count;
      CHECK_FALSE(view.available);
      CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
      CHECK_FALSE(view.memory_version.identity);
      CHECK_FALSE(view.memory_version.generation);
    }
  }
  CHECK(required_unsigned_count == 4);
  CHECK(optional_signed_count == 4);
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);

  const uint32_t mip_address = key.mip_page << 12;
  const uint32_t tail_address = mip_address + 0x4000;
  const uint32_t tail_end = mip_address + 0x4740;
  const auto overlaps_tail = [tail_address, tail_end](uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    return end > tail_address && address < tail_end;
  };
  CHECK_FALSE(overlaps_tail(mip_address, 0x4000));
  CHECK(overlaps_tail(mip_address, 0x4740));
  const auto expect_tail_rejection = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                                         std::function<bool(uint32_t, uint32_t)> gpu_written,
                                         std::function<bool(uint32_t, uint32_t)> readable) {
    diagnostic::DrawCaptureFrameInputDrawView rejected_draw;
    FrameInputDrawBuilderResult rejected_result;
    CHECK_FALSE(build(std::move(dirty), std::move(gpu_written), std::move(readable), rejected_draw,
                      rejected_result));
    CHECK_FALSE(rejected_draw.cpu_data_complete);
    CHECK(rejected_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    const auto required =
        std::find_if(rejected_result.textures.begin(), rejected_result.textures.end(),
                     [](const auto& view) { return !view.is_signed && view.required; });
    REQUIRE(required != rejected_result.textures.end());
    CHECK_FALSE(required->available);
    CHECK(required->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    CHECK(required->base_size == 0x4000);
    CHECK(required->mip_size == 0x4740);
  };
  bool dirty_tail_seen = false;
  bool dirty_tail_size_valid = true;
  expect_tail_rejection(
      [&dirty_tail_seen, &dirty_tail_size_valid, &overlaps_tail, mip_address](uint32_t address,
                                                                              uint32_t size) {
        if (address == mip_address && overlaps_tail(address, size)) {
          dirty_tail_seen = true;
          dirty_tail_size_valid &= address == mip_address && size == 0x4740;
          return true;
        }
        return false;
      },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  CHECK(dirty_tail_seen);
  CHECK(dirty_tail_size_valid);
  bool gpu_tail_seen = false;
  bool gpu_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [&gpu_tail_seen, &gpu_tail_size_valid, &overlaps_tail, mip_address](
                            uint32_t address, uint32_t size) {
                          if (address == mip_address && overlaps_tail(address, size)) {
                            gpu_tail_seen = true;
                            gpu_tail_size_valid &= address == mip_address && size == 0x4740;
                            return true;
                          }
                          return false;
                        },
                        [](uint32_t, uint32_t) { return true; });
  CHECK(gpu_tail_seen);
  CHECK(gpu_tail_size_valid);
  bool unreadable_tail_seen = false;
  bool unreadable_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [](uint32_t, uint32_t) { return false; },
                        [&unreadable_tail_seen, &unreadable_tail_size_valid, &overlaps_tail,
                         mip_address](uint32_t address, uint32_t size) {
                          if (address == mip_address && overlaps_tail(address, size)) {
                            unreadable_tail_seen = true;
                            unreadable_tail_size_valid &= address == mip_address && size == 0x4740;
                            return false;
                          }
                          return true;
                        });
  CHECK(unreadable_tail_seen);
  CHECK(unreadable_tail_size_valid);

  const auto reject_fetch = [&](const char* control, auto mutate) {
    CAPTURE(control);
    auto near_miss = selected_fetch;
    mutate(near_miss);
    DrawCaptureFrameInputTextureView view;
    CHECK_FALSE(cache.PrepareFrameInputTexture(near_miss, view));
    CHECK_FALSE(view.available);
  };
  reject_fetch("format", [](auto& fetch) { fetch.format = xenos::TextureFormat::k_DXN; });
  reject_fetch("width", [](auto& fetch) { fetch.size_2d.width = 127; });
  reject_fetch("height", [](auto& fetch) { fetch.size_2d.height = 63; });
  reject_fetch("pitch", [](auto& fetch) { fetch.pitch = 4; });
  reject_fetch("mip_max_level", [](auto& fetch) { fetch.mip_max_level = 7; });
  reject_fetch("endianness", [](auto& fetch) { fetch.endianness = xenos::Endian::kNone; });
  reject_fetch("swizzle", [](auto& fetch) { fetch.swizzle = 0x680; });
  reject_fetch("signed component", [](auto& fetch) { fetch.sign_y = xenos::TextureSign::kSigned; });
  reject_fetch("tiled", [](auto& fetch) { fetch.tiled = true; });
  reject_fetch("cube", [](auto& fetch) { fetch.dimension = xenos::DataDimension::kCube; });
  reject_fetch("stacked", [](auto& fetch) { fetch.stacked = true; });
  reject_fetch("base_address", [](auto& fetch) { fetch.base_address = 0; });
  reject_fetch("mip_min_level", [](auto& fetch) { fetch.mip_min_level = 1; });
  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(selected_fetch, signed_view));

  const auto ordinary_fetch = [&] {
    auto fetch = selected_fetch;
    fetch.packed_mips = false;
    return fetch;
  }();
  DrawCaptureFrameInputTextureView ordinary_view;
  REQUIRE(cache.PrepareFrameInputTexture(ordinary_fetch, ordinary_view));
  TextureInfo ordinary_info;
  REQUIRE(TextureInfo::Prepare(ordinary_fetch, &ordinary_info));
  TestTextureCache::TextureKey ordinary_key;
  TestTextureCache::BindingInfoFromFetchConstant(ordinary_fetch, ordinary_key, nullptr);
  const auto ordinary_layout = ordinary_key.GetGuestLayout();
  CHECK(ordinary_view.available);
  CHECK_FALSE(ordinary_view.packed_mips);
  CHECK(ordinary_view.base_size == ordinary_info.memory.base_size);
  CHECK(ordinary_view.mip_size == ordinary_info.memory.mip_size);
  CHECK(ordinary_view.base_size >= ordinary_layout.base.level_data_extent_bytes);
  CHECK(ordinary_view.mip_size >= ordinary_layout.mips_total_extent_bytes);
}

TEST_CASE("Prepared unsigned 512x512 BC1 admits its canonical packed mip footprint",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  constexpr std::array<uint32_t, 6> kSelectedBc1Fetch{0x04000002, 0x19F61052, 0x003FE1FF,
                                                      0x00A80D10, 0x00000243, 0x19F51A18};
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                kSelectedBc1Fetch.data(), sizeof(kSelectedBc1Fetch));
  }

  const auto selected_fetch = registers.GetTextureFetch(2);
  TextureInfo legacy;
  REQUIRE(TextureInfo::Prepare(selected_fetch, &legacy));
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(selected_fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  CHECK(legacy.memory.base_size == 0x20000);
  CHECK(legacy.memory.mip_size == 0xE000);
  CHECK(layout.base.level_data_extent_bytes == 0x20000);
  CHECK(layout.base.row_pitch_bytes == 1024);
  CHECK(layout.mips_total_extent_bytes == 0xE340);
  CHECK(layout.packed_level == 5);
  CHECK(layout.mip_offsets_bytes[1] == 0);
  CHECK(layout.mip_offsets_bytes[2] == 0x8000);
  CHECK(layout.mip_offsets_bytes[3] == 0xA000);
  CHECK(layout.mip_offsets_bytes[4] == 0xC000);
  CHECK(layout.mip_offsets_bytes[5] == 0xE000);
  CHECK(layout.mip_offsets_bytes[6] == 0);
  CHECK(layout.mip_offsets_bytes[7] == 0);
  CHECK(layout.mip_offsets_bytes[8] == 0);
  CHECK(layout.mip_offsets_bytes[9] == 0);
  constexpr std::array<uint32_t, 10> kMipOffsets{0,      0,      0x8000, 0xA000, 0xC000,
                                                 0xE020, 0xE010, 0xE008, 0xE200, 0xE100};
  for (uint32_t level = 0; level < 5; ++level) {
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    const uint32_t location = legacy.GetMipLocation(level, &offset_x, &offset_y, true);
    const uint32_t bank_address =
        level == 0 ? legacy.memory.base_address : legacy.memory.mip_address;
    CHECK(location - bank_address == kMipOffsets[level]);
    CHECK(offset_x == 0);
    CHECK(offset_y == 0);
  }
  for (uint32_t level = layout.packed_level; level < kMipOffsets.size(); ++level) {
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    uint32_t offset_z = 0;
    REQUIRE(texture_util::GetPackedMipOffset(512, 512, 1, xenos::TextureFormat::k_DXT1, level,
                                             offset_x, offset_y, offset_z));
    CHECK(offset_z == 0);
    CHECK(layout.mip_offsets_bytes[layout.packed_level] +
              offset_y * layout.mips[layout.packed_level].row_pitch_bytes + offset_x * 8 ==
          kMipOffsets[level]);
  }
  CHECK(layout.mips[1].row_pitch_bytes == 512);
  CHECK(layout.mips[1].array_slice_stride_bytes == 0x8000);
  CHECK(layout.mips[1].array_slice_data_extent_bytes == 0x8000);
  CHECK(layout.mips[2].row_pitch_bytes == 256);
  CHECK(layout.mips[2].array_slice_stride_bytes == 0x2000);
  CHECK(layout.mips[2].array_slice_data_extent_bytes == 0x2000);
  CHECK(layout.mips[3].row_pitch_bytes == 256);
  CHECK(layout.mips[3].array_slice_stride_bytes == 0x2000);
  CHECK(layout.mips[3].array_slice_data_extent_bytes == 0xF80);
  CHECK(layout.mips[4].row_pitch_bytes == 256);
  CHECK(layout.mips[4].array_slice_stride_bytes == 0x2000);
  CHECK(layout.mips[4].array_slice_data_extent_bytes == 0x740);
  CHECK(layout.mips[5].row_pitch_bytes == 256);
  CHECK(layout.mips[5].x_extent_blocks == 8);
  CHECK(layout.mips[5].y_extent_blocks == 4);
  CHECK(layout.mips[5].array_slice_data_extent_bytes == 0x340);
  CHECK(layout.mips[5].level_data_extent_bytes == 0x340);

  auto build = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                   std::function<bool(uint32_t, uint32_t)> gpu_written,
                   std::function<bool(uint32_t, uint32_t)> readable,
                   diagnostic::DrawCaptureFrameInputDrawView& draw,
                   FrameInputDrawBuilderResult& result) {
    FrameInputDrawBuilderInput input{
        registers,
        cache,
        &vertex,
        &pixel,
        &vertex,
        &pixel,
        {},
        std::move(dirty),
        std::move(gpu_written),
        std::move(readable),
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
        [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0xF880;
          return true;
        },
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
        0};
    draw = {};
    draw.index.present = false;
    return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
  };

  DrawCaptureFrameInputTextureView prepared_view;
  REQUIRE(cache.PrepareFrameInputTexture(selected_fetch, prepared_view));
  CHECK(prepared_view.available);
  CHECK_FALSE(prepared_view.is_signed);
  CHECK(prepared_view.required);
  CHECK(prepared_view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT1));
  CHECK(prepared_view.endianness == static_cast<uint32_t>(xenos::Endian::k8in16));
  CHECK(prepared_view.width == selected_fetch.size_2d.width);
  CHECK(prepared_view.height == selected_fetch.size_2d.height);
  CHECK(prepared_view.pitch == selected_fetch.pitch << 5);
  CHECK(prepared_view.mip_min_level == 0);
  CHECK(prepared_view.mip_max_level == 9);
  CHECK_FALSE(prepared_view.tiled);
  CHECK(prepared_view.packed_mips);
  CHECK(prepared_view.base_address == key.base_page << 12);
  CHECK(prepared_view.base_size == 0x20000);
  CHECK(prepared_view.mip_address == key.mip_page << 12);
  CHECK(prepared_view.mip_size == 0xE340);
  CHECK(prepared_view.swizzled_signs == 0);

  diagnostic::DrawCaptureFrameInputDrawView draw;
  FrameInputDrawBuilderResult result;
  REQUIRE(build([](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
                [](uint32_t, uint32_t) { return true; }, draw, result));
  REQUIRE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  REQUIRE(result.textures.size() == 8);
  size_t required_unsigned_count = 0;
  size_t optional_signed_count = 0;
  for (const auto& view : result.textures) {
    if (!view.is_signed && view.required) {
      ++required_unsigned_count;
      CHECK(view.available);
      CHECK(view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT1));
      CHECK(view.width == 512);
      CHECK(view.height == 512);
      CHECK(view.pitch == 16);
      CHECK(view.mip_max_level == 9);
      CHECK(view.base_size == 0x20000);
      CHECK(view.mip_size == 0xE340);
      CHECK(view.fetch_words.size() == 6);
      CHECK(view.swizzled_signs == 0);
    } else if (view.is_signed && !view.required) {
      ++optional_signed_count;
      CHECK_FALSE(view.available);
      CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
      CHECK_FALSE(view.memory_version.identity);
      CHECK_FALSE(view.memory_version.generation);
    }
  }
  CHECK(required_unsigned_count == 4);
  CHECK(optional_signed_count == 4);
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);

  const uint32_t mip_address = key.mip_page << 12;
  const uint32_t tail_address = mip_address + 0xE000;
  const uint32_t tail_end = mip_address + 0xE340;
  const auto overlaps_tail = [tail_address, tail_end](uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    return end > tail_address && address < tail_end;
  };
  CHECK_FALSE(overlaps_tail(mip_address, 0xE000));
  CHECK(overlaps_tail(mip_address, 0xE340));
  const auto expect_tail_rejection = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                                         std::function<bool(uint32_t, uint32_t)> gpu_written,
                                         std::function<bool(uint32_t, uint32_t)> readable) {
    diagnostic::DrawCaptureFrameInputDrawView rejected_draw;
    FrameInputDrawBuilderResult rejected_result;
    CHECK_FALSE(build(std::move(dirty), std::move(gpu_written), std::move(readable), rejected_draw,
                      rejected_result));
    CHECK_FALSE(rejected_draw.cpu_data_complete);
    CHECK(rejected_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    const auto required =
        std::find_if(rejected_result.textures.begin(), rejected_result.textures.end(),
                     [](const auto& view) { return !view.is_signed && view.required; });
    REQUIRE(required != rejected_result.textures.end());
    CHECK_FALSE(required->available);
    CHECK(required->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    CHECK(required->base_size == 0x20000);
    CHECK(required->mip_size == 0xE340);
  };
  bool dirty_tail_seen = false;
  bool dirty_tail_size_valid = true;
  expect_tail_rejection(
      [&dirty_tail_seen, &dirty_tail_size_valid, &overlaps_tail, mip_address](uint32_t address,
                                                                              uint32_t size) {
        if (address == mip_address && overlaps_tail(address, size)) {
          dirty_tail_seen = true;
          dirty_tail_size_valid &= size == 0xE340;
          return true;
        }
        return false;
      },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  CHECK(dirty_tail_seen);
  CHECK(dirty_tail_size_valid);
  bool gpu_tail_seen = false;
  bool gpu_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [&gpu_tail_seen, &gpu_tail_size_valid, &overlaps_tail, mip_address](
                            uint32_t address, uint32_t size) {
                          if (address == mip_address && overlaps_tail(address, size)) {
                            gpu_tail_seen = true;
                            gpu_tail_size_valid &= size == 0xE340;
                            return true;
                          }
                          return false;
                        },
                        [](uint32_t, uint32_t) { return true; });
  CHECK(gpu_tail_seen);
  CHECK(gpu_tail_size_valid);
  bool unreadable_tail_seen = false;
  bool unreadable_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [](uint32_t, uint32_t) { return false; },
                        [&unreadable_tail_seen, &unreadable_tail_size_valid, &overlaps_tail,
                         mip_address](uint32_t address, uint32_t size) {
                          if (address == mip_address && overlaps_tail(address, size)) {
                            unreadable_tail_seen = true;
                            unreadable_tail_size_valid &= size == 0xE340;
                            return false;
                          }
                          return true;
                        });
  CHECK(unreadable_tail_seen);
  CHECK(unreadable_tail_size_valid);

  const auto reject_fetch = [&](const char* control, auto mutate) {
    CAPTURE(control);
    auto near_miss = selected_fetch;
    mutate(near_miss);
    DrawCaptureFrameInputTextureView view;
    CHECK_FALSE(cache.PrepareFrameInputTexture(near_miss, view));
    CHECK_FALSE(view.available);
  };
  reject_fetch("format", [](auto& fetch) { fetch.format = xenos::TextureFormat::k_DXN; });
  reject_fetch("width", [](auto& fetch) { fetch.size_2d.width = 255; });
  reject_fetch("height", [](auto& fetch) { fetch.size_2d.height = 127; });
  reject_fetch("pitch", [](auto& fetch) { fetch.pitch = 8; });
  reject_fetch("mip_max_level", [](auto& fetch) { fetch.mip_max_level = 8; });
  reject_fetch("endianness", [](auto& fetch) { fetch.endianness = xenos::Endian::kNone; });
  reject_fetch("swizzle", [](auto& fetch) { fetch.swizzle = 0x680; });
  reject_fetch("signed component", [](auto& fetch) { fetch.sign_y = xenos::TextureSign::kSigned; });
  reject_fetch("tiled", [](auto& fetch) { fetch.tiled = true; });
  const auto expect_ordinary_fetch = [&](const char* control, auto mutate) {
    CAPTURE(control);
    auto ordinary_fetch = selected_fetch;
    mutate(ordinary_fetch);
    DrawCaptureFrameInputTextureView ordinary_view;
    REQUIRE(cache.PrepareFrameInputTexture(ordinary_fetch, ordinary_view));
    TextureInfo ordinary_info;
    REQUIRE(TextureInfo::Prepare(ordinary_fetch, &ordinary_info));
    TestTextureCache::TextureKey ordinary_key;
    TestTextureCache::BindingInfoFromFetchConstant(ordinary_fetch, ordinary_key, nullptr);
    const auto ordinary_layout = ordinary_key.GetGuestLayout();
    CHECK(ordinary_view.available);
    CHECK_FALSE(ordinary_view.packed_mips);
    CHECK(ordinary_view.base_size == ordinary_info.memory.base_size);
    CHECK(ordinary_view.mip_size == ordinary_info.memory.mip_size);
    CHECK(ordinary_view.base_size >= ordinary_layout.base.level_data_extent_bytes);
    CHECK(ordinary_view.mip_size >= ordinary_layout.mips_total_extent_bytes);
  };
  expect_ordinary_fetch("packed_mips", [](auto& fetch) { fetch.packed_mips = false; });
  reject_fetch("cube", [](auto& fetch) { fetch.dimension = xenos::DataDimension::kCube; });
  reject_fetch("stacked", [](auto& fetch) { fetch.stacked = true; });
  reject_fetch("base_address", [](auto& fetch) { fetch.base_address = 0; });
  reject_fetch("mip_min_level", [](auto& fetch) { fetch.mip_min_level = 1; });
  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(selected_fetch, signed_view));
}

TEST_CASE("Prepared unsigned BC2 admits its canonical packed mip footprint",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  constexpr std::array<uint32_t, 6> kSelectedBc2Fetch{0x01000002, 0x168C6053, 0x0007E03F,
                                                      0x00A80D10, 0x00000183, 0x168BEA18};
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                kSelectedBc2Fetch.data(), sizeof(kSelectedBc2Fetch));
  }

  const auto selected_fetch = registers.GetTextureFetch(2);
  TextureInfo legacy;
  REQUIRE(TextureInfo::Prepare(selected_fetch, &legacy));
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(selected_fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  CHECK(legacy.memory.base_size == 0x2000);
  CHECK(legacy.memory.mip_size == 0x4000);
  CHECK(layout.base.level_data_extent_bytes == 0x1F00);
  CHECK(layout.mips_total_extent_bytes == 0x4680);
  CHECK(layout.packed_level == 2);
  CHECK(layout.mip_offsets_bytes[2] == 0x4000);
  CHECK(layout.mips[2].array_slice_data_extent_bytes == 0x680);
  CHECK(layout.mips[2].level_data_extent_bytes == 0x680);
  CHECK(key.base_page << 12 == 0x168C6000);
  CHECK(key.mip_page << 12 == 0x168BE000);

  auto build = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                   std::function<bool(uint32_t, uint32_t)> gpu_written,
                   std::function<bool(uint32_t, uint32_t)> readable,
                   diagnostic::DrawCaptureFrameInputDrawView& draw,
                   FrameInputDrawBuilderResult& result) {
    FrameInputDrawBuilderInput input{
        registers,
        cache,
        &vertex,
        &pixel,
        &vertex,
        &pixel,
        {},
        std::move(dirty),
        std::move(gpu_written),
        std::move(readable),
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
        [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0xF880;
          return true;
        },
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
        0};
    draw = {};
    draw.index.present = false;
    return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
  };

  DrawCaptureFrameInputTextureView prepared_view;
  REQUIRE(cache.PrepareFrameInputTexture(selected_fetch, prepared_view));
  CHECK(prepared_view.available);
  CHECK_FALSE(prepared_view.is_signed);
  CHECK(prepared_view.required);
  CHECK(prepared_view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT2_3));
  CHECK(prepared_view.endianness == static_cast<uint32_t>(xenos::Endian::k8in16));
  CHECK(prepared_view.width == selected_fetch.size_2d.width);
  CHECK(prepared_view.height == selected_fetch.size_2d.height);
  CHECK(prepared_view.pitch == selected_fetch.pitch << 5);
  CHECK(prepared_view.mip_min_level == 0);
  CHECK(prepared_view.mip_max_level == 6);
  CHECK_FALSE(prepared_view.tiled);
  CHECK(prepared_view.packed_mips);
  CHECK(prepared_view.base_address == key.base_page << 12);
  CHECK(prepared_view.base_size == 0x2000);
  CHECK(prepared_view.mip_address == key.mip_page << 12);
  CHECK(prepared_view.mip_size == 0x4680);
  CHECK(prepared_view.swizzled_signs == 0);
  CHECK_FALSE(prepared_view.memory_version.identity);
  CHECK_FALSE(prepared_view.memory_version.generation);
  CHECK_FALSE(prepared_view.memory_version.check);

  diagnostic::DrawCaptureFrameInputDrawView draw;
  FrameInputDrawBuilderResult result;
  REQUIRE(build([](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
                [](uint32_t, uint32_t) { return true; }, draw, result));
  REQUIRE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  REQUIRE(result.textures.size() == 8);

  size_t required_unsigned_count = 0;
  size_t optional_signed_count = 0;
  for (const auto& view : result.textures) {
    if (!view.is_signed && view.required) {
      ++required_unsigned_count;
      CHECK(view.available);
      CHECK(view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT2_3));
      CHECK(view.base_size == 0x2000);
      CHECK(view.mip_size == 0x4680);
      CHECK(view.width == 64);
      CHECK(view.height == 64);
      CHECK(view.pitch == 4);
      CHECK(view.mip_max_level == 6);
      CHECK(view.fetch_words.size() == 6);
      CHECK(view.swizzled_signs == 0);
      CHECK_FALSE(view.memory_version.identity);
      CHECK_FALSE(view.memory_version.generation);
      CHECK_FALSE(view.memory_version.check);
    } else if (view.is_signed && !view.required) {
      ++optional_signed_count;
      CHECK_FALSE(view.available);
      CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
      CHECK_FALSE(view.memory_version.identity);
      CHECK_FALSE(view.memory_version.generation);
      CHECK_FALSE(view.memory_version.check);
    }
  }
  CHECK(required_unsigned_count == 4);
  CHECK(optional_signed_count == 4);
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);

  const uint32_t base_address = key.base_page << 12;
  const uint32_t mip_address = key.mip_page << 12;
  const uint32_t tail_address = mip_address + 0x4000;
  const uint32_t tail_end = mip_address + 0x4680;
  const auto overlaps_tail = [tail_address, tail_end](uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    return end > tail_address && address < tail_end;
  };
  CHECK_FALSE(overlaps_tail(mip_address, 0x4000));
  CHECK(overlaps_tail(mip_address, 0x4680));
  const auto expect_tail_rejection = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                                         std::function<bool(uint32_t, uint32_t)> gpu_written,
                                         std::function<bool(uint32_t, uint32_t)> readable) {
    diagnostic::DrawCaptureFrameInputDrawView rejected_draw;
    FrameInputDrawBuilderResult rejected_result;
    CHECK_FALSE(build(std::move(dirty), std::move(gpu_written), std::move(readable), rejected_draw,
                      rejected_result));
    CHECK_FALSE(rejected_draw.cpu_data_complete);
    CHECK(rejected_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    const auto required =
        std::find_if(rejected_result.textures.begin(), rejected_result.textures.end(),
                     [](const auto& view) { return !view.is_signed && view.required; });
    REQUIRE(required != rejected_result.textures.end());
    CHECK_FALSE(required->available);
    CHECK(required->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    CHECK(required->base_address == base_address);
    CHECK(required->base_size == 0x2000);
    CHECK(required->mip_address == mip_address);
    CHECK(required->mip_size == 0x4680);
  };

  bool dirty_tail_seen = false;
  bool dirty_tail_size_valid = true;
  expect_tail_rejection(
      [&dirty_tail_seen, &dirty_tail_size_valid, &overlaps_tail, mip_address](uint32_t address,
                                                                              uint32_t size) {
        if (overlaps_tail(address, size)) {
          dirty_tail_seen = true;
          dirty_tail_size_valid &= address == mip_address && size == 0x4680;
          return true;
        }
        return false;
      },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  CHECK(dirty_tail_seen);
  CHECK(dirty_tail_size_valid);

  bool gpu_tail_seen = false;
  bool gpu_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [&gpu_tail_seen, &gpu_tail_size_valid, &overlaps_tail, mip_address](
                            uint32_t address, uint32_t size) {
                          if (overlaps_tail(address, size)) {
                            gpu_tail_seen = true;
                            gpu_tail_size_valid &= address == mip_address && size == 0x4680;
                            return true;
                          }
                          return false;
                        },
                        [](uint32_t, uint32_t) { return true; });
  CHECK(gpu_tail_seen);
  CHECK(gpu_tail_size_valid);

  bool unreadable_tail_seen = false;
  bool unreadable_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [](uint32_t, uint32_t) { return false; },
                        [&unreadable_tail_seen, &unreadable_tail_size_valid, &overlaps_tail,
                         mip_address](uint32_t address, uint32_t size) {
                          if (overlaps_tail(address, size)) {
                            unreadable_tail_seen = true;
                            unreadable_tail_size_valid &= address == mip_address && size == 0x4680;
                            return false;
                          }
                          return true;
                        });
  CHECK(unreadable_tail_seen);
  CHECK(unreadable_tail_size_valid);

  const auto reject_fetch = [&](auto mutate) {
    auto near_miss = selected_fetch;
    mutate(near_miss);
    DrawCaptureFrameInputTextureView view;
    CHECK_FALSE(cache.PrepareFrameInputTexture(near_miss, view));
    CHECK_FALSE(view.available);
  };
  reject_fetch([](auto& fetch) { fetch.format = xenos::TextureFormat::k_DXN; });
  reject_fetch([](auto& fetch) { fetch.size_2d.width = 127; });
  reject_fetch([](auto& fetch) { fetch.size_2d.height = 127; });
  reject_fetch([](auto& fetch) { fetch.endianness = xenos::Endian::kNone; });
  reject_fetch([](auto& fetch) { fetch.swizzle = 0x680; });
  reject_fetch([](auto& fetch) { fetch.sign_y = xenos::TextureSign::kSigned; });
  reject_fetch([](auto& fetch) { fetch.tiled = true; });
  reject_fetch([](auto& fetch) { fetch.dimension = xenos::DataDimension::kCube; });
  reject_fetch([](auto& fetch) { fetch.stacked = true; });
  reject_fetch([](auto& fetch) { fetch.mip_min_level = 1; });
  reject_fetch([](auto& fetch) { fetch.mip_max_level = 5; });
  reject_fetch([](auto& fetch) { fetch.mip_max_level = 7; });
  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(selected_fetch, signed_view));
}

TEST_CASE("Prepared unsigned 128x128 BC2 admits its canonical packed mip footprint",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  constexpr std::array<uint32_t, 6> kSelectedBc2Fetch{0x01000002, 0x15AA7053, 0x000FE07F,
                                                      0x00A80D10, 0x000001C3, 0x15A9BA18};
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                kSelectedBc2Fetch.data(), sizeof(kSelectedBc2Fetch));
  }

  const auto selected_fetch = registers.GetTextureFetch(2);
  TextureInfo legacy;
  REQUIRE(TextureInfo::Prepare(selected_fetch, &legacy));
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(selected_fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  CHECK(legacy.memory.base_size == 0x4000);
  CHECK(legacy.memory.mip_size == 0x8000);
  CHECK(layout.base.level_data_extent_bytes == 0x4000);
  CHECK(layout.mips_total_extent_bytes == 0x8680);
  CHECK(layout.packed_level == 3);
  CHECK(layout.mips[1].array_slice_stride_bytes == 0x4000);
  CHECK(layout.mips[2].array_slice_stride_bytes == 0x4000);
  CHECK(layout.mips[1].level_data_extent_bytes == 0x1F00);
  CHECK(layout.mips[2].level_data_extent_bytes == 0xE80);
  CHECK(layout.mips[3].row_pitch_bytes == 0x200);
  CHECK(layout.mips[3].x_extent_blocks == 8);
  CHECK(layout.mips[3].y_extent_blocks == 4);
  CHECK(layout.mips[3].array_slice_data_extent_bytes == 0x680);
  CHECK(layout.mips[3].level_data_extent_bytes == 0x680);
  CHECK(layout.mip_offsets_bytes[1] == 0);
  CHECK(layout.mip_offsets_bytes[2] == 0x4000);
  CHECK(layout.mip_offsets_bytes[3] == 0x8000);
  constexpr std::array<uint32_t, 8> kExpectedMipOffsets{0,      0,      0x4000, 0x8040,
                                                        0x8020, 0x8010, 0x8400, 0x8200};
  constexpr uint32_t kBytesPerBlock = 16;
  for (uint32_t level = 3; level < kExpectedMipOffsets.size(); ++level) {
    uint32_t x_blocks = 0;
    uint32_t y_blocks = 0;
    uint32_t z_blocks = 0;
    REQUIRE(texture_util::GetPackedMipOffset(128, 128, 1, xenos::TextureFormat::k_DXT2_3, level,
                                             x_blocks, y_blocks, z_blocks));
    CHECK(z_blocks == 0);
    CHECK(layout.mip_offsets_bytes[3] + y_blocks * layout.mips[3].row_pitch_bytes +
              x_blocks * kBytesPerBlock ==
          kExpectedMipOffsets[level]);
  }
  CHECK(key.base_page << 12 == 0x15AA7000);
  CHECK(key.mip_page << 12 == 0x15A9B000);

  auto build = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                   std::function<bool(uint32_t, uint32_t)> gpu_written,
                   std::function<bool(uint32_t, uint32_t)> readable,
                   diagnostic::DrawCaptureFrameInputDrawView& draw,
                   FrameInputDrawBuilderResult& result) {
    FrameInputDrawBuilderInput input{
        registers,
        cache,
        &vertex,
        &pixel,
        &vertex,
        &pixel,
        {},
        std::move(dirty),
        std::move(gpu_written),
        std::move(readable),
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
        [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0xF880;
          return true;
        },
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
        0};
    draw = {};
    draw.index.present = false;
    return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
  };

  DrawCaptureFrameInputTextureView prepared_view;
  REQUIRE(cache.PrepareFrameInputTexture(selected_fetch, prepared_view));
  CHECK(prepared_view.available);
  CHECK_FALSE(prepared_view.is_signed);
  CHECK(prepared_view.required);
  CHECK(prepared_view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT2_3));
  CHECK(prepared_view.endianness == static_cast<uint32_t>(xenos::Endian::k8in16));
  CHECK(prepared_view.width == selected_fetch.size_2d.width);
  CHECK(prepared_view.height == selected_fetch.size_2d.height);
  CHECK(prepared_view.pitch == 128);
  CHECK(prepared_view.mip_min_level == 0);
  CHECK(prepared_view.mip_max_level == 7);
  CHECK_FALSE(prepared_view.tiled);
  CHECK(prepared_view.packed_mips);
  CHECK(prepared_view.base_address == key.base_page << 12);
  CHECK(prepared_view.base_size == 0x4000);
  CHECK(prepared_view.mip_address == key.mip_page << 12);
  CHECK(prepared_view.mip_size == 0x8680);
  CHECK(prepared_view.swizzled_signs == 0);
  CHECK_FALSE(prepared_view.memory_version.identity);
  CHECK_FALSE(prepared_view.memory_version.generation);
  CHECK_FALSE(prepared_view.memory_version.check);

  diagnostic::DrawCaptureFrameInputDrawView draw;
  FrameInputDrawBuilderResult result;
  const uint32_t base_address = key.base_page << 12;
  const uint32_t mip_address = key.mip_page << 12;
  bool base_guarded = false;
  bool mip_guarded = false;
  REQUIRE(build([](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
                [&](uint32_t address, uint32_t size) {
                  base_guarded |= address == base_address && size == 0x4000;
                  mip_guarded |= address == mip_address && size == 0x8680;
                  return true;
                },
                draw, result));
  REQUIRE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  CHECK(base_guarded);
  CHECK(mip_guarded);
  const auto required =
      std::find_if(result.textures.begin(), result.textures.end(),
                   [](const auto& view) { return !view.is_signed && view.required; });
  REQUIRE(required != result.textures.end());
  CHECK(required->available);
  CHECK(required->base_size == 0x4000);
  CHECK(required->mip_size == 0x8680);
  CHECK(required->width == 128);
  CHECK(required->height == 128);
  CHECK(required->pitch == 4);
  CHECK(required->mip_max_level == 7);
  CHECK(required->fetch_words.size() == 6);
  CHECK(required->swizzled_signs == 0);
  CHECK_FALSE(required->memory_version.identity);
  CHECK_FALSE(required->memory_version.generation);
  CHECK_FALSE(required->memory_version.check);
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);

  const uint32_t tail_address = mip_address + 0x8000;
  const uint32_t tail_end = mip_address + 0x8680;
  const auto overlaps_tail = [tail_address, tail_end](uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    return end > tail_address && address < tail_end;
  };
  CHECK_FALSE(overlaps_tail(mip_address, 0x8000));
  CHECK(overlaps_tail(mip_address, 0x8680));
  const auto expect_tail_rejection = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                                         std::function<bool(uint32_t, uint32_t)> gpu_written,
                                         std::function<bool(uint32_t, uint32_t)> readable) {
    diagnostic::DrawCaptureFrameInputDrawView rejected_draw;
    FrameInputDrawBuilderResult rejected_result;
    CHECK_FALSE(build(std::move(dirty), std::move(gpu_written), std::move(readable), rejected_draw,
                      rejected_result));
    CHECK_FALSE(rejected_draw.cpu_data_complete);
    CHECK(rejected_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    const auto rejected =
        std::find_if(rejected_result.textures.begin(), rejected_result.textures.end(),
                     [](const auto& view) { return !view.is_signed && view.required; });
    REQUIRE(rejected != rejected_result.textures.end());
    CHECK_FALSE(rejected->available);
    CHECK(rejected->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    CHECK(rejected->base_address == base_address);
    CHECK(rejected->base_size == 0x4000);
    CHECK(rejected->mip_address == mip_address);
    CHECK(rejected->mip_size == 0x8680);
  };

  bool dirty_tail_seen = false;
  bool dirty_tail_size_valid = true;
  expect_tail_rejection(
      [&dirty_tail_seen, &dirty_tail_size_valid, &overlaps_tail, mip_address](uint32_t address,
                                                                              uint32_t size) {
        if (overlaps_tail(address, size)) {
          dirty_tail_seen = true;
          dirty_tail_size_valid &= address == mip_address && size == 0x8680;
          return true;
        }
        return false;
      },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  CHECK(dirty_tail_seen);
  CHECK(dirty_tail_size_valid);

  bool gpu_tail_seen = false;
  bool gpu_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [&gpu_tail_seen, &gpu_tail_size_valid, &overlaps_tail, mip_address](
                            uint32_t address, uint32_t size) {
                          if (overlaps_tail(address, size)) {
                            gpu_tail_seen = true;
                            gpu_tail_size_valid &= address == mip_address && size == 0x8680;
                            return true;
                          }
                          return false;
                        },
                        [](uint32_t, uint32_t) { return true; });
  CHECK(gpu_tail_seen);
  CHECK(gpu_tail_size_valid);

  bool unreadable_tail_seen = false;
  bool unreadable_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [](uint32_t, uint32_t) { return false; },
                        [&unreadable_tail_seen, &unreadable_tail_size_valid, &overlaps_tail,
                         mip_address](uint32_t address, uint32_t size) {
                          if (overlaps_tail(address, size)) {
                            unreadable_tail_seen = true;
                            unreadable_tail_size_valid &= address == mip_address && size == 0x8680;
                            return false;
                          }
                          return true;
                        });
  CHECK(unreadable_tail_seen);
  CHECK(unreadable_tail_size_valid);

  const auto reject_fetch = [&](auto mutate) {
    auto near_miss = selected_fetch;
    mutate(near_miss);
    DrawCaptureFrameInputTextureView view;
    CHECK_FALSE(cache.PrepareFrameInputTexture(near_miss, view));
    CHECK_FALSE(view.available);
  };
  reject_fetch([](auto& fetch) { fetch.size_2d.width = 126; });
  reject_fetch([](auto& fetch) { fetch.size_2d.height = 126; });
  reject_fetch([](auto& fetch) { fetch.mip_max_level = 6; });
  reject_fetch([](auto& fetch) { fetch.mip_max_level = 8; });
  reject_fetch([](auto& fetch) { fetch.pitch = 3; });
  reject_fetch([](auto& fetch) { fetch.format = xenos::TextureFormat::k_DXN; });
  reject_fetch([](auto& fetch) { fetch.endianness = xenos::Endian::kNone; });
  reject_fetch([](auto& fetch) { fetch.swizzle = 0x680; });
  reject_fetch([](auto& fetch) { fetch.sign_y = xenos::TextureSign::kSigned; });
  reject_fetch([](auto& fetch) { fetch.tiled = true; });
  reject_fetch([](auto& fetch) { fetch.dimension = xenos::DataDimension::kCube; });
  reject_fetch([](auto& fetch) { fetch.stacked = true; });
  reject_fetch([](auto& fetch) { fetch.mip_min_level = 1; });
  reject_fetch([](auto& fetch) { fetch.base_address = 0; });
  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(selected_fetch, signed_view));
}

TEST_CASE("Prepared unsigned rectangular BC2 admits its canonical packed mip footprint",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto words = SampleUcode(false);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);

  constexpr std::array<uint32_t, 6> kSelectedBc2Fetch{0x01000002, 0x168B6053, 0x001FE07F,
                                                      0x00A80D10, 0x00000203, 0x168AAA18};
  for (const uint32_t fetch_constant : {2U, 7U}) {
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 6],
                kSelectedBc2Fetch.data(), sizeof(kSelectedBc2Fetch));
  }

  const auto selected_fetch = registers.GetTextureFetch(2);
  TextureInfo legacy;
  REQUIRE(TextureInfo::Prepare(selected_fetch, &legacy));
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(selected_fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  CHECK(legacy.memory.base_size == 0x8000);
  CHECK(legacy.memory.mip_size == 0x8000);
  CHECK(layout.base.level_data_extent_bytes == 0x8000);
  CHECK(layout.mips_total_extent_bytes == 0x8E80);
  CHECK(layout.packed_level == 3);
  CHECK(layout.mip_offsets_bytes[3] == 0x8000);
  CHECK(layout.mips[3].row_pitch_bytes == 0x200);
  CHECK(layout.mips[3].x_extent_blocks == 8);
  CHECK(layout.mips[3].y_extent_blocks == 8);
  CHECK(layout.mips[3].array_slice_data_extent_bytes == 0xE80);
  CHECK(layout.mips[3].level_data_extent_bytes == 0xE80);
  CHECK(key.base_page << 12 == 0x168B6000);
  CHECK(key.mip_page << 12 == 0x168AA000);

  auto build = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                   std::function<bool(uint32_t, uint32_t)> gpu_written,
                   std::function<bool(uint32_t, uint32_t)> readable,
                   diagnostic::DrawCaptureFrameInputDrawView& draw,
                   FrameInputDrawBuilderResult& result) {
    FrameInputDrawBuilderInput input{
        registers,
        cache,
        &vertex,
        &pixel,
        &vertex,
        &pixel,
        {},
        std::move(dirty),
        std::move(gpu_written),
        std::move(readable),
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureGuestMemoryVersion{}; },
        [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0xF880;
          return true;
        },
        [](uint32_t, uint32_t) { return diagnostic::DrawCaptureReason::kNone; },
        0};
    draw = {};
    draw.index.present = false;
    return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
  };

  DrawCaptureFrameInputTextureView prepared_view;
  REQUIRE(cache.PrepareFrameInputTexture(selected_fetch, prepared_view));
  CHECK(prepared_view.available);
  CHECK_FALSE(prepared_view.is_signed);
  CHECK(prepared_view.required);
  CHECK(prepared_view.format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT2_3));
  CHECK(prepared_view.endianness == static_cast<uint32_t>(xenos::Endian::k8in16));
  CHECK(prepared_view.width == selected_fetch.size_2d.width);
  CHECK(prepared_view.height == selected_fetch.size_2d.height);
  CHECK(prepared_view.pitch == selected_fetch.pitch << 5);
  CHECK(prepared_view.mip_min_level == 0);
  CHECK(prepared_view.mip_max_level == 8);
  CHECK_FALSE(prepared_view.tiled);
  CHECK(prepared_view.packed_mips);
  CHECK(prepared_view.base_address == key.base_page << 12);
  CHECK(prepared_view.base_size == 0x8000);
  CHECK(prepared_view.mip_address == key.mip_page << 12);
  CHECK(prepared_view.mip_size == 0x8E80);
  CHECK(prepared_view.swizzled_signs == 0);
  CHECK_FALSE(prepared_view.memory_version.identity);
  CHECK_FALSE(prepared_view.memory_version.generation);
  CHECK_FALSE(prepared_view.memory_version.check);

  diagnostic::DrawCaptureFrameInputDrawView draw;
  FrameInputDrawBuilderResult result;
  REQUIRE(build([](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return false; },
                [](uint32_t, uint32_t) { return true; }, draw, result));
  REQUIRE(draw.cpu_data_complete);
  CHECK(draw.unavailable_reason == diagnostic::DrawCaptureReason::kNone);
  CHECK(result.texture_bindings_complete);
  const auto required =
      std::find_if(result.textures.begin(), result.textures.end(),
                   [](const auto& view) { return !view.is_signed && view.required; });
  REQUIRE(required != result.textures.end());
  CHECK(required->available);
  CHECK(required->format == static_cast<uint32_t>(xenos::TextureFormat::k_DXT2_3));
  CHECK(required->base_size == 0x8000);
  CHECK(required->mip_size == 0x8E80);
  CHECK(required->width == 128);
  CHECK(required->height == 256);
  CHECK(required->pitch == 4);
  CHECK(required->mip_max_level == 8);
  CHECK(required->fetch_words.size() == 6);
  CHECK(required->swizzled_signs == 0);
  CHECK_FALSE(required->memory_version.identity);
  CHECK_FALSE(required->memory_version.generation);
  CHECK_FALSE(required->memory_version.check);
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);

  const uint32_t base_address = key.base_page << 12;
  const uint32_t mip_address = key.mip_page << 12;
  const uint32_t tail_address = mip_address + 0x8000;
  const uint32_t tail_end = mip_address + 0x8E80;
  const auto overlaps_tail = [tail_address, tail_end](uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    return end > tail_address && address < tail_end;
  };
  const auto expect_tail_rejection = [&](std::function<bool(uint32_t, uint32_t)> dirty,
                                         std::function<bool(uint32_t, uint32_t)> gpu_written,
                                         std::function<bool(uint32_t, uint32_t)> readable) {
    diagnostic::DrawCaptureFrameInputDrawView rejected_draw;
    FrameInputDrawBuilderResult rejected_result;
    CHECK_FALSE(build(std::move(dirty), std::move(gpu_written), std::move(readable), rejected_draw,
                      rejected_result));
    CHECK_FALSE(rejected_draw.cpu_data_complete);
    CHECK(rejected_draw.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    const auto rejected =
        std::find_if(rejected_result.textures.begin(), rejected_result.textures.end(),
                     [](const auto& view) { return !view.is_signed && view.required; });
    REQUIRE(rejected != rejected_result.textures.end());
    CHECK_FALSE(rejected->available);
    CHECK(rejected->unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
    CHECK(rejected->base_address == base_address);
    CHECK(rejected->base_size == 0x8000);
    CHECK(rejected->mip_address == mip_address);
    CHECK(rejected->mip_size == 0x8E80);
  };

  bool dirty_tail_seen = false;
  bool dirty_tail_size_valid = true;
  expect_tail_rejection(
      [&dirty_tail_seen, &dirty_tail_size_valid, &overlaps_tail, mip_address](uint32_t address,
                                                                              uint32_t size) {
        if (overlaps_tail(address, size)) {
          dirty_tail_seen = true;
          dirty_tail_size_valid &= address == mip_address && size == 0x8E80;
          return true;
        }
        return false;
      },
      [](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t) { return true; });
  CHECK(dirty_tail_seen);
  CHECK(dirty_tail_size_valid);

  bool gpu_tail_seen = false;
  bool gpu_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [&gpu_tail_seen, &gpu_tail_size_valid, &overlaps_tail, mip_address](
                            uint32_t address, uint32_t size) {
                          if (overlaps_tail(address, size)) {
                            gpu_tail_seen = true;
                            gpu_tail_size_valid &= address == mip_address && size == 0x8E80;
                            return true;
                          }
                          return false;
                        },
                        [](uint32_t, uint32_t) { return true; });
  CHECK(gpu_tail_seen);
  CHECK(gpu_tail_size_valid);

  bool unreadable_tail_seen = false;
  bool unreadable_tail_size_valid = true;
  expect_tail_rejection([](uint32_t, uint32_t) { return false; },
                        [](uint32_t, uint32_t) { return false; },
                        [&unreadable_tail_seen, &unreadable_tail_size_valid, &overlaps_tail,
                         mip_address](uint32_t address, uint32_t size) {
                          if (overlaps_tail(address, size)) {
                            unreadable_tail_seen = true;
                            unreadable_tail_size_valid &= address == mip_address && size == 0x8E80;
                            return false;
                          }
                          return true;
                        });
  CHECK(unreadable_tail_seen);
  CHECK(unreadable_tail_size_valid);

  const auto reject_fetch = [&](auto mutate) {
    auto near_miss = selected_fetch;
    mutate(near_miss);
    DrawCaptureFrameInputTextureView view;
    CHECK_FALSE(cache.PrepareFrameInputTexture(near_miss, view));
    CHECK_FALSE(view.available);
  };
  reject_fetch([](auto& fetch) {
    fetch.size_2d.width = 255;
    fetch.size_2d.height = 127;
  });
  reject_fetch([](auto& fetch) { fetch.mip_max_level = 7; });
  reject_fetch([](auto& fetch) { fetch.mip_max_level = 9; });
  reject_fetch([](auto& fetch) { fetch.format = xenos::TextureFormat::k_DXT1; });
  reject_fetch([](auto& fetch) { fetch.endianness = xenos::Endian::kNone; });
  reject_fetch([](auto& fetch) { fetch.swizzle = 0x680; });
  reject_fetch([](auto& fetch) { fetch.sign_y = xenos::TextureSign::kSigned; });
  reject_fetch([](auto& fetch) { fetch.tiled = true; });
  reject_fetch([](auto& fetch) { fetch.dimension = xenos::DataDimension::kCube; });
  reject_fetch([](auto& fetch) { fetch.stacked = true; });
  reject_fetch([](auto& fetch) { fetch.mip_min_level = 1; });
  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(selected_fetch, signed_view));
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

TEST_CASE("Prepared signed RG16 float view uses the complete tiled base extent",
          "[graphics][prepared][native]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);

  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.size_2d.width = 1279;
  fetch.size_2d.height = 719;
  fetch.pitch = 40;
  fetch.base_address = 16;
  fetch.format = xenos::TextureFormat::k_16_16_FLOAT;
  fetch.tiled = true;
  // Captured dword_3 encodes R,G,1,1 (0xB48), which becomes the D3D12
  // component mapping 0x1B48 after the builder's validation bit.
  fetch.swizzle = 0xB48;
  fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w = xenos::TextureSign::kSigned;

  DrawCaptureFrameInputTextureView signed_view;
  signed_view.is_signed = true;
  REQUIRE(cache.PrepareFrameInputTexture(fetch, signed_view));
  TestTextureCache::TextureKey key;
  TestTextureCache::BindingInfoFromFetchConstant(fetch, key, nullptr);
  const auto layout = key.GetGuestLayout();
  TextureInfo legacy;
  REQUIRE(TextureInfo::Prepare(fetch, &legacy));
  CHECK(legacy.memory.base_size < layout.base.level_data_extent_bytes);
  CHECK(signed_view.base_address == key.base_page << 12);
  CHECK(signed_view.base_size == layout.base.level_data_extent_bytes);
  CHECK(signed_view.base_size == 0x398000);
  CHECK(signed_view.swizzled_signs == 0x55);
  CHECK(signed_view.required);

  // The same undercovered layout remains rejected when the binding is not the
  // exact selected signed form.
  DrawCaptureFrameInputTextureView unsigned_view;
  CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, unsigned_view));
  fetch.sign_y = xenos::TextureSign::kUnsigned;
  DrawCaptureFrameInputTextureView mixed_sign_view;
  mixed_sign_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, mixed_sign_view));
  fetch.sign_y = xenos::TextureSign::kSigned;

  for (const auto bad_sign : {xenos::TextureSign::kUnsigned, xenos::TextureSign::kUnsignedBiased}) {
    fetch.sign_z = bad_sign;
    DrawCaptureFrameInputTextureView z_sign_view;
    z_sign_view.is_signed = true;
    CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, z_sign_view));
    fetch.sign_z = xenos::TextureSign::kSigned;

    fetch.sign_w = bad_sign;
    DrawCaptureFrameInputTextureView w_sign_view;
    w_sign_view.is_signed = true;
    CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, w_sign_view));
    fetch.sign_w = xenos::TextureSign::kSigned;
  }

  fetch.packed_mips = true;
  DrawCaptureFrameInputTextureView packed_view;
  packed_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, packed_view));
  fetch.packed_mips = false;
  fetch.size_2d.width = 1278;
  DrawCaptureFrameInputTextureView width_mismatch_view;
  width_mismatch_view.is_signed = true;
  CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, width_mismatch_view));
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

  void ConfigureMainClear() {
    const std::array<float, 6> vertices{-0.5F, -0.5F, 639.5F, -0.5F, 639.5F, 719.5F};
    std::memcpy(memory.TranslatePhysical(address), vertices.data(), sizeof(vertices));
    registers[XE_GPU_REG_RB_SURFACE_INFO] = 0x0A010280U;
    registers[XE_GPU_REG_RB_COLOR_INFO] = 0;
    registers[XE_GPU_REG_RB_DEPTH_INFO] = 0x530;
    registers[XE_GPU_REG_RB_MODECONTROL] = 6;
    registers[XE_GPU_REG_PA_SC_WINDOW_OFFSET] = 0;
    registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 0;
    registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 640U | (720U << 16);
    registers[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL] = 0;
    registers[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 0x20002000U;
    registers[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0x00010000U;
    registers[XE_GPU_REG_RB_COPY_CONTROL] = 0x00100340U;
    registers[XE_GPU_REG_RB_COPY_DEST_PITCH] = 0x02D00500U;
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x01000300U;
    registers[XE_GPU_REG_RB_HIZ_CLEAR] = 0x00000FFFU;
    registers[XE_GPU_REG_RB_DEPTH_CLEAR] = 0xFFFFFF00U;
    registers[XE_GPU_REG_RB_COLOR_CLEAR] = 0;
    registers[XE_GPU_REG_RB_COLOR_CLEAR_LO] = 0;
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

TEST_CASE("Prepared main clear admits only the selected color zero form", "[graphics][prepared]") {
  ResolveFixture fixture;
  fixture.ConfigureMainClear();
  const auto accepted = fixture.Plan();
  REQUIRE(accepted.cpu_data_complete);
  CHECK_FALSE(accepted.original_succeeded);
  CHECK(accepted.written_address == 0);
  CHECK(accepted.written_length == 0);

  for (const auto mutation :
       {0x231CU, 0x231DU, 0x231EU, 0x231FU, 0x2001U, 0x231BU, 0x2080U, 0x2081U, 0x2082U, 0x2205U}) {
    fixture.ConfigureMainClear();
    fixture.registers[mutation] ^= 1U;
    CHECK_FALSE(fixture.Plan().cpu_data_complete);
  }
  fixture.ConfigureMainClear();
  fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = 0x00100341U;
  CHECK_FALSE(fixture.Plan().cpu_data_complete);
}

TEST_CASE("Prepared main no-clear right half keeps the ordinary boundary", "[graphics][prepared]") {
  ResolveFixture fixture;
  fixture.ConfigureMainClear();
  fixture.registers[XE_GPU_REG_RB_COPY_CONTROL] = 0x00100040U;
  fixture.registers[XE_GPU_REG_PA_SC_WINDOW_OFFSET] = 0x00007D80U;
  fixture.registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 640U;
  fixture.registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 1280U | (720U << 16);
  fixture.registers[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0x00010000U;
  const std::array<float, 6> vertices{639.5F, -0.5F, 1279.5F, -0.5F, 1279.5F, 719.5F};
  std::memcpy(fixture.memory.TranslatePhysical(fixture.address), vertices.data(), sizeof(vertices));
  fixture.registers[XE_GPU_REG_RB_COPY_DEST_BASE] += 0x14000U;
  const auto copy = fixture.Plan();
  REQUIRE(copy.cpu_data_complete);
  draw_util::ResolveInfo original;
  REQUIRE(
      draw_util::GetResolveInfo(fixture.registers, fixture.memory, 1, 1, false, false, original));
  CHECK(copy.source_x == original.coordinate_info.edram_offset_x_div_8 * 8);
  CHECK(copy.source_y == original.coordinate_info.edram_offset_y_div_8 * 8);
  CHECK(copy.source_width == original.coordinate_info.width_div_8 * 8);
  CHECK(copy.source_height == original.height_div_8 * 8);
  CHECK(copy.planned_address == original.copy_dest_extent_start);
  CHECK(copy.planned_length == original.copy_dest_extent_length);
  CHECK_FALSE(copy.original_succeeded);
  CHECK(copy.written_address == 0);
  CHECK(copy.written_length == 0);
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

TEST_CASE("Prepared tiled BC writers preserve exact canonical ranges", "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  struct Form {
    xenos::TextureFormat format;
    uint32_t width, height, pitch, max_level, base, mips, legacy_base, legacy_mips;
  };
  constexpr Form kForms[] = {
      {xenos::TextureFormat::k_DXT1, 128, 64, 4, 7, 8192, 16384, 4096, 8192},
      {xenos::TextureFormat::k_DXT1, 256, 128, 8, 8, 16384, 24576, 16384, 16384},
      {xenos::TextureFormat::k_DXT2_3, 128, 64, 4, 7, 16384, 32768, 8192, 16384},
      {xenos::TextureFormat::k_DXT2_3, 32, 64, 4, 0, 16384, 0, 8192, 0},
      {xenos::TextureFormat::k_DXT4_5, 128, 128, 4, 0, 16384, 0, 16384, 0},
      {xenos::TextureFormat::k_DXT4_5, 128, 128, 4, 7, 16384, 49152, 16384, 32768},
  };
  for (const auto& form : kForms) {
    CAPTURE(form.format, form.width, form.height, form.max_level);
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    fetch.format = form.format;
    fetch.size_2d.width = form.width - 1;
    fetch.size_2d.height = form.height - 1;
    fetch.pitch = form.pitch;
    fetch.base_address = 0x10000 >> 12;
    fetch.mip_address = form.max_level ? 0x1000000 >> 12 : 0;
    fetch.tiled = true;
    fetch.endianness = xenos::Endian::kNone;
    fetch.swizzle = 0x688;
    fetch.packed_mips = form.max_level != 0;
    fetch.mip_max_level = form.max_level;
    TextureInfo legacy;
    REQUIRE(TextureInfo::Prepare(fetch, &legacy));
    CHECK(legacy.memory.base_size == form.legacy_base);
    CHECK(legacy.memory.mip_size == form.legacy_mips);
    TestTextureCache::TextureKey key;
    TestTextureCache::BindingInfoFromFetchConstant(fetch, key, nullptr);
    const auto layout = key.GetGuestLayout();
    CHECK(layout.base.level_data_extent_bytes == form.base);
    CHECK(layout.mips_total_extent_bytes == form.mips);
    DrawCaptureFrameInputTextureView view;
    REQUIRE(cache.PrepareFrameInputTexture(fetch, view));
    CHECK(view.available);
    CHECK(view.base_size == form.base);
    CHECK(view.mip_size == form.mips);
    CHECK(view.tiled);
    CHECK(view.endianness == 0);
    CHECK(view.pitch == form.pitch << 5);
    CHECK(view.swizzled_signs == 0);
    CHECK(view.packed_mips == (form.max_level != 0));
    if (form.format == xenos::TextureFormat::k_DXT4_5 && !form.max_level) {
      // B75's ordinary legacy range already covers the complete tiled base.
      // A non-selected endian neighbor also uses ordinary admission.
      CHECK(legacy.memory.base_size >= layout.base.level_data_extent_bytes);
      fetch.endianness = xenos::Endian::k8in16;
      DrawCaptureFrameInputTextureView ordinary;
      REQUIRE(cache.PrepareFrameInputTexture(fetch, ordinary));
      CHECK(ordinary.base_size == form.base);
      continue;
    }
    CHECK((legacy.memory.base_size < layout.base.level_data_extent_bytes ||
           legacy.memory.mip_size < layout.mips_total_extent_bytes));
    const auto reject = [&](const char* name, auto mutate) {
      CAPTURE(name);
      auto neighbor = fetch;
      mutate(neighbor);
      TextureInfo neighbor_legacy;
      REQUIRE(TextureInfo::Prepare(neighbor, &neighbor_legacy));
      TestTextureCache::TextureKey neighbor_key;
      TestTextureCache::BindingInfoFromFetchConstant(neighbor, neighbor_key, nullptr);
      const auto neighbor_layout = neighbor_key.GetGuestLayout();
      CHECK((neighbor_legacy.memory.base_size < neighbor_layout.base.level_data_extent_bytes ||
             neighbor_legacy.memory.mip_size < neighbor_layout.mips_total_extent_bytes));
      DrawCaptureFrameInputTextureView rejected;
      CHECK_FALSE(cache.PrepareFrameInputTexture(neighbor, rejected));
      CHECK_FALSE(rejected.available);
    };
    if (form.width == form.pitch << 5) {
      reject("pitch", [](auto& f) { ++f.pitch; });
    } else {
      // The padded form's wider neighbor is fully covered by its legacy range
      // and takes ordinary admission.
      auto wider = fetch;
      ++wider.pitch;
      TextureInfo wider_legacy;
      REQUIRE(TextureInfo::Prepare(wider, &wider_legacy));
      TestTextureCache::TextureKey wider_key;
      TestTextureCache::BindingInfoFromFetchConstant(wider, wider_key, nullptr);
      CHECK(wider_legacy.memory.base_size >=
            wider_key.GetGuestLayout().base.level_data_extent_bytes);
      DrawCaptureFrameInputTextureView ordinary;
      CHECK(cache.PrepareFrameInputTexture(wider, ordinary));
    }
    reject("endian", [](auto& f) { f.endianness = xenos::Endian::k8in16; });
    reject("swizzle", [](auto& f) { f.swizzle = 0x680; });
    reject("sign", [](auto& f) { f.sign_y = xenos::TextureSign::kSigned; });
    reject("num_format", [](auto& f) { f.num_format = 1; });
    reject("exp_adjust", [](auto& f) { f.exp_adjust = 1; });
    reject("force_bc_w", [](auto& f) { f.force_bc_w_to_max = 1; });
    reject("width", [](auto& f) { --f.size_2d.width; });
    reject("height", [](auto& f) { --f.size_2d.height; });
    if (form.max_level)
      reject("mip_max", [](auto& f) { --f.mip_max_level; });
    if (!form.max_level || form.legacy_base < form.base)
      reject("mip_address", [](auto& f) { f.mip_address = f.mip_address ? 0 : 0x1000000 >> 12; });
    DrawCaptureFrameInputTextureView signed_view;
    signed_view.is_signed = true;
    CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, signed_view));
  }
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);
}

TEST_CASE("Prepared unsigned linear BC matrix preserves exact range admission",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  struct Form {
    xenos::TextureFormat format;
    uint32_t width, height, pitch, max_level, base, base_extent, mips;
  };
  // Count 4x4 blocks with 8-byte BC1 or 16-byte BC2 payloads and 32-block strides.
  // Packed tails occupy 8x4 blocks for squares or 8x8 for these 2:1 rectangles;
  // mips includes preceding aligned levels and the tail's final partial row.
  constexpr Form kForms[] = {
      {xenos::TextureFormat::k_DXT1, 8, 8, 4, 0, 512, 272, 0},
      {xenos::TextureFormat::k_DXT1, 32, 32, 4, 5, 2048, 1856, 832},
      {xenos::TextureFormat::k_DXT1, 512, 256, 16, 9, 65536, 65536, 34624},
      {xenos::TextureFormat::k_DXT1, 512, 512, 16, 0, 131072, 131072, 0},
      {xenos::TextureFormat::k_DXT1, 1024, 1024, 32, 0, 524288, 524288, 0},
      {xenos::TextureFormat::k_DXT1, 1024, 1024, 32, 10, 524288, 524288, 189248},
      {xenos::TextureFormat::k_DXT1, 2048, 2048, 64, 0, 2097152, 2097152, 0},
      {xenos::TextureFormat::k_DXT1, 4096, 4096, 128, 0, 8388608, 8388608, 0},
      {xenos::TextureFormat::k_DXT2_3, 128, 64, 4, 7, 8192, 8192, 20096},
      {xenos::TextureFormat::k_DXT2_3, 256, 128, 8, 8, 32768, 32768, 36480},
      {xenos::TextureFormat::k_DXT2_3, 256, 256, 8, 0, 65536, 65536, 0},
      {xenos::TextureFormat::k_DXT2_3, 256, 256, 8, 8, 65536, 65536, 50816},
      {xenos::TextureFormat::k_DXT2_3, 512, 256, 16, 9, 131072, 131072, 69248},
      {xenos::TextureFormat::k_DXT2_3, 512, 512, 16, 9, 262144, 262144, 116352},
  };
  for (const auto& form : kForms) {
    CAPTURE(form.format, form.width, form.height, form.pitch, form.max_level);
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    fetch.format = form.format;
    fetch.size_2d.width = form.width - 1;
    fetch.size_2d.height = form.height - 1;
    fetch.pitch = form.pitch;
    fetch.base_address = 0x10000 >> 12;
    fetch.mip_address = form.max_level ? 0x1000000 >> 12 : 0;
    fetch.endianness = xenos::Endian::k8in16;
    fetch.swizzle = 0x688;
    fetch.packed_mips = form.max_level != 0;
    fetch.mip_max_level = form.max_level;
    TextureInfo legacy;
    REQUIRE(TextureInfo::Prepare(fetch, &legacy));
    TestTextureCache::TextureKey key;
    TestTextureCache::BindingInfoFromFetchConstant(fetch, key, nullptr);
    const auto layout = key.GetGuestLayout();
    CHECK(layout.base.level_data_extent_bytes == form.base_extent);
    CHECK(layout.mips_total_extent_bytes == form.mips);
    DrawCaptureFrameInputTextureView view;
    REQUIRE(cache.PrepareFrameInputTexture(fetch, view));
    CHECK(view.available);
    CHECK(view.base_size == form.base);
    CHECK(view.mip_size == form.mips);
    CHECK(view.base_size == legacy.memory.base_size);
    CHECK(view.base_size >= layout.base.level_data_extent_bytes);
    if (form.max_level) {
      CHECK(legacy.memory.mip_size < layout.mips_total_extent_bytes);
      CHECK(view.mip_size == layout.mips_total_extent_bytes);
      const auto reject = [&](auto mutate) {
        auto neighbor = fetch;
        mutate(neighbor);
        DrawCaptureFrameInputTextureView rejected;
        CHECK_FALSE(cache.PrepareFrameInputTexture(neighbor, rejected));
        CHECK_FALSE(rejected.available);
      };
      reject([](auto& f) { ++f.pitch; });
      reject([](auto& f) { --f.mip_max_level; });
      reject([](auto& f) { f.tiled = true; });
      reject([](auto& f) { f.sign_y = xenos::TextureSign::kSigned; });
      reject([](auto& f) { f.format = xenos::TextureFormat::k_DXN; });
      if ((form.width == 32 && form.height == 32) || form.width == 1024 ||
          (form.width == 128 && form.height == 64)) {
        reject([](auto& f) { f.format = xenos::TextureFormat::k_DXT4_5; });
      }
      if (form.format == xenos::TextureFormat::k_DXT2_3 && form.width == 128 && form.height == 64) {
        reject([](auto& f) { f.format = xenos::TextureFormat::k_DXT1; });
      }
      if (form.format == xenos::TextureFormat::k_DXT1 && form.width == 32 && form.height == 32) {
        reject([](auto& f) { f.format = xenos::TextureFormat::k_DXT2_3; });
      }
      reject([](auto& f) { f.stacked = true; });
      reject([](auto& f) { f.endianness = xenos::Endian::kNone; });
      reject([](auto& f) { f.swizzle = 0x680; });
      DrawCaptureFrameInputTextureView signed_view;
      signed_view.is_signed = true;
      CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, signed_view));
    } else {
      // No predicate correction is needed: the complete ordinary base range
      // covers even the 8x8/pitch4 form, and there is no mip bank to extend.
      CHECK(view.mip_size == 0);
      CHECK(layout.mips_total_extent_bytes == 0);
      CHECK(legacy.memory.base_size >= layout.base.level_data_extent_bytes);
      auto malformed = fetch;
      malformed.pitch = 0;
      DrawCaptureFrameInputTextureView rejected;
      CHECK_FALSE(cache.PrepareFrameInputTexture(malformed, rejected));
    }
  }
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);
}

TEST_CASE("Prepared unsigned linear BC3 matrix preserves exact range admission",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  struct Form {
    xenos::TextureFormat format;
    uint32_t width, height, pitch, max_level, base, base_extent, mips;
  };
  // BC3 uses 16-byte 4x4 blocks: the square tail ends after 3 * 512 + 8 * 16
  // bytes, and the 2:1 tail after 7 * 512 + 8 * 16, following aligned levels.
  constexpr Form kForms[] = {
      {xenos::TextureFormat::k_DXT4_5, 64, 64, 4, 6, 8192, 7936, 18048},
      {xenos::TextureFormat::k_DXT4_5, 128, 128, 4, 7, 16384, 16384, 34432},
      {xenos::TextureFormat::k_DXT4_5, 256, 128, 8, 0, 32768, 32768, 0},
      {xenos::TextureFormat::k_DXT4_5, 256, 128, 8, 8, 32768, 32768, 36480},
      {xenos::TextureFormat::k_DXT4_5, 256, 256, 8, 8, 65536, 65536, 50816},
      {xenos::TextureFormat::k_DXT4_5, 512, 256, 16, 9, 131072, 131072, 69248},
      {xenos::TextureFormat::k_DXT4_5, 512, 512, 16, 0, 262144, 262144, 0},
      {xenos::TextureFormat::k_DXT4_5, 512, 512, 16, 9, 262144, 262144, 116352},
  };
  for (const auto& form : kForms) {
    CAPTURE(form.width, form.height, form.pitch, form.max_level);
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    fetch.format = form.format;
    fetch.size_2d.width = form.width - 1;
    fetch.size_2d.height = form.height - 1;
    fetch.pitch = form.pitch;
    fetch.base_address = 0x10000 >> 12;
    fetch.mip_address = form.max_level ? 0x1000000 >> 12 : 0;
    fetch.endianness = xenos::Endian::k8in16;
    fetch.swizzle = 0x688;
    fetch.packed_mips = form.max_level != 0;
    fetch.mip_max_level = form.max_level;
    TextureInfo legacy;
    REQUIRE(TextureInfo::Prepare(fetch, &legacy));
    TestTextureCache::TextureKey key;
    TestTextureCache::BindingInfoFromFetchConstant(fetch, key, nullptr);
    const auto layout = key.GetGuestLayout();
    CHECK(layout.base.level_data_extent_bytes == form.base_extent);
    CHECK(layout.mips_total_extent_bytes == form.mips);
    DrawCaptureFrameInputTextureView view;
    REQUIRE(cache.PrepareFrameInputTexture(fetch, view));
    CHECK(view.available);
    CHECK(view.base_size == form.base);
    CHECK(view.mip_size == form.mips);
    CHECK(view.format == static_cast<uint32_t>(form.format));
    CHECK(view.endianness == static_cast<uint32_t>(xenos::Endian::k8in16));
    CHECK(view.dimension == static_cast<uint32_t>(xenos::DataDimension::k2DOrStacked));
    CHECK(view.width == form.width - 1);
    CHECK(view.height == form.height - 1);
    CHECK(view.depth == 0);
    CHECK(view.pitch == form.pitch * 32);
    CHECK(view.mip_min_level == 0);
    CHECK(view.mip_max_level == form.max_level);
    CHECK(view.packed_mips == (form.max_level != 0));
    CHECK_FALSE(view.tiled);
    CHECK(view.swizzled_signs == 0);
    CHECK(view.base_size == legacy.memory.base_size);
    CHECK(view.base_size >= layout.base.level_data_extent_bytes);
    if (form.max_level) {
      CHECK(legacy.memory.mip_size < layout.mips_total_extent_bytes);
      CHECK(view.mip_size == layout.mips_total_extent_bytes);
      const auto reject = [&](auto mutate) {
        auto neighbor = fetch;
        mutate(neighbor);
        DrawCaptureFrameInputTextureView rejected;
        CHECK_FALSE(cache.PrepareFrameInputTexture(neighbor, rejected));
        CHECK_FALSE(rejected.available);
      };
      reject([](auto& f) { ++f.pitch; });
      reject([](auto& f) { --f.mip_max_level; });
      reject([](auto& f) { f.tiled = true; });
      reject([](auto& f) { f.sign_y = xenos::TextureSign::kSigned; });
      reject([](auto& f) { f.format = xenos::TextureFormat::k_DXN; });
      reject([](auto& f) { --f.size_2d.width; });
      reject([](auto& f) { --f.size_2d.height; });
      reject([](auto& f) { f.mip_min_level = 1; });
      reject([](auto& f) { ++f.mip_max_level; });
      reject([](auto& f) { f.sign_x = xenos::TextureSign::kSigned; });
      reject([](auto& f) { f.sign_z = xenos::TextureSign::kSigned; });
      reject([](auto& f) { f.sign_w = xenos::TextureSign::kSigned; });
      reject([](auto& f) { f.size_2d.stack_depth = 1; });
      reject([](auto& f) { f.num_format = 1; });
      reject([](auto& f) { f.exp_adjust = 1; });
      reject([](auto& f) { f.force_bc_w_to_max = 1; });
      reject([](auto& f) { f.base_address = 0; });
      reject([](auto& f) { f.dimension = xenos::DataDimension::k3D; });
      reject([](auto& f) { f.stacked = true; });
      reject([](auto& f) { f.endianness = xenos::Endian::kNone; });
      reject([](auto& f) { f.swizzle = 0x680; });
      DrawCaptureFrameInputTextureView signed_view;
      signed_view.is_signed = true;
      CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, signed_view));
      // Removing packing is an ordinarily covered chain, not another range
      // exception. Its legacy size must remain unchanged.
      auto ordinary = fetch;
      ordinary.packed_mips = false;
      TextureInfo ordinary_legacy;
      REQUIRE(TextureInfo::Prepare(ordinary, &ordinary_legacy));
      DrawCaptureFrameInputTextureView ordinary_view;
      REQUIRE(cache.PrepareFrameInputTexture(ordinary, ordinary_view));
      CHECK(ordinary_view.mip_size == ordinary_legacy.memory.mip_size);
    } else {
      // No predicate correction is needed: the complete ordinary base range
      // covers the single-level forms, and there is no mip bank to extend.
      CHECK(view.mip_size == 0);
      CHECK(layout.mips_total_extent_bytes == 0);
      CHECK(legacy.memory.base_size >= layout.base.level_data_extent_bytes);
      auto malformed = fetch;
      malformed.pitch = 0;
      DrawCaptureFrameInputTextureView rejected;
      CHECK_FALSE(cache.PrepareFrameInputTexture(malformed, rejected));
      malformed = fetch;
      --malformed.pitch;
      CHECK_FALSE(cache.PrepareFrameInputTexture(malformed, rejected));
      malformed = fetch;
      malformed.base_address = 0;
      CHECK_FALSE(cache.PrepareFrameInputTexture(malformed, rejected));
      malformed = fetch;
      malformed.stacked = true;
      CHECK_FALSE(cache.PrepareFrameInputTexture(malformed, rejected));
      malformed = fetch;
      malformed.dimension = xenos::DataDimension::kCube;
      CHECK_FALSE(cache.PrepareFrameInputTexture(malformed, rejected));
      malformed = fetch;
      malformed.mip_min_level = 1;
      CHECK_FALSE(cache.PrepareFrameInputTexture(malformed, rejected));
    }
  }
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);
}

TEST_CASE("Prepared R8 and R32_FLOAT writers use ordinary admission", "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  constexpr std::array<std::array<uint32_t, 6>, 2> words{
      {{0x08000002, 0x1FB17002, 0x007FE3FF, 0x00000248, 0, 0x200},
       {0x84000156, 0x185920A4, 0x003FE1FF, 0x000016D1, 0, 0x200}}};
  for (size_t index = 0; index < words.size(); ++index) {
    CAPTURE(index);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words[index].data(), sizeof(fetch));
    TextureInfo legacy;
    REQUIRE(TextureInfo::Prepare(fetch, &legacy));
    TestTextureCache::TextureKey key;
    TestTextureCache::BindingInfoFromFetchConstant(fetch, key, nullptr);
    const auto layout = key.GetGuestLayout();
    REQUIRE(legacy.memory.base_size == 1048576);
    REQUIRE(layout.base.level_data_extent_bytes == legacy.memory.base_size);
    CHECK(layout.base.row_pitch_bytes == (index ? 2048 : 1024));
    CHECK(layout.base.array_slice_stride_bytes == 1048576);
    CHECK(layout.mips_total_extent_bytes == 0);
    DrawCaptureFrameInputTextureView view;
    view.is_signed = index != 0;
    // R32_FLOAT replicates its only channel before composing guest selectors.
    cache.host_swizzle = index ? 0 : xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    REQUIRE(cache.PrepareFrameInputTexture(fetch, view));
    CHECK(view.available);
    CHECK(view.required);
    CHECK(view.base_size == legacy.memory.base_size);
    CHECK(view.mip_size == 0);
    CHECK(view.swizzled_signs == (index ? 0x55 : 0));
    CHECK(view.component_mapping == (index ? 0xB68 : 0x124));
    CHECK(view.mip_min_level == 0);
    CHECK(view.mip_max_level == 0);

    auto neighbor = fetch;
    if (index) {
      // One missing visible row still addresses the final tiled allocation.
      --neighbor.size_2d.height;
    } else {
      // The adjacent packed full chain lacks ordinary tail coverage.
      neighbor.packed_mips = true;
      neighbor.mip_address = 0x100;
      neighbor.mip_max_level = 10;
    }
    TextureInfo undercovered;
    REQUIRE(TextureInfo::Prepare(neighbor, &undercovered));
    TestTextureCache::TextureKey neighbor_key;
    TestTextureCache::BindingInfoFromFetchConstant(neighbor, neighbor_key, nullptr);
    const auto neighbor_layout = neighbor_key.GetGuestLayout();
    REQUIRE((undercovered.memory.base_size < neighbor_layout.base.level_data_extent_bytes ||
             undercovered.memory.mip_size < neighbor_layout.mips_total_extent_bytes));
    DrawCaptureFrameInputTextureView rejected;
    rejected.is_signed = index != 0;
    CHECK_FALSE(cache.PrepareFrameInputTexture(neighbor, rejected));
    CHECK_FALSE(rejected.available);
  }
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);
}

TEST_CASE("Prepared RGBA8/R8 matrix keeps exact range and LOD normalization",
          "[graphics][prepared]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  struct Form {
    std::array<uint32_t, 6> words;
    uint32_t legacy_base, base, legacy_mips, mips, max_level, lod0_offset;
    bool extended;
  };
  constexpr Form forms[] = {
      {{0x00800002, 0x1BB79086, 0x0000E007, 0x00000C14, 0x000000C0, 0x00000A00},
       2048,
       2048,
       0,
       0,
       0,
       64,
       false},
      {{0x04000002, 0x1B195002, 0x003FE1FF, 0x00000248, 0x00000240, 0x1B175A00},
       262144,
       262144,
       122880,
       126752,
       9,
       0,
       true},
      {{0x02000002, 0x1B10D086, 0x000060FF, 0x00000C14, 0x00000200, 0x00000A00},
       4096,
       20480,
       0,
       0,
       0,
       16384,
       true},
      {{0x02000002, 0x1B105086, 0x000000FF, 0x00000C14, 0x00000200, 0x00000A00},
       1024,
       17408,
       0,
       0,
       0,
       16384,
       true},
      {{0x80400002, 0x1BC8D086, 0x0003E01F, 0x00000C14, 0, 0x00000200},
       4096,
       4096,
       0,
       0,
       0,
       0,
       false},
      {{0x81000002, 0x1BCAD086, 0x0007E07F, 0x00000C14, 0, 0x00000200},
       32768,
       32768,
       0,
       0,
       0,
       0,
       false},
      {{0x80800002, 0x164DE086, 0x0007E03F, 0x00000C14, 0, 0x00000200},
       16384,
       16384,
       0,
       0,
       0,
       0,
       false},
      {{0x81000002, 0x155BE086, 0x000FE07F, 0x00000C14, 0, 0x00000200},
       65536,
       65536,
       0,
       0,
       0,
       0,
       false},
      {{0x80800002, 0x1BD2E086, 0x000FE03F, 0x00000C14, 0, 0x00000200},
       32768,
       32768,
       0,
       0,
       0,
       0,
       false}};
  for (const auto& form : forms) {
    CAPTURE(form.words);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, form.words.data(), sizeof(fetch));
    TextureInfo legacy;
    REQUIRE(TextureInfo::Prepare(fetch, &legacy));
    CHECK(legacy.memory.base_size == form.legacy_base);
    CHECK(legacy.memory.mip_size == form.legacy_mips);
    TestTextureCache::TextureKey key;
    TestTextureCache::BindingInfoFromFetchConstant(fetch, key, nullptr);
    const auto layout = key.GetGuestLayout();
    CHECK(key.mip_max_level == form.max_level);
    CHECK(legacy.mip_max_level == form.max_level);
    DrawCaptureFrameInputTextureView view;
    REQUIRE(cache.PrepareFrameInputTexture(fetch, view));
    CHECK(view.available);
    CHECK(view.base_size == form.base);
    CHECK(view.mip_size == form.mips);
    CHECK(view.mip_max_level == form.max_level);
    CHECK(view.base_address == legacy.memory.base_address);
    CHECK(view.mip_address == legacy.memory.mip_address);
    CHECK(view.base_size >= layout.base.level_data_extent_bytes);
    CHECK(layout.mips_total_extent_bytes == form.mips);
    if (fetch.packed_mips && layout.packed_level == 0) {
      uint32_t x, y, z;
      REQUIRE(texture_util::GetPackedMipOffset(fetch.size_2d.width + 1, fetch.size_2d.height + 1, 1,
                                               fetch.format, 0, x, y, z));
      CHECK(x * FormatInfo::Get(fetch.format)->bytes_per_block() +
                y * layout.base.row_pitch_bytes ==
            form.lod0_offset);
      CHECK(fetch.mip_address == 0);
      CHECK(fetch.mip_max_level > view.mip_max_level);
    }
    const auto reject = [&](auto mutate) {
      auto neighbor = fetch;
      mutate(neighbor);
      DrawCaptureFrameInputTextureView rejected;
      CHECK_FALSE(cache.PrepareFrameInputTexture(neighbor, rejected));
      CHECK_FALSE(rejected.available);
    };
    reject([](auto& f) { f.pitch = 0; });
    reject([](auto& f) { f.mip_min_level = 1; });
    reject([](auto& f) { f.stacked = true; });
    if (form.extended) {
      reject([](auto& f) { ++f.pitch; });
      reject([](auto& f) { --f.mip_max_level; });
      reject([](auto& f) { f.tiled = true; });
      reject([](auto& f) { f.sign_y = xenos::TextureSign::kSigned; });
      reject([](auto& f) {
        f.endianness =
            f.endianness == xenos::Endian::kNone ? xenos::Endian::k8in32 : xenos::Endian::kNone;
      });
      reject([](auto& f) {
        f.format = f.format == xenos::TextureFormat::k_8 ? xenos::TextureFormat::k_8_8
                                                         : xenos::TextureFormat::k_16_16;
      });
      reject([](auto& f) { f.swizzle ^= 1; });
      reject([](auto& f) { f.num_format = 1; });
      reject([](auto& f) { f.exp_adjust = 1; });
      reject([](auto& f) { f.force_bc_w_to_max = 1; });
      reject([](auto& f) { f.size_2d.stack_depth = 1; });
      reject([](auto& f) { --f.size_2d.width; });
      // 256x4 -> 256x8, 256x1 -> 256x2, 512x512 -> 512x256.
      reject([](auto& f) {
        f.size_2d.height = f.size_2d.height == 3 ? 7 : f.size_2d.height == 0 ? 1 : 255;
      });
      reject([](auto& f) { f.dimension = xenos::DataDimension::kCube; });
      if (fetch.format == xenos::TextureFormat::k_8_8_8_8) {
        reject([](auto& f) { f.mip_address = 0x1000000 >> 12; });
      } else {
        // Removing the mip page keeps the existing ordinary LOD0 admission.
        auto clamped = fetch;
        clamped.mip_address = 0;
        TextureInfo clamped_legacy;
        REQUIRE(TextureInfo::Prepare(clamped, &clamped_legacy));
        DrawCaptureFrameInputTextureView clamped_view;
        REQUIRE(cache.PrepareFrameInputTexture(clamped, clamped_view));
        CHECK(clamped_view.mip_max_level == 0);
        CHECK(clamped_view.mip_size == clamped_legacy.memory.mip_size);
      }
      DrawCaptureFrameInputTextureView signed_view;
      signed_view.is_signed = true;
      CHECK_FALSE(cache.PrepareFrameInputTexture(fetch, signed_view));
      // The ordinary complete single-level neighbor still needs no exception.
      auto ordinary = fetch;
      ordinary.packed_mips = false;
      ordinary.mip_max_level = 0;
      ordinary.mip_address = 0;
      TextureInfo ordinary_legacy;
      REQUIRE(TextureInfo::Prepare(ordinary, &ordinary_legacy));
      DrawCaptureFrameInputTextureView ordinary_view;
      REQUIRE(cache.PrepareFrameInputTexture(ordinary, ordinary_view));
      CHECK(ordinary_view.base_size == ordinary_legacy.memory.base_size);
    }
  }
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);
}

constexpr std::array<std::array<uint32_t, 6>, 3> kPreparedCubeWords{
    {{0x00824802, 0x1BB5B086, 0x1407E03F, 0x00A81414, 0x00000183, 0x1BB43E18},
     {0x00824956, 0x1BA0B060, 0x1407E03F, 0x00A80D11, 0x00000183, 0x1B9F3E18},
     {0x01000002, 0x19F39054, 0x140FE07F, 0x00000D10, 0x000001C0, 0x19EF1E00}}};

TEST_CASE("Prepared cube tuples cover all six faces without changing legacy ranges",
          "[graphics][prepared][cube]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  for (size_t form = 0; form < kPreparedCubeWords.size(); ++form) {
    CAPTURE(form);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, kPreparedCubeWords[form].data(), sizeof(fetch));
    const uint32_t base_extent = form == 1 ? 196608 : 98304;
    const uint32_t mip_extent = form == 2 ? 280192 : form == 1 ? 94208 : 94080;
    TextureInfo legacy;
    REQUIRE(TextureInfo::Prepare(fetch, &legacy));
    CHECK(legacy.memory.base_size == base_extent);
    CHECK(legacy.memory.mip_size == (form == 2 ? 196608 : 49152));
    TestTextureCache::TextureKey key;
    TestTextureCache::BindingInfoFromFetchConstant(fetch, key, nullptr);
    const auto layout = key.GetGuestLayout();
    CHECK(layout.base.level_data_extent_bytes == base_extent);
    CHECK(layout.base.array_slice_stride_bytes == (form == 1 ? 32768 : 16384));
    CHECK(layout.mips_total_extent_bytes == mip_extent);
    CHECK(layout.packed_level == (form == 2 ? 3 : 2));
    CHECK(layout.mip_offsets_bytes[form == 2 ? 3 : 2] == (form == 2 ? 196608 : 49152));
    CHECK(layout.mips[1].array_slice_stride_bytes == (form == 2 ? 16384 : 8192));
    CHECK(layout.mips[2].array_slice_stride_bytes == (form == 2 ? 16384 : 8192));
    CHECK(layout.mips[form == 2 ? 3 : 2].array_slice_data_extent_bytes == (form == 2 ? 1664
                                                                           : form    ? 4096
                                                                                     : 3968));
    CHECK(layout.mips[form == 2 ? 3 : 2].level_data_extent_bytes == (form == 2 ? 83584
                                                                     : form    ? 45056
                                                                               : 44928));
    for (uint32_t level = 0; level <= fetch.mip_max_level; ++level) {
      const auto stored = level ? std::min(level, layout.packed_level) : 0U;
      const auto& span = level ? layout.mips[stored] : layout.base;
      uint32_t x = 0, y = 0, z = 0;
      if (level >= layout.packed_level)
        REQUIRE(texture_util::GetPackedMipOffset(fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                                                 1, fetch.format, level, x, y, z));
      const auto* info = FormatInfo::Get(fetch.format);
      const auto n = std::max(1U, (fetch.size_2d.width + 1U) >> level);
      for (uint32_t face = 0; face < 6; ++face) {
        const auto end =
            (level ? layout.mip_offsets_bytes[stored] : 0) + face * span.array_slice_stride_bytes +
            (y + (n + info->block_height - 1) / info->block_height - 1) * span.row_pitch_bytes +
            (x + (n + info->block_width - 1) / info->block_width) * info->bytes_per_block();
        CHECK(end <= (level ? mip_extent : base_extent));
      }
    }
    for (bool is_signed : {false, true}) {
      DrawCaptureFrameInputTextureView view;
      view.is_signed = is_signed;
      REQUIRE(cache.PrepareFrameInputTexture(fetch, view));
      CHECK(view.required == (is_signed == (form == 1)));
      CHECK(view.swizzled_signs == (form == 1 ? 0x55 : 0));
      CHECK(view.width == (form == 2 ? 127 : 63));
      CHECK(view.height == (form == 2 ? 127 : 63));
      CHECK(view.depth == 5);
      CHECK(view.base_size == base_extent);
      CHECK(view.mip_size == mip_extent);
      CHECK(view.component_mapping == (form ? 0x688 : 0xA0A));
    }
    // Relocated pages need no allocator-specific distance or ordering.
    auto relocated = fetch;
    relocated.base_address = 0x1000;
    relocated.mip_address = 0x3000;
    DrawCaptureFrameInputTextureView view;
    REQUIRE(cache.PrepareFrameInputTexture(relocated, view));
    CHECK(view.base_address == 0x1000000);
    CHECK(view.mip_address == 0x3000000);

    auto reject = [&](const char* name, auto mutate) {
      CAPTURE(name);
      auto neighbor = fetch;
      mutate(neighbor);
      DrawCaptureFrameInputTextureView rejected;
      CHECK_FALSE(cache.PrepareFrameInputTexture(neighbor, rejected));
      CHECK_FALSE(rejected.available);
    };
    reject("fetch type", [](auto& f) { f.type = xenos::FetchConstantType::kInvalidTexture; });
    reject("1D", [](auto& f) { f.dimension = xenos::DataDimension::k1D; });
    reject("2D", [](auto& f) { f.dimension = xenos::DataDimension::k2DOrStacked; });
    reject("3D", [](auto& f) { f.dimension = xenos::DataDimension::k3D; });
    reject("stacked", [](auto& f) { f.stacked = 1; });
    for (uint32_t depth : {0U, 4U, 6U, 63U})
      reject("face count", [depth](auto& f) { f.size_2d.stack_depth = depth; });
    reject("width", [](auto& f) { --f.size_2d.width; });
    reject("height", [](auto& f) { --f.size_2d.height; });
    reject("zero pitch", [](auto& f) { f.pitch = 0; });
    reject("pitch", [](auto& f) { ++f.pitch; });
    reject("tiled", [](auto& f) { f.tiled = 1; });
    reject("unpacked", [](auto& f) { f.packed_mips = 0; });
    reject("minimum LOD", [](auto& f) { f.mip_min_level = 1; });
    reject("short chain", [](auto& f) { --f.mip_max_level; });
    reject("long chain", [](auto& f) { ++f.mip_max_level; });
    reject("exponent", [](auto& f) { f.exp_adjust = 1; });
    reject("force W", [](auto& f) { f.force_bc_w_to_max = 1; });
    reject("base page", [](auto& f) { f.base_address = 0; });
    reject("base overflow", [](auto& f) { f.base_address = 0x1FFFF; });
    reject("mip overflow", [](auto& f) { f.mip_address = 0x1FFFF; });
    reject("mip page", [](auto& f) { f.mip_address = 0; });
    reject("aliased pages", [](auto& f) { f.mip_address = f.base_address; });
    reject("other format", [form](auto& f) {
      f.format = form == 2 ? xenos::TextureFormat::k_8_8_8_8 : xenos::TextureFormat::k_DXT4_5;
    });
    reject("endian", [](auto& f) { f.endianness = xenos::Endian::kNone; });
    reject("endian unit", [form](auto& f) {
      f.endianness = form ? xenos::Endian::k8in32 : xenos::Endian::k8in16;
    });
    reject("swizzle", [](auto& f) { f.swizzle ^= 1; });
    reject("guest alpha mapping", [](auto& f) { f.swizzle = 0x60A; });
    reject("other cube mapping", [form](auto& f) { f.swizzle = form ? 0xA0A : 0x688; });
    reject("numeric format", [](auto& f) { f.num_format ^= 1; });
    for (auto sign : {xenos::TextureSign::kUnsigned, xenos::TextureSign::kSigned,
                      xenos::TextureSign::kUnsignedBiased, xenos::TextureSign::kGamma}) {
      if (sign != fetch.sign_x) {
        reject("sign", [sign](auto& f) { f.sign_x = sign; });
        reject("sign Y", [sign](auto& f) { f.sign_y = sign; });
        reject("sign Z", [sign](auto& f) { f.sign_z = sign; });
        reject("sign W", [sign](auto& f) { f.sign_w = sign; });
      }
    }
    CHECK(legacy.memory.mip_size == (form == 2 ? 196608 : 49152));
  }
  CHECK(cache.create_count == 0);
  CHECK(cache.load_count == 0);
}

#ifdef REX_TEST_DXBC_TRANSLATION
TEST_CASE("Prepared builder rejects cube and 2D instruction descriptor mismatches",
          "[graphics][prepared][cube]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  const auto no_texture_words = NoTextureUcode();
  DxbcShader empty_vertex(xenos::ShaderType::kVertex, 1, no_texture_words.data(),
                          no_texture_words.size(), std::endian::native);
  DxbcShader empty_pixel(xenos::ShaderType::kPixel, 2, no_texture_words.data(),
                         no_texture_words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  empty_vertex.AnalyzeUcode(disassembly);
  empty_pixel.AnalyzeUcode(disassembly);
  for (bool cube_instruction : {false, true}) {
    CAPTURE(cube_instruction);
    auto words = SampleUcode(false);
    if (cube_instruction) {
      for (size_t i = 3; i < words.size(); i += 3) {
        words[i + 2] =
            (words[i + 2] & ~(3U << 14)) | (uint32_t(xenos::FetchOpDimension::kCube) << 14);
      }
    }
    DxbcShader vertex(xenos::ShaderType::kVertex, 3, words.data(), words.size(),
                      std::endian::native);
    DxbcShader pixel(xenos::ShaderType::kPixel, 4, words.data(), words.size(), std::endian::native);
    vertex.AnalyzeUcode(disassembly);
    pixel.AnalyzeUcode(disassembly);
    REQUIRE(vertex.is_ucode_analyzed());
    REQUIRE(pixel.is_ucode_analyzed());
    for (size_t form = 0; form < kPreparedCubeWords.size(); ++form) {
      CAPTURE(form);
      xenos::xe_gpu_texture_fetch_t fetch{};
      if (cube_instruction) {
        fetch.type = xenos::FetchConstantType::kTexture;
        fetch.dimension = xenos::DataDimension::k2DOrStacked;
        fetch.size_2d.width = fetch.size_2d.height = 31;
        fetch.pitch = 1;
        fetch.base_address = 16;
        fetch.format = xenos::TextureFormat::k_8_8_8_8;
        fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
      } else {
        std::memcpy(&fetch, kPreparedCubeWords[form].data(), sizeof(fetch));
      }
      DrawCaptureFrameInputTextureView supported;
      REQUIRE(cache.PrepareFrameInputTexture(fetch, supported));
      for (uint32_t slot : {2U, 7U}) {
        std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6], &fetch,
                    sizeof(fetch));
      }
      for (bool pixel_stage : {false, true}) {
        CAPTURE(pixel_stage);
        FrameInputDrawBuilderInput input{registers,
                                         cache,
                                         pixel_stage ? &empty_vertex : &vertex,
                                         pixel_stage ? &pixel : &empty_pixel,
                                         pixel_stage ? &empty_vertex : &vertex,
                                         pixel_stage ? &pixel : &empty_pixel};
        uint32_t prepare_calls = 0;
        input.prepare_texture = [&](const auto& actual, auto& view) {
          ++prepare_calls;
          return cache.PrepareFrameInputTexture(actual, view);
        };
        input.sampler_value = [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
          value = 0xF880;
          return true;
        };
        FrameInputDrawBuilderResult result;
        diagnostic::DrawCaptureFrameInputDrawView draw;
        draw.index.present = false;
        CHECK_FALSE(FrameInputDrawBuilder::BuildPrepared(input, result, draw));
        CHECK_FALSE(result.texture_bindings_complete);
        CHECK(result.non_native_texture_incomplete);
        REQUIRE(result.textures.size() == 4);
        for (const auto& view : result.textures) {
          CHECK_FALSE(view.available);
          CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
        }
        CHECK(prepare_calls == 0);
        auto matching = fetch;
        if (cube_instruction) {
          std::memcpy(&matching, kPreparedCubeWords[form].data(), sizeof(matching));
        } else {
          matching.dimension = xenos::DataDimension::k2DOrStacked;
          matching.size_2d.stack_depth = 0;
          matching.packed_mips = 0;
          matching.mip_max_level = 0;
          matching.mip_address = 0;
          matching.format = xenos::TextureFormat::k_8_8_8_8;
          matching.endianness = xenos::Endian::kNone;
          matching.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
          matching.num_format = 0;
          matching.sign_x = matching.sign_y = matching.sign_z = matching.sign_w =
              xenos::TextureSign::kUnsigned;
        }
        REQUIRE(cache.PrepareFrameInputTexture(matching, supported));
        std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 7 * 6], &matching,
                    sizeof(matching));
        CHECK_FALSE(FrameInputDrawBuilder::BuildPrepared(input, result, draw));
        REQUIRE(result.textures.size() == 4);
        CHECK(prepare_calls == 2);
        for (const auto& view : result.textures) {
          CHECK(view.available == (view.fetch_constant == 7));
        }
        std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 7 * 6], &fetch,
                    sizeof(fetch));
      }
    }
  }
}

TEST_CASE("Prepared cube builder normalizes one required companion and retains CPU guards",
          "[graphics][prepared][cube]") {
  auto& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared(memory);
  RegisterFile registers;
  TestTextureCache cache(registers, shared);
  auto words = SampleUcode(false);
  for (size_t i = 3; i < words.size(); i += 3)
    words[i + 2] = (words[i + 2] & ~(3U << 14)) | (uint32_t(xenos::FetchOpDimension::kCube) << 14);
  DxbcShader vertex(xenos::ShaderType::kVertex, 1, words.data(), words.size(), std::endian::native);
  DxbcShader pixel(xenos::ShaderType::kPixel, 2, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  vertex.AnalyzeUcode(disassembly);
  pixel.AnalyzeUcode(disassembly);
  for (size_t form = 0; form < kPreparedCubeWords.size(); ++form) {
    for (uint32_t slot : {2U, 7U})
      std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6],
                  kPreparedCubeWords[form].data(), 24);
    FrameInputDrawBuilderInput input{registers, cache, &vertex, &pixel, &vertex, &pixel};
    input.dirty = [](uint32_t, uint32_t) { return false; };
    input.gpu_written = [](uint32_t, uint32_t) { return false; };
    input.readable = [](uint32_t, uint32_t) { return true; };
    input.sampler_value = [](const Shader::PreparedSamplerBinding&, uint32_t& value) {
      value = 0xF880;
      return true;
    };
    auto build = [&](FrameInputDrawBuilderResult& result) {
      diagnostic::DrawCaptureFrameInputDrawView draw;
      draw.index.present = false;
      return FrameInputDrawBuilder::BuildPrepared(input, result, draw);
    };
    FrameInputDrawBuilderResult result;
    REQUIRE(build(result));
    REQUIRE(result.textures.size() == 8);
    for (const auto& view : result.textures) {
      CHECK(view.available);
      CHECK(view.required == (view.is_signed == (form == 1)));
      CHECK(view.depth == 6);
      CHECK(view.width == (form == 2 ? 128 : 64));
      CHECK(view.height == (form == 2 ? 128 : 64));
      CHECK(view.pitch == (form == 2 ? 4 : 2));
      CHECK(view.mip_size == (form == 2 ? 280192 : form == 1 ? 94208 : 94080));
      CHECK(view.component_mapping == (form ? 0x1688 : 0x1A0A));
    }
    const uint32_t mip = registers.GetTextureFetch(2).mip_address << 12;
    const uint32_t last_byte = mip + (form == 2 ? 280192 : form == 1 ? 94208 : 94080) - 1;
    const auto includes_last = [last_byte](uint32_t start, uint32_t length) {
      return start <= last_byte && uint64_t(start) + length > last_byte;
    };
    input.dirty = includes_last;
    CHECK_FALSE(build(result));
    input.dirty = [](uint32_t, uint32_t) { return false; };
    input.gpu_written = includes_last;
    CHECK_FALSE(build(result));
    input.gpu_written = [](uint32_t, uint32_t) { return false; };
    input.readable = [includes_last](uint32_t start, uint32_t length) {
      return !includes_last(start, length);
    };
    CHECK_FALSE(build(result));
    input.readable = [](uint32_t, uint32_t) { return true; };
    input.prepare_texture = [&](const auto& fetch, auto& view) {
      if (view.is_signed != (form == 1)) {
        view.available = false;
        view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
        return false;
      }
      return cache.PrepareFrameInputTexture(fetch, view);
    };
    REQUIRE(build(result));
    for (const auto& view : result.textures)
      if (!view.required) {
        CHECK_FALSE(view.available);
        CHECK_FALSE(view.memory_version.identity);
      }
  }
}
#endif
}  // namespace
