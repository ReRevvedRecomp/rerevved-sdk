#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

namespace {
using namespace rex::graphics;

// One tfetch2D from fetch constant 0, using the named field positions in
// ucode.h.
std::vector<uint32_t> TextureFetchUcode() {
  return {1 | (1 << 12) | (1 << 16),
          uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12,
          0,
          uint32_t(ucode::FetchOpcode::kTextureFetch) | (1 << 19) | (0x24u << 26),
          0x688u | (uint32_t(xenos::TextureFilter::kUseFetchConst) << 12) |
              (uint32_t(xenos::TextureFilter::kUseFetchConst) << 14) |
              (uint32_t(xenos::TextureFilter::kUseFetchConst) << 16) |
              (uint32_t(xenos::AnisoFilter::kUseFetchConst) << 18) | (3u << 24) | (3u << 26) |
              (1u << 28),
          uint32_t(xenos::FetchOpDimension::k2D) << 14};
}

struct SpirvInstruction {
  uint32_t opcode;
  std::vector<uint32_t> operands;
};

}  // namespace

// The fetch constant's exp_adjust is bits 13:18 of word 3. Word 4 bits 12:21
// are the LOD bias, so reading the exponent from word 4 scales textures with a
// LOD bias towards black.
TEST_CASE("SPIR-V texture fetch takes the result exponent from fetch word 3", "[graphics][spirv]") {
  const auto words = TextureFetchUcode();
  Shader shader(xenos::ShaderType::kPixel, 1, words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  REQUIRE(shader.is_ucode_analyzed());
  REQUIRE(shader.texture_bindings().size() == 1);

  SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true), false, false, false);
  Shader::Translation* translation = shader.GetOrCreateTranslation(
      translator.GetDefaultPixelShaderModification(shader.GetDynamicAddressableRegisterCount(0)));
  REQUIRE(translation != nullptr);
  REQUIRE(translator.TranslateAnalyzedShader(*translation));
  const auto& binary = translation->translated_binary();
  REQUIRE(binary.size() % sizeof(uint32_t) == 0);
  std::vector<uint32_t> spirv(binary.size() / sizeof(uint32_t));
  std::memcpy(spirv.data(), binary.data(), binary.size());
  REQUIRE(spirv.size() > 5);

  constexpr uint32_t kOpConstant = 43;
  constexpr uint32_t kOpLoad = 61;
  constexpr uint32_t kOpAccessChain = 65;
  constexpr uint32_t kOpBitcast = 124;
  constexpr uint32_t kOpBitFieldSExtract = 202;
  std::unordered_map<uint32_t, uint32_t> constants;
  std::unordered_map<uint32_t, SpirvInstruction> results;
  std::vector<SpirvInstruction> extracts;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t word_count = spirv[i] >> 16;
    REQUIRE(word_count != 0);
    REQUIRE(i + word_count <= spirv.size());
    SpirvInstruction instruction{spirv[i] & 0xFFFF,
                                 {spirv.begin() + i + 1, spirv.begin() + i + word_count}};
    if (instruction.opcode == kOpConstant && instruction.operands.size() == 3) {
      constants[instruction.operands[1]] = instruction.operands[2];
    }
    if ((instruction.opcode == kOpLoad || instruction.opcode == kOpAccessChain ||
         instruction.opcode == kOpBitcast) &&
        instruction.operands.size() >= 3) {
      results[instruction.operands[1]] = instruction;
    }
    if (instruction.opcode == kOpBitFieldSExtract && instruction.operands.size() == 5) {
      extracts.push_back(instruction);
    }
    i += word_count;
  }

  auto constant = [&](uint32_t id) {
    auto it = constants.find(id);
    return it == constants.end() ? UINT32_MAX : it->second;
  };
  // Operands: result type, result, base, offset, count.
  std::vector<std::vector<uint32_t>> exponent_sources;
  for (const SpirvInstruction& extract : extracts) {
    if (constant(extract.operands[3]) != 13 || constant(extract.operands[4]) != 6) {
      continue;
    }
    auto bitcast = results.find(extract.operands[2]);
    REQUIRE(bitcast != results.end());
    REQUIRE(bitcast->second.opcode == kOpBitcast);
    auto load = results.find(bitcast->second.operands[2]);
    REQUIRE(load != results.end());
    REQUIRE(load->second.opcode == kOpLoad);
    auto chain = results.find(load->second.operands[2]);
    REQUIRE(chain != results.end());
    REQUIRE(chain->second.opcode == kOpAccessChain);
    std::vector<uint32_t> indices;
    for (size_t j = 3; j < chain->second.operands.size(); ++j) {
      indices.push_back(constant(chain->second.operands[j]));
    }
    exponent_sources.push_back(indices);
  }

  // Fetch constant 0 word 3 is uvec4 element 0, component 3.
  REQUIRE(exponent_sources.size() == 1);
  CAPTURE(exponent_sources[0]);
  CHECK(exponent_sources[0] == std::vector<uint32_t>{0, 0, 3});
}
