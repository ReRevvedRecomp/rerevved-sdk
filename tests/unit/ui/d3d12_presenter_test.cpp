/**
 * @file        d3d12_presenter_test.cpp
 * @brief       Guest output FIFO and GPU copy ownership tests
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <string_view>
#include <thread>

#include <d3d12sdklayers.h>
#include <rex/cvar.h>

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

namespace rex::ui::d3d12::detail {
struct GpuGuestOutputTestAccess {
  static std::shared_ptr<GpuGuestOutputTicket> Pending() {
    return std::shared_ptr<GpuGuestOutputTicket>(new GpuGuestOutputTicket);
  }
  static void Claim(GpuGuestOutputTicket& ticket) { ticket.Claim(); }
  static void Cancel(GpuGuestOutputTicket& ticket) { ticket.Cancel(); }
  static void Fail(GpuGuestOutputTicket& ticket) { ticket.Fail(); }
  static void Submit(GpuGuestOutputTicket& ticket, ID3D12Device* device, ID3D12Fence* fence,
                     uint64_t value) {
    ticket.Claim();
    ticket.Submit(device, fence, value);
  }
  static size_t Copies(const D3D12Presenter& presenter) {
    return presenter.gpu_guest_output_copies_.size();
  }
  static bool Owns(const D3D12Presenter& presenter, ID3D12Resource* resource) {
    for (const auto& copy : presenter.gpu_guest_output_copies_) {
      if (std::get<D3D12Presenter::GpuGuestOutputFrame>(copy.pending.frame).resource.Get() ==
          resource) {
        return true;
      }
    }
    return false;
  }
  static void ClaimThenClose(D3D12Presenter& presenter,
                             const std::shared_ptr<GpuGuestOutputStream>& stream) {
    auto entry =
        presenter.guest_output_frame_queue_.Pop([](auto& frame) { frame.ticket->Claim(); });
    REQUIRE(entry);
    CHECK(entry->frame.ticket->status() == GpuGuestOutputTicket::Status::kClaimed);
    presenter.CloseGpuGuestOutputStream(stream);
    CHECK(entry->frame.ticket->status() == GpuGuestOutputTicket::Status::kClaimed);
    entry->frame.ticket->Cancel();
  }
  static void FailLastCopy(D3D12Presenter& presenter) {
    REQUIRE_FALSE(presenter.gpu_guest_output_copies_.empty());
    CHECK_FALSE(presenter.FinishGpuGuestOutputCopy(presenter.gpu_guest_output_copy_value_, E_FAIL));
  }
};
}  // namespace rex::ui::d3d12::detail

TEST_CASE("D3D12 GPU output retirement rejects incomplete and failed copies") {
  using Access = rex::ui::d3d12::detail::GpuGuestOutputTestAccess;
  using Status = rex::ui::d3d12::GpuGuestOutputTicket::Status;
  auto canceled = Access::Pending();
  CHECK_FALSE(canceled->AwaitRetirement(0));
  CHECK_FALSE(canceled->AwaitRetirement(INFINITE));
  CHECK(canceled->status() == Status::kPending);
  Access::Claim(*canceled);
  CHECK_FALSE(canceled->AwaitRetirement(0));
  Access::Cancel(*canceled);
  CHECK(canceled->AwaitRetirement(0));
  CHECK(canceled->status() == Status::kCanceled);
  auto failed = Access::Pending();
  Access::Claim(*failed);
  Access::Fail(*failed);
  CHECK_FALSE(failed->AwaitRetirement(0));
  CHECK(failed->status() == Status::kFailed);
  CHECK(failed->copy_fence_value() == 0);
}

namespace {
using Microsoft::WRL::ComPtr;
using rex::ui::d3d12::D3D12Presenter;
using rex::ui::d3d12::D3D12Provider;

struct TestGpuOwners {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12Fence> fence;
  uint64_t value;
  std::vector<ComPtr<ID3D12DeviceChild>> objects;
  ~TestGpuOwners() {
    const auto completed = fence->GetCompletedValue();
    if (SUCCEEDED(device->GetDeviceRemovedReason()) &&
        (completed == UINT64_MAX || completed < value)) {
      for (auto& object : objects) {
        (void)object.Detach();
      }
      (void)fence.Detach();
      (void)device.Detach();
    }
  }
};

void AwaitTestFence(ID3D12Fence* fence, uint64_t value) {
  REQUIRE(fence->GetCompletedValue() != UINT64_MAX);
  HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  REQUIRE(event);
  REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(value, event)));
  const DWORD waited = WaitForSingleObject(event, 5000);
  ComPtr<ID3D12Device> device;
  REQUIRE(SUCCEEDED(fence->GetDevice(IID_PPV_ARGS(&device))));
  const auto completed = fence->GetCompletedValue();
  if (FAILED(device->GetDeviceRemovedReason()) || (completed != UINT64_MAX && completed >= value)) {
    CloseHandle(event);
  }
  REQUIRE(waited == WAIT_OBJECT_0);
  REQUIRE(fence->GetCompletedValue() != UINT64_MAX);
  REQUIRE(fence->GetCompletedValue() >= value);
}

ComPtr<ID3D12Resource> TestBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = type;
  ComPtr<ID3D12Resource> resource;
  REQUIRE(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    type == D3D12_HEAP_TYPE_UPLOAD
                                                        ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                        : D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&resource))));
  return resource;
}

void TestBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &barrier);
}

D3D12Presenter::GpuGuestOutputFrame MakeGpuFrame(const D3D12Provider& provider,
                                                 const std::array<uint32_t, 6>& pixels) {
  auto* device = provider.GetDevice();
  D3D12Presenter::GpuGuestOutputFrame frame;
  frame.width = 3;
  frame.height = 2;
  frame.ready_value = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = frame.width;
  desc.Height = frame.height;
  desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Format = D3D12Presenter::kGuestOutputFormat;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  REQUIRE(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&frame.resource))));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  REQUIRE(footprint.Footprint.RowPitch == 256);
  auto upload = TestBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD);
  void* mapped = nullptr;
  D3D12_RANGE no_read{0, 0};
  REQUIRE(SUCCEEDED(upload->Map(0, &no_read, &mapped)));
  for (uint32_t row = 0; row < 2; ++row) {
    std::memcpy(
        static_cast<uint8_t*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch,
        pixels.data() + row * 3, 3 * sizeof(uint32_t));
  }
  upload->Unmap(0, nullptr);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12CommandQueue> producer;
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  REQUIRE(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&producer))));
  REQUIRE(producer.Get() != provider.GetDirectQueue());
  REQUIRE(SUCCEEDED(
      device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
  REQUIRE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                              nullptr, IID_PPV_ARGS(&list))));
  D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
  source.pResource = upload.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = footprint;
  destination.pResource = frame.resource.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  TestBarrier(list.Get(), frame.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
  REQUIRE(SUCCEEDED(list->Close()));
  REQUIRE(
      SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frame.ready_fence))));
  ID3D12CommandList* execute = list.Get();
  TestGpuOwners owners{device, frame.ready_fence, frame.ready_value};
  const std::array<ID3D12DeviceChild*, 5> submitted_objects{
      frame.resource.Get(), upload.Get(), allocator.Get(), list.Get(), producer.Get()};
  for (auto* object : submitted_objects) {
    owners.objects.emplace_back(object);
  }
  producer->ExecuteCommandLists(1, &execute);
  REQUIRE(SUCCEEDED(producer->Signal(frame.ready_fence.Get(), frame.ready_value)));
  AwaitTestFence(frame.ready_fence.Get(), frame.ready_value);
  return frame;
}

std::vector<uint32_t> ReadGpuFrame(const D3D12Provider& provider, ID3D12Resource* texture) {
  auto* device = provider.GetDevice();
  const auto desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = TestBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  REQUIRE(SUCCEEDED(
      device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
  REQUIRE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                              nullptr, IID_PPV_ARGS(&list))));
  REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
  TestBarrier(list.Get(), texture, D3D12Presenter::kGuestOutputInternalState,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
  source.pResource = texture;
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  destination.pResource = readback.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint = footprint;
  list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  TestBarrier(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE,
              D3D12Presenter::kGuestOutputInternalState);
  REQUIRE(SUCCEEDED(list->Close()));
  ID3D12CommandList* execute = list.Get();
  TestGpuOwners owners{device, fence, 1};
  const std::array<ID3D12DeviceChild*, 5> submitted_objects{
      texture, readback.Get(), allocator.Get(), list.Get(), provider.GetDirectQueue()};
  for (auto* object : submitted_objects) {
    owners.objects.emplace_back(object);
  }
  provider.GetDirectQueue()->ExecuteCommandLists(1, &execute);
  REQUIRE(SUCCEEDED(provider.GetDirectQueue()->Signal(fence.Get(), 1)));
  AwaitTestFence(fence.Get(), 1);
  void* mapped = nullptr;
  D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
  REQUIRE(SUCCEEDED(readback->Map(0, &range, &mapped)));
  std::vector<uint32_t> pixels(desc.Width * desc.Height);
  for (uint32_t row = 0; row < desc.Height; ++row) {
    std::memcpy(
        pixels.data() + row * desc.Width,
        static_cast<const uint8_t*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch,
        desc.Width * sizeof(uint32_t));
  }
  D3D12_RANGE no_write{0, 0};
  readback->Unmap(0, &no_write);
  return pixels;
}
}  // namespace

TEST_CASE("D3D12 GPU output shares bounds and preserves copy ownership") {
  if (const auto* run = std::getenv("REX_RUN_D3D12_GPU_TESTS");
      !run || std::string_view(run) != "1") {
    SKIP("set REX_RUN_D3D12_GPU_TESTS=1 to run the D3D12 output ownership check");
  }
  REQUIRE(rex::cvar::SetFlagByName("d3d12_debug", "true"));
  auto provider = D3D12Provider::Create();
  REQUIRE(provider);
  ComPtr<ID3D12InfoQueue> debug;
  REQUIRE(SUCCEEDED(provider->GetDevice()->QueryInterface(IID_PPV_ARGS(&debug))));
  auto presenter = D3D12Presenter::Create([](bool, bool) {}, *provider);
  REQUIRE(presenter);
  using Access = rex::ui::d3d12::detail::GpuGuestOutputTestAccess;
  using Status = rex::ui::d3d12::GpuGuestOutputTicket::Status;
  const std::array<uint32_t, 6> pixels = {PackRgb10(1, 257, 700, 2),    PackRgb10(1023, 17, 3, 1),
                                          PackRgb10(512, 1023, 256, 0), PackRgb10(9, 700, 1022, 3),
                                          PackRgb10(1000, 2, 500, 1),   PackRgb10(300, 800, 7, 2)};
  auto frame = MakeGpuFrame(*provider, pixels);
  auto stream = presenter->CreateGpuGuestOutputStream();
  auto first = presenter->QueueGpuGuestOutputFrame(stream, frame);
  REQUIRE(first);
  const auto cpu_token = presenter->QueueCpuGuestOutputFrame(MakeFrame(3, 2, 0x20));
  REQUIRE(cpu_token);
  auto third = presenter->QueueGpuGuestOutputFrame(stream, frame);
  REQUIRE(third);
  CHECK(first->token() < cpu_token);
  CHECK(cpu_token < third->token());
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(stream, frame));
  CHECK(presenter->QueueCpuGuestOutputFrame(MakeFrame(3, 2, 0x30)) == 0);
  presenter->CloseGpuGuestOutputStream(stream);
  CHECK(first->status() == Status::kCanceled);
  CHECK(third->status() == Status::kCanceled);
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(stream, frame));
  presenter->ClearCpuGuestOutputFrames();
  stream = presenter->CreateGpuGuestOutputStream();
  auto claimed = presenter->QueueGpuGuestOutputFrame(stream, frame);
  REQUIRE(claimed);
  Access::ClaimThenClose(*presenter, stream);
  CHECK(claimed->status() == Status::kCanceled);

  stream = presenter->CreateGpuGuestOutputStream();
  auto canceled = presenter->QueueGpuGuestOutputFrame(stream, frame);
  REQUIRE(canceled);
  std::latch entered(1), closed(1);
  std::thread closer([&] {
    entered.wait();
    presenter->CloseGpuGuestOutputStream(stream);
    closed.count_down();
  });
  const bool refreshed = presenter->RefreshGuestOutput(3, 2, 3, 2, [&](auto&) {
    entered.count_down();
    closed.wait();
    return true;
  });
  closer.join();
  REQUIRE(refreshed);
  CHECK(canceled->status() == Status::kCanceled);
  CHECK(Access::Copies(*presenter) == 0);

  stream = presenter->CreateGpuGuestOutputStream();
  auto future = frame;
  future.ready_value = 2;
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(stream, future));
  auto wrong_extent = frame;
  wrong_extent.width = 4;
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(stream, wrong_extent));
  auto wrong_layout = frame;
  wrong_layout.resource = TestBuffer(provider->GetDevice(), 256, D3D12_HEAP_TYPE_READBACK);
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(stream, wrong_layout));
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> other_device;
  REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
  REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
  REQUIRE(SUCCEEDED(
      D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other_device))));
  auto wrong_device = frame;
  REQUIRE(SUCCEEDED(other_device->CreateFence(1, D3D12_FENCE_FLAG_NONE,
                                              IID_PPV_ARGS(&wrong_device.ready_fence))));
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(stream, wrong_device));

  auto incomplete = Access::Pending();
  ComPtr<ID3D12Fence> incomplete_fence;
  REQUIRE(SUCCEEDED(
      other_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&incomplete_fence))));
  Access::Submit(*incomplete, other_device.Get(), incomplete_fence.Get(), 1);
  CHECK_FALSE(incomplete->AwaitRetirement(1));
  CHECK(incomplete->status() == Status::kSubmitted);
  REQUIRE(SUCCEEDED(incomplete_fence->Signal(1)));
  CHECK(incomplete->AwaitRetirement(0));
  CHECK(incomplete->status() == Status::kCompleted);

  auto removed = Access::Pending();
  Access::Submit(*removed, other_device.Get(), incomplete_fence.Get(), 2);
  ComPtr<ID3D12Device5> removable;
  REQUIRE(SUCCEEDED(other_device.As(&removable)));
  removable->RemoveDevice();
  CHECK(FAILED(other_device->GetDeviceRemovedReason()));
  CHECK_FALSE(removed->AwaitRetirement(1));
  CHECK(removed->status() == Status::kFailed);

  auto oversized = frame;
  oversized.width = 8193;
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(stream, oversized));
  auto zero_ready = frame;
  zero_ready.ready_value = 0;
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(stream, zero_ready));
  zero_ready.ready_value = UINT64_MAX;
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(stream, zero_ready));

  for (uint32_t iteration = 0; iteration < 4; ++iteration) {
    auto expected = pixels;
    expected[0] ^= iteration;
    frame = MakeGpuFrame(*provider, expected);
    auto ticket = presenter->QueueGpuGuestOutputFrame(stream, frame);
    REQUIRE(ticket);
    presenter->ClearCpuGuestOutputFrames();
    CHECK(ticket->status() == Status::kPending);
    ComPtr<ID3D12Resource> destination;
    REQUIRE(presenter->RefreshGuestOutput(
        3, 2, 3, 2, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
          CHECK(ticket->status() == Status::kPending);
          destination = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
                            .resource_uav_capable();
          context.SetIs8bpc(true);
          return true;
        }));
    REQUIRE(ticket->AwaitRetirement(5000));
    CHECK(ticket->status() == Status::kCompleted);
    CHECK(ticket->copy_fence_value() != 0);
    CHECK(ReadGpuFrame(*provider, destination.Get()) ==
          std::vector<uint32_t>(expected.begin(), expected.end()));
  }
  std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket> self_copy;
  REQUIRE(presenter->RefreshGuestOutput(
      3, 2, 3, 2, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
        auto same = frame;
        same.resource = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
                            .resource_uav_capable();
        self_copy = presenter->QueueGpuGuestOutputFrame(stream, std::move(same));
        return true;
      }));
  REQUIRE(self_copy);
  CHECK(self_copy->status() == Status::kCanceled);
  CHECK(Access::Copies(*presenter) == 0);

  auto failed = presenter->QueueGpuGuestOutputFrame(stream, frame);
  REQUIRE(failed);
  REQUIRE(presenter->RefreshGuestOutput(3, 2, 3, 2, [](auto&) { return true; }));
  auto* retained_source = frame.resource.Get();
  Access::FailLastCopy(*presenter);
  CHECK_FALSE(failed->AwaitRetirement(0));
  CHECK(Access::Copies(*presenter) == 1);
  frame.resource.Reset();
  CHECK(Access::Owns(*presenter, retained_source));
  presenter->CloseGpuGuestOutputStream(stream);
  CHECK(Access::Copies(*presenter) == 1);
  CHECK_FALSE(presenter->QueueGpuGuestOutputFrame(presenter->CreateGpuGuestOutputStream(),
                                                  MakeGpuFrame(*provider, pixels)));
  bool original_called = false;
  REQUIRE(presenter->RefreshGuestOutput(3, 2, 3, 2, [&](auto&) {
    original_called = true;
    return true;
  }));
  CHECK(original_called);
  CHECK(Access::Copies(*presenter) == 1);
  CHECK(presenter->GetGuestOutputFrameStats().frame_count == 0);
  presenter.reset();
  for (UINT64 index = 0; index < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
    SIZE_T size = 0;
    REQUIRE(SUCCEEDED(debug->GetMessage(index, nullptr, &size)));
    std::vector<uint8_t> bytes(size);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
    REQUIRE(SUCCEEDED(debug->GetMessage(index, message, &size)));
    CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR);
    CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION);
    CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_WARNING);
  }
}

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

TEST_CASE("D3D12 guest output FPS counts distinct presented tokens") {
  using Counter = rex::ui::d3d12::detail::GuestOutputFrameStats;
  Counter counter;
  const Counter::Clock::time_point start{};
  counter.Record(1, S_OK, start);
  for (uint64_t frame = 1; frame <= 10; ++frame) {
    const auto now = start + std::chrono::milliseconds(100 * frame);
    counter.Record(frame + 1, S_OK, now);
    for (int repeat = 1; repeat < 6; ++repeat) {
      counter.Record(frame + 1, S_OK, now + std::chrono::milliseconds(repeat * 10));
    }
    counter.Record(1, S_OK, now + std::chrono::milliseconds(60));
  }
  const auto stats = counter.Snapshot(start + std::chrono::milliseconds(1070));
  CHECK(stats.frame_count == 11);
  CHECK(stats.fps == 10.0);
  CHECK(stats.frame_time_ms == 100.0);
}

TEST_CASE("D3D12 guest output FPS excludes failure occlusion and stale output") {
  using Counter = rex::ui::d3d12::detail::GuestOutputFrameStats;
  Counter counter;
  const Counter::Clock::time_point start{};
  counter.Record(1, S_OK, start);
  counter.Record(2, E_FAIL, start + std::chrono::milliseconds(500));
  counter.Record(2, DXGI_STATUS_OCCLUDED, start + std::chrono::milliseconds(900));
  CHECK(counter.Snapshot(start + std::chrono::milliseconds(950)).frame_count == 1);
  counter.Record(2, S_OK, start + std::chrono::seconds(1));
  auto stats = counter.Snapshot(start + std::chrono::milliseconds(1500));
  CHECK(stats.frame_count == 2);
  CHECK(stats.fps == 1.0);
  CHECK(stats.frame_time_ms == 1000.0);
  stats = counter.Snapshot(start + std::chrono::seconds(2));
  CHECK(stats.frame_count == 2);
  CHECK(stats.fps == 0);
  CHECK(stats.frame_time_ms == 0);
  counter.Record(0, E_FAIL, start + std::chrono::seconds(3));
  CHECK(counter.Snapshot(start + std::chrono::seconds(3)).frame_count == 2);
  counter.Record(0, S_OK, start + std::chrono::seconds(3));
  CHECK(counter.Snapshot(start + std::chrono::seconds(3)).frame_count == 0);
  counter.Record(1, S_OK, start + std::chrono::seconds(4));
  CHECK(counter.Snapshot(start + std::chrono::seconds(4)).frame_count == 0);
  counter.Record(3, S_OK, start + std::chrono::seconds(4));
  counter.Record(4, S_OK, start + std::chrono::seconds(5));
  CHECK(counter.Snapshot(start + std::chrono::seconds(5)).fps == 1.0);
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
