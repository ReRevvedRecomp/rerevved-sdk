/**
 * @file        d3d12_presenter_test.cpp
 * @brief       CPU guest output FIFO contract tests
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>

namespace {

using CpuGuestOutputFrame = rex::ui::d3d12::D3D12Presenter::CpuGuestOutputFrame;
using CpuGuestOutputQueue = rex::ui::d3d12::detail::CpuGuestOutputFrameQueue<
    CpuGuestOutputFrame, rex::ui::d3d12::D3D12Presenter::kMaxCpuGuestOutputFrames>;

CpuGuestOutputFrame MakeFrame(uint32_t width, uint32_t height, uint8_t seed) {
  CpuGuestOutputFrame frame;
  frame.width = width;
  frame.height = height;
  frame.row_pitch = width * 4;
  frame.pixels.resize(size_t(frame.row_pitch) * height);
  for (size_t i = 0; i < frame.pixels.size(); ++i) {
    frame.pixels[i] = static_cast<uint8_t>(seed + i);
  }
  return frame;
}

bool IsValid(const CpuGuestOutputFrame& frame) {
  return rex::ui::d3d12::detail::IsCpuGuestOutputFrameValid(
      frame, rex::ui::d3d12::D3D12Presenter::kMaxCpuGuestOutputDimension,
      rex::ui::d3d12::D3D12Presenter::kMaxCpuGuestOutputPayloadBytes);
}

uint32_t PackRgb10(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  return r | (g << 10) | (b << 20) | (a << 30);
}

uint32_t ConvertRgb10ToRgba8(uint32_t rgb10) {
  const auto convert = [](uint32_t channel) {
    return static_cast<uint32_t>(float(channel) * (255.0f / 1023.0f) + 0.5f);
  };
  return convert(rgb10 & 0x3FF) | (convert((rgb10 >> 10) & 0x3FF) << 8) |
         (convert((rgb10 >> 20) & 0x3FF) << 16) | (uint32_t(0xFF) << 24);
}

}  // namespace

TEST_CASE("D3D12 CPU guest output validates tight complete payloads") {
  CpuGuestOutputFrame valid = MakeFrame(3, 2, 0x10);
  CHECK(IsValid(valid));

  CpuGuestOutputFrame wrong_pitch = valid;
  wrong_pitch.row_pitch += 4;
  CHECK_FALSE(IsValid(wrong_pitch));

  CpuGuestOutputFrame truncated = valid;
  truncated.pixels.pop_back();
  CHECK_FALSE(IsValid(truncated));

  CpuGuestOutputFrame zero_extent = valid;
  zero_extent.width = 0;
  CHECK_FALSE(IsValid(zero_extent));

  CpuGuestOutputFrame oversized =
      MakeFrame(rex::ui::d3d12::D3D12Presenter::kMaxCpuGuestOutputDimension + 1, 1, 0x20);
  CHECK_FALSE(IsValid(oversized));
}

TEST_CASE("D3D12 CPU guest output FIFO preserves ownership and bounds") {
  CpuGuestOutputQueue queue;
  CpuGuestOutputFrame first = MakeFrame(2, 1, 0x01);
  CpuGuestOutputFrame second = MakeFrame(2, 1, 0x11);
  CpuGuestOutputFrame third = MakeFrame(2, 1, 0x21);
  CpuGuestOutputFrame rejected = MakeFrame(2, 1, 0x31);

  const uint64_t first_token = queue.Push(std::move(first));
  const uint64_t second_token = queue.Push(std::move(second));
  const uint64_t third_token = queue.Push(std::move(third));
  CHECK(first_token != 0);
  CHECK(second_token != 0);
  CHECK(third_token != 0);
  CHECK(first_token != second_token);
  CHECK(first_token != third_token);
  CHECK(second_token != third_token);
  CHECK(queue.Push(std::move(rejected)) == 0);

  auto first_entry = queue.Pop();
  REQUIRE(first_entry);
  CHECK(first_entry->token == first_token);
  CHECK(first_entry->frame.pixels.front() == 0x01);

  auto second_entry = queue.Pop();
  REQUIRE(second_entry);
  CHECK(second_entry->token == second_token);
  CHECK(second_entry->frame.pixels.front() == 0x11);

  auto third_entry = queue.Pop();
  REQUIRE(third_entry);
  CHECK(third_entry->token == third_token);
  CHECK(third_entry->frame.pixels.front() == 0x21);
  CHECK_FALSE(queue.Pop());
}

TEST_CASE("D3D12 CPU guest output FIFO clear drops pending frames") {
  CpuGuestOutputQueue queue;
  queue.Push(MakeFrame(1, 1, 0x40));
  queue.Push(MakeFrame(1, 1, 0x50));
  queue.Clear();
  CHECK_FALSE(queue.Pop());

  const uint64_t token_after_clear = queue.Push(MakeFrame(1, 1, 0x60));
  CHECK(token_after_clear != 0);
  auto entry_after_clear = queue.Pop();
  REQUIRE(entry_after_clear);
  CHECK(entry_after_clear->token == token_after_clear);
  CHECK(entry_after_clear->frame.pixels.front() == 0x60);
}

TEST_CASE("D3D12 CPU guest output upload preserves padded RGB10A2 rows") {
  const char* run_gpu_tests = std::getenv("REX_RUN_D3D12_GPU_TESTS");
  if (!run_gpu_tests || std::string_view(run_gpu_tests) != "1") {
    SKIP("set REX_RUN_D3D12_GPU_TESTS=1 to run the native D3D12 upload check");
  }
  if (!rex::ui::d3d12::D3D12Provider::IsD3D12APIAvailable()) {
    SKIP("D3D12 is unavailable on this host");
  }

  auto provider = rex::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  auto presenter = rex::ui::d3d12::D3D12Presenter::Create([](bool, bool) {}, *provider);
  REQUIRE(presenter);

  constexpr uint32_t kWidth = 3;
  constexpr uint32_t kHeight = 2;
  const std::array<uint32_t, kWidth * kHeight> expected_rgb10 = {
      PackRgb10(1, 257, 700, 2),  PackRgb10(1023, 17, 3, 1),  PackRgb10(512, 1023, 256, 0),
      PackRgb10(9, 700, 1022, 3), PackRgb10(1000, 2, 500, 1), PackRgb10(300, 800, 7, 2)};
  CpuGuestOutputFrame frame;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.row_pitch = kWidth * sizeof(uint32_t);
  frame.pixels.resize(size_t(frame.row_pitch) * kHeight);
  std::memcpy(frame.pixels.data(), expected_rgb10.data(), frame.pixels.size());
  const uint64_t token = presenter->QueueCpuGuestOutputFrame(std::move(frame));
  REQUIRE(token != 0);

  bool original_refresher_called = false;
  REQUIRE(
      presenter->RefreshGuestOutput(kWidth, kHeight, kWidth, kHeight,
                                    [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
                                      original_refresher_called = true;
                                      context.SetIs8bpc(true);
                                      return true;
                                    }));
  CHECK(original_refresher_called);

  rex::ui::RawImage captured;
  REQUIRE(presenter->CaptureGuestOutput(captured));
  REQUIRE(captured.width == kWidth);
  REQUIRE(captured.height == kHeight);
  REQUIRE(captured.stride == kWidth * sizeof(uint32_t));
  REQUIRE(captured.data.size() == size_t(captured.stride) * kHeight);
  for (size_t i = 0; i < expected_rgb10.size(); ++i) {
    uint32_t captured_rgba8 = 0;
    std::memcpy(&captured_rgba8, captured.data.data() + i * sizeof(uint32_t),
                sizeof(captured_rgba8));
    CHECK(captured_rgba8 == ConvertRgb10ToRgba8(expected_rgb10[i]));
  }
}
