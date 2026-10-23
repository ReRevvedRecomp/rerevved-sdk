#include <array>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using rex::graphics::CommandProcessor;
using rex::graphics::CommandProcessorCpuContext;

class ConsumerCommandProcessor final : public CommandProcessor {
 public:
  explicit ConsumerCommandProcessor(CommandProcessorCpuContext context)
      : CommandProcessor(nullptr, context) {}

  using CommandProcessor::ExecutePacket;

  std::array<uint32_t, 3> swap = {};

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType, uint32_t, const uint32_t*,
                                    uint32_t) override {
    return nullptr;
  }
  bool IssueDraw(rex::graphics::xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override {
    return false;
  }
  bool IssueCopy() override { return false; }
  void IssueSwap(uint32_t pointer, uint32_t width, uint32_t height) override {
    swap = {pointer, width, height};
  }
};

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

bool IsXenosModuleLoaded() {
#if REX_PLATFORM_WIN32
  return GetModuleHandleW(L"rexgpu-xenos.dll") != nullptr ||
         GetModuleHandleW(L"rexgpu-xenosd.dll") != nullptr ||
         GetModuleHandleW(L"rexgpu-xenosrd.dll") != nullptr;
#else
  return false;
#endif
}

std::array<uint32_t, 9> SwapPacket(uint32_t pointer, uint32_t width, uint32_t height) {
  std::array<uint32_t, 9> packet = {
      rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_XE_SWAP, 7),
      rex::graphics::xenos::kSwapSignature,
      pointer,
      width,
      height,
      0xAA,
      0xBB,
      0xCC,
      rex::graphics::xenos::MakePacketType2(),
  };
  for (auto& word : packet) {
    word = rex::byte_swap(word);
  }
  return packet;
}

std::array<uint32_t, 3> InterruptPacket(uint32_t cpu_mask) {
  std::array<uint32_t, 3> packet = {
      rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_INTERRUPT, 1),
      cpu_mask,
      rex::graphics::xenos::MakePacketType2(),
  };
  for (auto& word : packet) {
    word = rex::byte_swap(word);
  }
  return packet;
}

struct InterruptRecorder {
  std::vector<std::pair<uint32_t, uint32_t>> calls;
};

bool RecordInterrupt(void* owner, uint32_t source, uint32_t cpu) {
  static_cast<InterruptRecorder*>(owner)->calls.emplace_back(source, cpu);
  return true;
}

}  // namespace

int main() {
  int failures = 0;
  rex::graphics::RegisterFile register_file;

  CommandProcessorCpuContext context;
  context.register_file = &register_file;
  ConsumerCommandProcessor processor(context);
  const auto* register_info =
      rex::graphics::RegisterFile::GetRegisterInfo(rex::graphics::XE_GPU_REG_RB_COLOR_INFO);
  failures +=
      !Check(register_info && register_info->name, "runtime register metadata was not available");
  const auto color_index = rex::graphics::reg::RB_COLOR_INFO::rt_register_indices[0];
  const auto blend_index = rex::graphics::reg::RB_BLENDCONTROL::rt_register_indices[0];
  failures += !Check(color_index == rex::graphics::XE_GPU_REG_RB_COLOR_INFO &&
                         blend_index == rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0,
                     "runtime render-target register indices were not available");
  failures +=
      !Check(!IsXenosModuleLoaded(), "CPU consumer loaded the xenos plugin before decoding");
  failures += !Check(REXCVAR_GET(vsync), "runtime cvar registry did not expose vsync");
  processor.SetDesiredSwapPostEffect(CommandProcessor::SwapPostEffect::kFxaa);
  failures +=
      !Check(processor.GetDesiredSwapPostEffect() == CommandProcessor::SwapPostEffect::kFxaa,
             "desired swap post effect was not retained");
  processor.SetDesiredSwapPostEffect(CommandProcessor::SwapPostEffect::kFxaaExtreme);
  failures +=
      !Check(processor.GetDesiredSwapPostEffect() == CommandProcessor::SwapPostEffect::kFxaaExtreme,
             "desired swap post effect change was not retained");

  auto swap_packet = SwapPacket(0x12340000, 1280, 720);
  rex::memory::RingBuffer swap_reader(reinterpret_cast<uint8_t*>(swap_packet.data()),
                                      sizeof(swap_packet));
  failures += !Check(processor.ExecutePacket(&swap_reader), "swap packet did not decode");
  failures += !Check(processor.swap == std::array<uint32_t, 3>{0x12340000, 1280, 720},
                     "swap output values were not forwarded");
  failures += !Check(processor.counter() == 1, "swap packet did not advance the decoder counter");
  failures += !Check(swap_reader.read_offset() == 8 * sizeof(uint32_t),
                     "swap packet consumed the wrong number of words");
  failures += !Check(!IsXenosModuleLoaded(), "CPU consumer loaded the xenos plugin while decoding");

  auto changed_swap_packet = SwapPacket(0x56780000, 640, 360);
  rex::memory::RingBuffer changed_swap_reader(
      reinterpret_cast<uint8_t*>(changed_swap_packet.data()), sizeof(changed_swap_packet));
  failures +=
      !Check(processor.ExecutePacket(&changed_swap_reader), "changed swap packet did not decode");
  failures += !Check(processor.swap == std::array<uint32_t, 3>{0x56780000, 640, 360},
                     "changed swap output values were not forwarded");
  failures +=
      !Check(processor.counter() == 2, "changed swap packet did not advance the decoder counter");

  auto missing_interrupt = InterruptPacket((1u << 0) | (1u << 2));
  rex::memory::RingBuffer missing_reader(reinterpret_cast<uint8_t*>(missing_interrupt.data()),
                                         sizeof(missing_interrupt));
  failures +=
      !Check(!processor.ExecutePacket(&missing_reader), "missing interrupt owner was accepted");
  failures += !Check(missing_reader.read_offset() == 2 * sizeof(uint32_t),
                     "missing interrupt did not consume its mask");
  failures += !Check(missing_reader.read_count() == sizeof(uint32_t),
                     "missing interrupt consumed its trailing packet");

  InterruptRecorder recorder;
  context.interrupt_owner = &recorder;
  context.dispatch_interrupt = &RecordInterrupt;
  ConsumerCommandProcessor bridged_processor(context);
  auto bridged_interrupt = InterruptPacket((1u << 0) | (1u << 2) | (1u << 5));
  rex::memory::RingBuffer bridged_reader(reinterpret_cast<uint8_t*>(bridged_interrupt.data()),
                                         sizeof(bridged_interrupt));
  failures += !Check(bridged_processor.ExecutePacket(&bridged_reader),
                     "interrupt bridge rejected a valid packet");
  failures +=
      !Check(recorder.calls == std::vector<std::pair<uint32_t, uint32_t>>{{1, 0}, {1, 2}, {1, 5}},
             "interrupt bridge changed source or CPU order");
  failures += !Check(bridged_reader.read_offset() == 2 * sizeof(uint32_t),
                     "interrupt bridge consumed its trailing packet");
  failures +=
      !Check(bridged_processor.ExecutePacket(&bridged_reader), "trailing packet did not decode");
  failures +=
      !Check(bridged_reader.read_offset() == 0, "trailing packet did not advance the reader");

  return failures == 0 ? 0 : 1;
}
