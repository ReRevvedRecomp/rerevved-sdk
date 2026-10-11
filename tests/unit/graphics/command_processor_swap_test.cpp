#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/xenos_fence_trace.h>
#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include "graphics/frame_input_swap_view.h"

namespace rex::graphics {
namespace {

class CpuGraphicsSystem final : public GraphicsSystem {
 public:
  std::string name() const override { return "CPU swap test"; }
  void BindRuntime(Runtime& runtime) {
    memory_ = runtime.memory();
    function_dispatcher_ = runtime.function_dispatcher();
    kernel_state_ = runtime.kernel_state();
  }

 protected:
  void CreateProvider(bool) override {}
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override { return {}; }
};

CommandProcessorCpuContext CpuContext(GraphicsSystem* system);

class CpuCommandProcessor : public CommandProcessor {
 public:
  explicit CpuCommandProcessor(GraphicsSystem* system)
      : CommandProcessor(system, CpuContext(system)) {}
  CpuCommandProcessor(GraphicsSystem* system, CommandProcessorCpuContext context)
      : CommandProcessor(system, context) {}
  using CommandProcessor::ExecutePacket;
  using CommandProcessor::WriteRegister;
  uint32_t issue_count = 0;
  std::array<uint32_t, 3> issued = {};
  std::function<void()> on_issue;

  diagnostic::DrawCaptureFrameInputSwapView Build(FrameInputSwapView& storage, uint64_t id,
                                                  uint32_t pointer, uint32_t width,
                                                  uint32_t height) {
    return storage.Build(*register_file_, gamma_ramp_256_entry_table(), gamma_ramp_pwl_rgb(),
                         GetActualSwapPostEffect(), id, 7, 3, pointer, width, height);
  }
  void SetPostEffect(SwapPostEffect effect) { swap_post_effect_actual_ = effect; }
  const auto* Table() const { return gamma_ramp_256_entry_table(); }
  const auto* Pwl() const { return gamma_ramp_pwl_rgb(); }

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  Shader* LoadShader(xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override {
    return nullptr;
  }
  bool IssueDraw(xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override { return false; }
  bool IssueCopy() override { return false; }
  void IssueSwap(uint32_t pointer, uint32_t width, uint32_t height) override {
    ++issue_count;
    issued = {pointer, width, height};
    if (on_issue) {
      on_issue();
    }
  }
};

class FailingSetupCommandProcessor final : public CpuCommandProcessor {
 public:
  FailingSetupCommandProcessor(GraphicsSystem* system, CommandProcessorCpuContext context)
      : CpuCommandProcessor(system, context) {}

 protected:
  bool SetupContext() override { return false; }
};

class BlockingSwapCommandProcessor final : public CpuCommandProcessor {
 public:
  BlockingSwapCommandProcessor(GraphicsSystem* system, CommandProcessorCpuContext context)
      : CpuCommandProcessor(system, context) {}

  std::atomic<bool> issue_entered{false};
  std::atomic<bool> release_issue{false};

 protected:
  void IssueSwap(uint32_t, uint32_t, uint32_t) override {
    issue_entered.store(true, std::memory_order_release);
    while (!release_issue.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ++issue_count;
  }
};

class ShutdownGuard final {
 public:
  explicit ShutdownGuard(std::function<void()> shutdown) : shutdown_(std::move(shutdown)) {}
  ~ShutdownGuard() { shutdown_(); }

 private:
  std::function<void()> shutdown_;
};

CommandProcessorCpuContext CpuContext(GraphicsSystem* system) {
  return {
      .memory = system->memory(),
      .kernel_state = system->kernel_state(),
      .register_file = system->register_file(),
  };
}

class DecodedCommandProcessor final : public CpuCommandProcessor {
 public:
  using CpuCommandProcessor::CpuCommandProcessor;
  std::function<void(uint32_t, uint32_t, uint32_t)> on_decoded;

 protected:
  void OnSwapDecoded(uint32_t pointer, uint32_t width, uint32_t height) override {
    if (on_decoded) {
      on_decoded(pointer, width, height);
    }
  }
};

class PreparedObserver final : public diagnostic::DrawCaptureFrameInputObserver {
 public:
  bool OnFrameBegin(const diagnostic::DrawCaptureFrameInputSwapView&) override { return true; }
  bool OnDraw(const diagnostic::DrawCaptureFrameInputDrawView&) override { return true; }
  bool OnCopy(const diagnostic::DrawCaptureFrameInputCopyView&) override { return true; }
  bool OnSwap(const diagnostic::DrawCaptureFrameInputSwapView&) override { return true; }
  bool OnFrameEnd(uint64_t, uint64_t, bool) override { return true; }
  bool WantsPreparedSwaps() const noexcept override { return true; }
  bool OnSwapPrepared(const diagnostic::DrawCaptureFrameInputSwapView&) override {
    if (throwing) {
      throw std::runtime_error("prepared terminal failure");
    }
    return false;
  }
  bool throwing = false;
};

std::array<uint32_t, 9> SwapPacket(bool predicate = false) {
  // Four decoded values and three padding words followed by a separate packet.
  std::array<uint32_t, 9> packet = {xenos::MakePacketType3(xenos::PM4_XE_SWAP, 7, predicate),
                                    xenos::kSwapSignature,
                                    0x12340000,
                                    1280,
                                    720,
                                    0xAA,
                                    0xBB,
                                    0xCC,
                                    xenos::MakePacketType2()};
  for (auto& word : packet) {
    word = rex::byte_swap(word);
  }
  return packet;
}

std::array<uint32_t, 3> InterruptPacket(uint32_t cpu_mask) {
  std::array<uint32_t, 3> packet = {
      xenos::MakePacketType3(xenos::PM4_INTERRUPT, 1),
      cpu_mask,
      xenos::MakePacketType2(),
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

struct AdmissionRecorder {
  uint32_t packet = 0;
  uint32_t count = 0;
  uint32_t read_offset = 0;
};

bool RejectPacket(void* owner, const memory::RingBuffer* reader, uint32_t packet, uint32_t count) {
  auto* recorder = static_cast<AdmissionRecorder*>(owner);
  recorder->packet = packet;
  recorder->count = count;
  recorder->read_offset = reader->read_offset();
  return false;
}

TEST_CASE("shared interrupt decoder requires an owner after consuming the mask",
          "[graphics][cpu]") {
  CpuGraphicsSystem system;
  CpuCommandProcessor cp(&system);
  auto packet = InterruptPacket((1u << 0) | (1u << 2));
  memory::RingBuffer reader(reinterpret_cast<uint8_t*>(packet.data()), sizeof(packet));
  REQUIRE_FALSE(cp.ExecutePacket(&reader));
  CHECK(reader.read_offset() == 2 * sizeof(uint32_t));
  CHECK(reader.read_count() == sizeof(uint32_t));
}

TEST_CASE("shared interrupt decoder preserves CPU order through the legacy bridge",
          "[graphics][cpu]") {
  CpuGraphicsSystem system;
  InterruptRecorder recorder;
  auto context = CpuContext(&system);
  context.interrupt_owner = &recorder;
  context.dispatch_interrupt = &RecordInterrupt;
  CpuCommandProcessor cp(&system, context);
  auto packet = InterruptPacket((1u << 0) | (1u << 2) | (1u << 5));
  memory::RingBuffer reader(reinterpret_cast<uint8_t*>(packet.data()), sizeof(packet));
  REQUIRE(cp.ExecutePacket(&reader));
  CHECK(recorder.calls == std::vector<std::pair<uint32_t, uint32_t>>{{1, 0}, {1, 2}, {1, 5}});
  CHECK(reader.read_offset() == 2 * sizeof(uint32_t));
  REQUIRE(cp.ExecutePacket(&reader));
  CHECK(reader.read_offset() == 0);
}

TEST_CASE("CPU packet admission rejects before consume and predication", "[graphics][cpu]") {
  CpuGraphicsSystem system;
  AdmissionRecorder recorder;
  auto context = CpuContext(&system);
  context.packet_admission_owner = &recorder;
  context.admit_packet = &RejectPacket;
  CpuCommandProcessor cp(&system, context);

  std::array<uint32_t, 3> packet = {
      xenos::MakePacketType3(xenos::PM4_DRAW_INDX, 1, true),
      0x01020304,
      0,
  };
  for (auto& word : packet) {
    word = rex::byte_swap(word);
  }
  memory::RingBuffer reader(reinterpret_cast<uint8_t*>(packet.data()), sizeof(packet));
  reader.set_write_offset(2 * sizeof(uint32_t));

  REQUIRE_FALSE(cp.ExecutePacket(&reader));
  CHECK(recorder.packet == xenos::MakePacketType3(xenos::PM4_DRAW_INDX, 1, true));
  CHECK(recorder.count == 1);
  CHECK(recorder.read_offset == 0);
  CHECK(reader.read_offset() == 0);
  CHECK(reader.read_count() == 2 * sizeof(uint32_t));
}

TEST_CASE("CPU packet admission rejects truncated packets at their start", "[graphics][cpu]") {
  CpuGraphicsSystem system;
  AdmissionRecorder recorder;
  auto context = CpuContext(&system);
  context.packet_admission_owner = &recorder;
  context.admit_packet = &RejectPacket;
  CpuCommandProcessor cp(&system, context);

  std::array<uint32_t, 4> packet = {
      xenos::MakePacketType3(xenos::PM4_MEM_WRITE, 3),
      0x00100000,
      0xAABBCCDD,
      0,
  };
  for (auto& word : packet) {
    word = rex::byte_swap(word);
  }
  memory::RingBuffer reader(reinterpret_cast<uint8_t*>(packet.data()), sizeof(packet));
  reader.set_write_offset(3 * sizeof(uint32_t));

  REQUIRE_FALSE(cp.ExecutePacket(&reader));
  CHECK(recorder.packet == xenos::MakePacketType3(xenos::PM4_MEM_WRITE, 3));
  CHECK(recorder.count == 3);
  CHECK(recorder.read_offset == 0);
  CHECK(reader.read_offset() == 0);
}

TEST_CASE("CPU worker reports setup failure and shuts down cleanly", "[graphics][cpu]") {
  Runtime runtime(std::filesystem::path{});
  REQUIRE(runtime.Setup(RuntimeConfig{
              .kernel_init = rex::kernel::InitializeKernel,
              .tool_mode = true,
          }) == X_STATUS_SUCCESS);
  CpuGraphicsSystem system;
  system.BindRuntime(runtime);
  AdmissionRecorder recorder;
  auto context = CpuContext(&system);
  context.packet_admission_owner = &recorder;
  context.admit_packet = &RejectPacket;
  FailingSetupCommandProcessor cp(&system, context);

  if (!cp.Initialize()) {
    cp.Shutdown();
    FAIL("CPU worker initialization must report thread creation success");
  }
  ShutdownGuard shutdown_guard([&] { cp.Shutdown(); });
  for (uint32_t i = 0; i < 1000 && !cp.worker_failed(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(cp.worker_failed());
  CHECK_FALSE(cp.worker_running());
  cp.Shutdown();
  cp.Shutdown();
}

TEST_CASE("CPU invalid publication waits for an in-flight range", "[graphics][cpu]") {
  Runtime runtime(std::filesystem::path{});
  REQUIRE(runtime.Setup(RuntimeConfig{
              .kernel_init = rex::kernel::InitializeKernel,
              .tool_mode = true,
          }) == X_STATUS_SUCCESS);
  CpuGraphicsSystem system;
  system.BindRuntime(runtime);

  AdmissionRecorder recorder;
  auto context = CpuContext(&system);
  context.packet_admission_owner = &recorder;
  context.admit_packet = [](void*, const memory::RingBuffer*, uint32_t, uint32_t) { return true; };
  BlockingSwapCommandProcessor cp(&system, context);

  constexpr uint32_t kRingPointer = 0x00100000;
  constexpr uint32_t kRingSizeLog2 = 3;
  constexpr uint32_t kWritebackPointer = 0x00200000;
  constexpr uint32_t kWritebackBlockSizeLog2 = 2;
  cp.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
  cp.EnableReadPointerWriteBack(kWritebackPointer, kWritebackBlockSizeLog2);
  if (!cp.Initialize()) {
    cp.Shutdown();
    FAIL("CPU worker initialization must report thread creation success");
  }
  ShutdownGuard shutdown_guard([&] {
    cp.release_issue.store(true, std::memory_order_release);
    cp.Shutdown();
  });

  auto packet = SwapPacket();
  std::memcpy(runtime.memory()->TranslatePhysical(kRingPointer), packet.data(),
              8 * sizeof(uint32_t));
  auto readback = [&] {
    uint32_t raw = 0;
    std::memcpy(&raw, runtime.memory()->TranslatePhysical(kWritebackPointer), sizeof(raw));
    return rex::byte_swap(raw);
  };

  const uint64_t sequence = cp.progress_sequence();
  REQUIRE(cp.PublishWritePointer(8));
  for (uint32_t i = 0; i < 1000 && !cp.issue_entered.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(cp.issue_entered.load(std::memory_order_acquire));

  CHECK_FALSE(cp.worker_failed());
  CHECK(cp.worker_running());
  CHECK(cp.read_ptr_index() == 0);
  CHECK(readback() == 0);
  CHECK_FALSE(cp.PublishWritePointer(16));
  CHECK_FALSE(cp.PublishWritePointer(8));
  CHECK_FALSE(cp.worker_failed());
  CHECK(cp.worker_running());
  CHECK(cp.progress_sequence() == sequence);
  CHECK(cp.read_ptr_index() == 0);
  CHECK(readback() == 0);

  cp.release_issue.store(true, std::memory_order_release);
  for (uint32_t i = 0; i < 1000 && !(cp.worker_failed() && cp.progress_sequence() > sequence);
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(cp.worker_failed());
  CHECK_FALSE(cp.worker_running());
  CHECK(cp.progress_sequence() > sequence);
  CHECK(cp.read_ptr_index() == 8);
  CHECK(readback() == 8);
  CHECK(cp.issue_count == 1);
  cp.Shutdown();
  cp.Shutdown();
}

TEST_CASE("shared swap decoder preserves decoded order and packet progress",
          "[graphics][prepared]") {
  REQUIRE_FALSE(diagnostic::GetXenosFenceTrace().enabled());
  CpuGraphicsSystem system;
  DecodedCommandProcessor cp(&system);
  auto packet = SwapPacket();
  memory::RingBuffer reader(reinterpret_cast<uint8_t*>(packet.data()), sizeof(packet));
  std::vector<int> order;
  cp.on_decoded = [&](uint32_t pointer, uint32_t width, uint32_t height) {
    order.push_back(1);
    CHECK(pointer == 0x12340000);
    CHECK(width == 1280);
    CHECK(height == 720);
    CHECK(reader.read_offset() == 8 * sizeof(uint32_t));
    CHECK(cp.counter() == 0);
  };
  cp.on_issue = [&] { order.push_back(2); };
  REQUIRE(cp.ExecutePacket(&reader));
  CHECK(order == std::vector<int>{1, 2});
  CHECK(cp.issued == std::array<uint32_t, 3>{0x12340000, 1280, 720});
  CHECK(cp.counter() == 1);
  CHECK(reader.read_offset() == 8 * sizeof(uint32_t));
  REQUIRE(cp.ExecutePacket(&reader));
  CHECK(reader.read_offset() == 0);
  CHECK(cp.issue_count == 1);
  CHECK(cp.counter() == 1);
}

TEST_CASE("shared swap decoder skips predicated swaps and defaults to original issue",
          "[graphics][prepared]") {
  REQUIRE_FALSE(diagnostic::GetXenosFenceTrace().enabled());
  CpuGraphicsSystem system;
  DecodedCommandProcessor prepared(&system);
  uint32_t decoded_count = 0;
  prepared.on_decoded = [&](uint32_t, uint32_t, uint32_t) { ++decoded_count; };
  auto packet = SwapPacket(true);
  memory::RingBuffer reader(reinterpret_cast<uint8_t*>(packet.data()), sizeof(packet));
  REQUIRE(prepared.ExecutePacket(&reader));
  CHECK(reader.read_offset() == 8 * sizeof(uint32_t));
  CHECK(decoded_count == 0);
  CHECK(prepared.issue_count == 0);
  CHECK(prepared.counter() == 0);

  CpuCommandProcessor legacy(&system);
  packet = SwapPacket();
  reader.set_read_offset(0);
  REQUIRE(legacy.ExecutePacket(&reader));
  CHECK(legacy.issue_count == 1);
  CHECK(legacy.counter() == 1);
  CHECK(reader.read_offset() == 8 * sizeof(uint32_t));
}

TEST_CASE("prepared callback failure leaves real swap decode and original issue progressing",
          "[graphics][prepared]") {
  REQUIRE_FALSE(diagnostic::GetXenosFenceTrace().enabled());
  for (bool throwing : {false, true}) {
    CpuGraphicsSystem system;
    DecodedCommandProcessor cp(&system);
    diagnostic::DrawCaptureMailbox mailbox;
    auto observer = std::make_shared<PreparedObserver>();
    observer->throwing = throwing;
    const auto token = mailbox.RegisterFrameInputObserver(observer);
    REQUIRE(token);
    cp.on_decoded = [&](uint32_t pointer, uint32_t width, uint32_t height) {
      FrameInputSwapView storage;
      const auto view = cp.Build(storage, 1, pointer, width, height);
      CHECK_FALSE(mailbox.DispatchFrameInputPreparedSwap(token, view));
    };
    auto packet = SwapPacket();
    memory::RingBuffer reader(reinterpret_cast<uint8_t*>(packet.data()), sizeof(packet));
    REQUIRE(cp.ExecutePacket(&reader));
    CHECK_FALSE(mailbox.HasFrameInputObserver());
    CHECK(cp.issue_count == 1);
    CHECK(cp.counter() == 1);
    CHECK(reader.read_offset() == 8 * sizeof(uint32_t));
  }
}

TEST_CASE("CPU swap builder copies the real gamma owner and derives requested policy",
          "[graphics][prepared]") {
  CpuGraphicsSystem system;
  CpuCommandProcessor cp(&system);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_WRITE_EN_MASK, 7);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX, 37);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_30_COLOR, 0x32145678);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_RW_MODE, 1);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX, 12);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_PWL_DATA, 0x12345678);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_PWL_DATA, 0x23456789);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_PWL_DATA, 0x3456789A);
  auto fetch = system.register_file()->GetTextureFetch(0);
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  std::memcpy(system.register_file()->values + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, &fetch,
              sizeof(fetch));
  FrameInputSwapView storage;
  auto view = cp.Build(storage, 19, 0x12340000, 1280, 720);
  CHECK(view.id == 19);
  CHECK(view.frame == 7);
  CHECK(view.submission == 3);
  CHECK_FALSE(view.original_succeeded);
  CHECK_FALSE(view.use_pwl_gamma_ramp);
  CHECK_FALSE(view.use_fxaa);
  CHECK(view.is_8bpc);
  REQUIRE(view.gamma_256.size() == 256);
  REQUIRE(view.gamma_pwl_rgb.size() == 384);
  for (size_t i = 0; i < 256; ++i) {
    CHECK(view.gamma_256[i] == cp.Table()[i].value);
  }
  for (size_t i = 0; i < 384; ++i) {
    CHECK(view.gamma_pwl_rgb[i] == cp.Pwl()[i].value);
  }
  CHECK(view.gamma_256[37] == 0x32145678);
  CHECK(view.gamma_pwl_rgb[12 * 3 + 2] != 0);
  const std::vector<uint32_t> retained_registers(view.registers.begin(), view.registers.end());
  const auto retained_gamma = storage.gamma_256;
  const auto retained_pwl = storage.gamma_pwl_rgb;
  cp.WriteRegister(XE_GPU_REG_DC_LUT_RW_MODE, 0);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX, 37);
  cp.WriteRegister(XE_GPU_REG_DC_LUT_30_COLOR, 0);
  CHECK(view.gamma_256[37] == retained_gamma[37]);
  CHECK(cp.Table()[37].value == 0);

  cp.SetPostEffect(CommandProcessor::SwapPostEffect::kFxaa);
  view = cp.Build(storage, 20, 0x12340000, 1280, 720);
  CHECK(view.use_fxaa);
  CHECK_FALSE(view.use_pwl_gamma_ramp);
  CHECK_FALSE(view.is_8bpc);
  cp.SetPostEffect(CommandProcessor::SwapPostEffect::kNone);

  fetch.format = xenos::TextureFormat::k_2_10_10_10;
  std::memcpy(system.register_file()->values + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, &fetch,
              sizeof(fetch));
  view = cp.Build(storage, 20, 0x12340000, 1280, 720);
  CHECK(view.use_pwl_gamma_ramp);
  CHECK_FALSE(view.is_8bpc);
  fetch.format = xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
  std::memcpy(system.register_file()->values + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, &fetch,
              sizeof(fetch));
  view = cp.Build(storage, 21, 0x12340000, 1280, 720);
  CHECK(view.use_pwl_gamma_ramp);
  cp.SetPostEffect(CommandProcessor::SwapPostEffect::kFxaaExtreme);
  view = cp.Build(storage, 21, 0x12340000, 1280, 720);
  CHECK(view.use_fxaa);
  CHECK(retained_registers != std::vector<uint32_t>(view.registers.begin(), view.registers.end()));
  CHECK(retained_gamma[37] == 0x32145678);
  CHECK(retained_pwl[12 * 3 + 2] != 0);
}

}  // namespace
}  // namespace rex::graphics
