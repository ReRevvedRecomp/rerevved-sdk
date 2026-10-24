#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/xenos.h>
#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/interfaces/video_delegate.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "kernel/xboxkrnl/video_routing.h"
#include "test_memory.h"

namespace rex::kernel::xboxkrnl {
namespace {

struct RecordedCall {
  int method;
  uint32_t a;
  uint32_t b;
  bool operator==(const RecordedCall&) const = default;
};

enum : int { kInterrupt = 1, kRing = 2, kWriteBack = 3 };

class FakeGraphics final : public system::IGraphicsSystem {
 public:
  X_STATUS SetupPresentation(ui::WindowedAppContext*) override { return X_STATUS_SUCCESS; }
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher*, system::KernelState*) override {
    return X_STATUS_SUCCESS;
  }
  bool has_presentation() const override { return false; }
  void Shutdown() override {}

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    calls.push_back({kInterrupt, callback, user_data});
  }
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    calls.push_back({kRing, ptr, size_log2});
  }
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    calls.push_back({kWriteBack, ptr, block_size_log2});
  }

  std::vector<RecordedCall> calls;
};

class FakeDelegate final : public system::IDetachedVideoDelegate {
 public:
  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    calls.push_back({kInterrupt, callback, user_data});
  }
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    calls.push_back({kRing, ptr, size_log2});
  }
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    calls.push_back({kWriteBack, ptr, block_size_log2});
  }
  void OnSwap(const system::DetachedVideoSwap& swap) override { swaps.push_back(swap); }

  std::vector<RecordedCall> calls;
  std::vector<system::DetachedVideoSwap> swaps;
};

void RouteAll(system::IDetachedVideoDelegate* video_delegate, system::IGraphicsSystem* graphics,
              std::array<std::atomic<bool>, 3>& warned, std::array<VideoExportRoute, 3>& routes) {
  routes[0] =
      RouteSetInterruptCallback(video_delegate, graphics, warned[0], 0x82001234, 0x40005678);
  routes[1] = RouteInitializeRingBuffer(video_delegate, graphics, warned[1], 0x1F400000, 15);
  routes[2] = RouteEnableReadPointerWriteBack(video_delegate, graphics, warned[2], 0x1F500040, 6);
}

const std::vector<RecordedCall> kExpectedCalls = {
    {kInterrupt, 0x82001234, 0x40005678},
    {kRing, 0x1F400000, 15},
    {kWriteBack, 0x1F500040, 6},
};

TEST_CASE("Vd registration without delegate or graphics warns once", "[kernel][video]") {
  std::array<std::atomic<bool>, 3> warned{};
  std::array<VideoExportRoute, 3> routes{};

  RouteAll(nullptr, nullptr, warned, routes);
  for (size_t i = 0; i < routes.size(); ++i) {
    CHECK(routes[i] == VideoExportRoute::kNoGpuWarned);
    CHECK(warned[i].load());
  }

  RouteAll(nullptr, nullptr, warned, routes);
  for (auto route : routes) {
    CHECK(route == VideoExportRoute::kNoGpuIgnored);
  }
}

TEST_CASE("Vd registration forwards unchanged to graphics without a delegate", "[kernel][video]") {
  FakeGraphics graphics;
  std::array<std::atomic<bool>, 3> warned{};
  std::array<VideoExportRoute, 3> routes{};

  RouteAll(nullptr, &graphics, warned, routes);
  for (size_t i = 0; i < routes.size(); ++i) {
    CHECK(routes[i] == VideoExportRoute::kGraphics);
    CHECK_FALSE(warned[i].load());
  }
  CHECK(graphics.calls == kExpectedCalls);
}

TEST_CASE("Vd registration reaches the delegate first with exact arguments", "[kernel][video]") {
  FakeDelegate video_delegate;
  FakeGraphics graphics;
  std::array<std::atomic<bool>, 3> warned{};
  std::array<VideoExportRoute, 3> routes{};

  RouteAll(&video_delegate, &graphics, warned, routes);
  for (size_t i = 0; i < routes.size(); ++i) {
    CHECK(routes[i] == VideoExportRoute::kDelegate);
    CHECK_FALSE(warned[i].load());
  }
  CHECK(video_delegate.calls == kExpectedCalls);
  CHECK(graphics.calls.empty());
  CHECK(video_delegate.swaps.empty());
}

constexpr uint32_t kFrontbufferVirtual = 0xA1230000;
constexpr uint32_t kFrontbufferPhysical = 0x01230000;
constexpr uint32_t kReservation = 0x1F400100;
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kTextureFormat = 6;  // k_8_8_8_8

struct SwapFixture {
  std::array<rex::be<uint32_t>, 6> fetch{};
  rex::be<uint32_t> frontbuffer;
  rex::be<uint32_t> texture_format = kTextureFormat;
  rex::be<uint32_t> color_space = 0u;
  rex::be<uint32_t> width = kWidth;
  rex::be<uint32_t> height = kHeight;
  std::array<uint8_t, 256> reservation{};
  std::array<uint32_t, 6> fetch_host{};

  explicit SwapFixture(uint32_t frontbuffer_virtual) {
    graphics::xenos::xe_gpu_texture_fetch_t gpu_fetch{};
    gpu_fetch.dword_0 = 0x00000002;
    gpu_fetch.dword_3 = 0x00000D10;
    gpu_fetch.dword_5 = 0x00000200;
    gpu_fetch.format = graphics::xenos::TextureFormat::k_8_8_8_8;
    gpu_fetch.base_address = frontbuffer_virtual >> 12;
    gpu_fetch.size_2d.width = kWidth - 1;
    gpu_fetch.size_2d.height = kHeight - 1;
    std::memcpy(fetch_host.data(), &gpu_fetch, sizeof(fetch_host));
    for (size_t i = 0; i < fetch.size(); ++i) {
      fetch[i] = fetch_host[i];
    }
    frontbuffer = frontbuffer_virtual;
    reservation.fill(0xCD);
  }

  VdSwapInputs Inputs() {
    return {reservation.data(), kReservation, fetch.data(), &frontbuffer,
            &texture_format,    &color_space, &width,       &height};
  }
};

std::array<uint8_t, 256> ExpectedPacket(const std::array<uint32_t, 6>& fetch) {
  std::array<uint32_t, 64> dwords{};
  dwords[0] = 0x00054800;  // Type 0, SHADER_CONSTANT_FETCH_00_0, six registers.
  for (size_t i = 0; i < 6; ++i) {
    dwords[1 + i] = fetch[i];
  }
  dwords[7] = 0xC0036400;  // Type 3 PM4_XE_SWAP, four words.
  dwords[8] = 0x53574150;  // 'SWAP'
  dwords[9] = kFrontbufferPhysical;
  dwords[10] = kWidth;
  dwords[11] = kHeight;
  for (size_t i = 12; i < dwords.size(); ++i) {
    dwords[i] = 0x80000000;  // Type 2 NOP.
  }
  std::array<uint8_t, 256> bytes{};
  for (size_t i = 0; i < dwords.size(); ++i) {
    bytes[i * 4 + 0] = uint8_t(dwords[i] >> 24);
    bytes[i * 4 + 1] = uint8_t(dwords[i] >> 16);
    bytes[i * 4 + 2] = uint8_t(dwords[i] >> 8);
    bytes[i * 4 + 3] = uint8_t(dwords[i]);
  }
  return bytes;
}

TEST_CASE("VdSwap writes identical ring bytes and then notifies the delegate", "[kernel][video]") {
  auto& memory = rex::testing::GetTestMemory();
  REQUIRE(memory.GetPhysicalAddress(kFrontbufferVirtual) == kFrontbufferPhysical);

  SwapFixture without_delegate(kFrontbufferVirtual);
  REQUIRE(ProcessVdSwap(memory, nullptr, without_delegate.Inputs()));

  FakeDelegate video_delegate;
  SwapFixture with_delegate(kFrontbufferVirtual);
  REQUIRE(ProcessVdSwap(memory, &video_delegate, with_delegate.Inputs()));

  std::array<uint32_t, 6> physical_fetch = without_delegate.fetch_host;
  physical_fetch[1] = (physical_fetch[1] & 0x00000FFF) | kFrontbufferPhysical;
  CHECK(without_delegate.reservation == ExpectedPacket(physical_fetch));
  CHECK(with_delegate.reservation == without_delegate.reservation);

  REQUIRE(video_delegate.swaps.size() == 1);
  const auto& swap = video_delegate.swaps.front();
  CHECK(swap.reservation == kReservation);
  CHECK(swap.frontbuffer_virtual == kFrontbufferVirtual);
  CHECK(swap.frontbuffer_physical == kFrontbufferPhysical);
  for (size_t i = 0; i < physical_fetch.size(); ++i) {
    CHECK(swap.fetch[i] == physical_fetch[i]);
  }
  CHECK(swap.texture_format == kTextureFormat);
  CHECK(swap.color_space == 0);
  CHECK(swap.width == kWidth);
  CHECK(swap.height == kHeight);
  CHECK(video_delegate.calls.empty());
}

TEST_CASE("VdSwap carries a pending presentation aspect in the swap packet", "[kernel][video]") {
  auto& memory = rex::testing::GetTestMemory();

  system::SwapPresentationAspect presentation;
  presentation.aspect_x = 21;
  presentation.aspect_y = 9;
  presentation.policy_sequence = 0x100000002ull;
  presentation.override_valid = true;

  SwapFixture plain(kFrontbufferVirtual);
  REQUIRE(ProcessVdSwap(memory, nullptr, plain.Inputs()));
  SwapFixture carrying(kFrontbufferVirtual);
  auto inputs = carrying.Inputs();
  inputs.presentation = &presentation;
  REQUIRE(ProcessVdSwap(memory, nullptr, inputs));

  auto word_at = [&](size_t word) {
    const auto& bytes = carrying.reservation;
    return (uint32_t(bytes[word * 4]) << 24) | (uint32_t(bytes[word * 4 + 1]) << 16) |
           (uint32_t(bytes[word * 4 + 2]) << 8) | uint32_t(bytes[word * 4 + 3]);
  };
  CHECK(word_at(7) == 0xC0086400);  // Type 3 PM4_XE_SWAP, nine words.
  CHECK(word_at(8) == 0x53574150);  // 'SWAP'
  CHECK(word_at(9) == kFrontbufferPhysical);
  CHECK(word_at(10) == kWidth);
  CHECK(word_at(11) == kHeight);
  CHECK(word_at(12) == 0x41535058);  // 'ASPX'
  CHECK(word_at(13) == 21);
  CHECK(word_at(14) == 9);
  CHECK(word_at(15) == 2);
  CHECK(word_at(16) == 1);
  for (size_t word = 17; word < 64; ++word) {
    CHECK(word_at(word) == 0x80000000);
  }
  // The fetch registers precede the packet and do not depend on the trailer.
  CHECK(std::equal(plain.reservation.begin(), plain.reservation.begin() + 7 * 4,
                   carrying.reservation.begin()));
}

TEST_CASE("VdSwap encodes explicit clears and omits the trailer without a request",
          "[kernel][video]") {
  auto& memory = rex::testing::GetTestMemory();

  system::SwapPresentationAspect clear;
  clear.aspect_x = 21;
  clear.aspect_y = 9;
  clear.policy_sequence = 7;
  clear.override_valid = false;

  SwapFixture cleared(kFrontbufferVirtual);
  auto inputs = cleared.Inputs();
  inputs.presentation = &clear;
  REQUIRE(ProcessVdSwap(memory, nullptr, inputs));
  const auto word_at = [&](size_t word) {
    return (uint32_t(cleared.reservation[word * 4]) << 24) |
           (uint32_t(cleared.reservation[word * 4 + 1]) << 16) |
           (uint32_t(cleared.reservation[word * 4 + 2]) << 8) |
           uint32_t(cleared.reservation[word * 4 + 3]);
  };
  CHECK(word_at(12) == 0x41535058);
  CHECK(word_at(13) == 0);
  CHECK(word_at(14) == 0);
  CHECK(word_at(15) == 7);

  // A request without a sequence is no request.
  system::SwapPresentationAspect none;
  SwapFixture plain(kFrontbufferVirtual);
  SwapFixture without(kFrontbufferVirtual);
  auto none_inputs = without.Inputs();
  none_inputs.presentation = &none;
  REQUIRE(ProcessVdSwap(memory, nullptr, plain.Inputs()));
  REQUIRE(ProcessVdSwap(memory, nullptr, none_inputs));
  CHECK(without.reservation == plain.reservation);
}

// Debug builds assert on an unmapped frontbuffer before the safety return.
#ifdef NDEBUG
constexpr uint32_t kUnmappedFrontbuffer = 0x40010000;

TEST_CASE("VdSwap with an unmapped frontbuffer writes nothing and skips OnSwap",
          "[kernel][video]") {
  auto& memory = rex::testing::GetTestMemory();
  REQUIRE(memory.GetPhysicalAddress(kUnmappedFrontbuffer) == UINT32_MAX);

  FakeDelegate video_delegate;
  SwapFixture fixture(kUnmappedFrontbuffer);
  const auto untouched = fixture.reservation;
  CHECK_FALSE(ProcessVdSwap(memory, &video_delegate, fixture.Inputs()));
  CHECK(fixture.reservation == untouched);
  CHECK(video_delegate.swaps.empty());
}
#endif

TEST_CASE("Runtime detached-video delegate accepts, rejects replacement and clears",
          "[kernel][video]") {
  rex::Runtime runtime(std::filesystem::temp_directory_path() / "rex_video_delegate_test");
  CHECK(runtime.detached_video_delegate() == nullptr);

  auto first = std::make_shared<FakeDelegate>();
  auto second = std::make_shared<FakeDelegate>();
  CHECK(runtime.SetDetachedVideoDelegate(first) == X_STATUS_SUCCESS);
  CHECK(runtime.detached_video_delegate() == first);

  CHECK(runtime.SetDetachedVideoDelegate(second) == X_STATUS_OBJECT_NAME_COLLISION);
  CHECK(runtime.detached_video_delegate() == first);
  CHECK(second.use_count() == 1);

  CHECK(runtime.SetDetachedVideoDelegate(nullptr) == X_STATUS_SUCCESS);
  CHECK(runtime.detached_video_delegate() == nullptr);
  CHECK(first.use_count() == 1);

  CHECK(runtime.SetDetachedVideoDelegate(second) == X_STATUS_SUCCESS);
  CHECK(runtime.detached_video_delegate() == second);
}

TEST_CASE("Runtime setup rejects an early delegate in tool mode and clears it", "[kernel][video]") {
  rex::Runtime runtime(std::filesystem::path{});
  auto video_delegate = std::make_shared<FakeDelegate>();
  REQUIRE(runtime.SetDetachedVideoDelegate(video_delegate) == X_STATUS_SUCCESS);

  // The failed Setup runs Shutdown, which must drop the Runtime's reference.
  CHECK(runtime.Setup(RuntimeConfig{.kernel_init = rex::kernel::InitializeKernel,
                                    .tool_mode = true}) == X_STATUS_NOT_SUPPORTED);
  CHECK(runtime.detached_video_delegate() == nullptr);
  CHECK(video_delegate.use_count() == 1);
}

TEST_CASE("Detached-video delegate is rejected with graphics or in tool mode", "[kernel][video]") {
  CHECK(CheckDetachedVideoDelegateChange(false, false, false, true) == X_STATUS_SUCCESS);
  CHECK(CheckDetachedVideoDelegateChange(false, true, false, true) == X_STATUS_NOT_SUPPORTED);
  CHECK(CheckDetachedVideoDelegateChange(true, false, false, true) == X_STATUS_NOT_SUPPORTED);
  CHECK(CheckDetachedVideoDelegateChange(false, false, true, true) ==
        X_STATUS_OBJECT_NAME_COLLISION);
  // Clearing is always allowed.
  CHECK(CheckDetachedVideoDelegateChange(true, true, true, false) == X_STATUS_SUCCESS);
}

}  // namespace
}  // namespace rex::kernel::xboxkrnl
