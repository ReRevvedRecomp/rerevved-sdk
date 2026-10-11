#include <catch2/catch_test_macros.hpp>

#include "graphics/frame_input_buffer_versions.h"
#include "test_memory.h"

namespace {

class TestSharedMemory final : public rex::graphics::SharedMemory {
 public:
  explicit TestSharedMemory(rex::memory::Memory& memory) : SharedMemory(memory) {
    InitializeCommon();
  }

 private:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>&) override { return false; }
};

constexpr uint32_t kAllocation =
    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
constexpr uint32_t kWritable = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

}  // namespace

TEST_CASE("Frame input buffer versions follow protected guest writes", "[graphics][memory]") {
  auto& memory = rex::testing::GetTestMemory();
  uint32_t heap_base = 0xA0000000;
  SECTION("64 KiB physical heap") {}
  SECTION("16 MiB physical heap") {
    heap_base = 0xC0000000;
  }
  SECTION("4 KiB physical heap with its physical offset") {
    heap_base = 0xE0000000;
  }
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(heap_base));
  REQUIRE(heap);
  const auto page = uint32_t(rex::memory::page_size());
  uint32_t guest = 0;
  REQUIRE(heap->Alloc(page, page, kAllocation, kWritable, false, &guest));
  const uint32_t address = memory.GetPhysicalAddress(guest);
  {
    TestSharedMemory shared(memory);
    rex::graphics::FrameInputBufferVersions versions(memory, shared);
    auto first = versions.Get(address, 16);
    REQUIRE(first.IsCurrent());
    CHECK(versions.Get(address, 16).identity == first.identity);
    CHECK(versions.Get(address, 16).generation == first.generation);
    CHECK(versions.Get(address, 8).identity != first.identity);
    // Subsequent protected writes must invalidate after rearming. Checking
    // alone must not silently rearm an old token.
    *memory.TranslateVirtual<volatile uint32_t*>(guest) = 1;
    CHECK_FALSE(first.IsCurrent());
    auto second = versions.Get(address, 16);
    REQUIRE(second.IsCurrent());
    CHECK(second.identity == first.identity);
    CHECK(second.generation != first.generation);
    *memory.TranslateVirtual<volatile uint32_t*>(guest + sizeof(uint32_t)) = 2;
    CHECK_FALSE(second.IsCurrent());
    CHECK_FALSE(first.IsCurrent());
    auto third = versions.Get(address, 16);
    REQUIRE(third.IsCurrent());
    CHECK(third.generation != second.generation);
    shared.ClearCache();
    CHECK_FALSE(third.IsCurrent());
    REQUIRE(versions.Get(address, 16).IsCurrent());
  }
  REQUIRE(heap->Release(guest, nullptr));
}

TEST_CASE("Frame input buffer versions reject GPU ownership and lost access",
          "[graphics][memory]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(heap);
  const auto page = uint32_t(rex::memory::page_size());
  uint32_t guest = 0;
  REQUIRE(heap->Alloc(page, page, kAllocation, kWritable, false, &guest));
  const uint32_t address = guest & 0x1FFFFFFF;
  {
    TestSharedMemory shared(memory);
    rex::graphics::FrameInputBufferVersions versions(memory, shared);
    const auto cpu = versions.Get(address, 16);
    REQUIRE(cpu.IsCurrent());
    shared.RangeWrittenByGpu(address, 16);
    CHECK_FALSE(cpu.IsCurrent());
    CHECK_FALSE(versions.Get(address, 16).IsCurrent());
    shared.SetSystemPageBlocksValidWithGpuDataWritten();
    CHECK_FALSE(versions.Get(address, 16).IsCurrent());
    *memory.TranslateVirtual<volatile uint32_t*>(guest) = 3;
    const auto rewritten = versions.Get(address, 16);
    REQUIRE(rewritten.IsCurrent());
    CHECK(rewritten.generation != cpu.generation);
    REQUIRE(heap->Protect(guest, page, rex::memory::kMemoryProtectNoAccess));
    CHECK_FALSE(rewritten.IsCurrent());
    CHECK_FALSE(versions.Get(address, 16).IsCurrent());
    REQUIRE(heap->Protect(guest, page, kWritable));
    CHECK_FALSE(rewritten.IsCurrent());
    REQUIRE(versions.Get(address, 16).IsCurrent());
  }
  REQUIRE(heap->Release(guest, nullptr));
}

TEST_CASE("Frame input buffer versions retire released allocations", "[graphics][memory]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(heap);
  const auto page = uint32_t(rex::memory::page_size());
  uint32_t guest = 0;
  REQUIRE(heap->Alloc(page, page, kAllocation, kWritable, false, &guest));
  const uint32_t address = guest & 0x1FFFFFFF;
  {
    TestSharedMemory shared(memory);
    rex::graphics::FrameInputBufferVersions versions(memory, shared);
    const auto before = versions.Get(address, 16);
    REQUIRE(before.IsCurrent());
    REQUIRE(heap->Release(guest, nullptr));
    CHECK_FALSE(before.IsCurrent());
    CHECK_FALSE(versions.Get(address, 16).IsCurrent());
    REQUIRE(heap->AllocFixed(guest, page, page, kAllocation, kWritable));
    const auto reused = versions.Get(address, 16);
    REQUIRE(reused.IsCurrent());
    CHECK(reused.generation != before.generation);
    CHECK_FALSE(before.IsCurrent());
    const auto identity = reused.identity;
    versions.Reset();
    // Borrowed contexts end at Reset. Compare only retained scalars afterward.
    const auto next_frame = versions.Get(address, 16);
    REQUIRE(next_frame.IsCurrent());
    CHECK(next_frame.identity != identity);
  }
  REQUIRE(heap->Release(guest, nullptr));
}

TEST_CASE("Frame input buffer versions retire recommitted allocations", "[graphics][memory]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(heap);
  const auto page = uint32_t(rex::memory::page_size());
  uint32_t guest = 0;
  REQUIRE(heap->Alloc(page, page, kAllocation, kWritable, false, &guest));
  const uint32_t address = memory.GetPhysicalAddress(guest);
  {
    TestSharedMemory shared(memory);
    rex::graphics::FrameInputBufferVersions versions(memory, shared);
    const auto before = versions.Get(address, 16);
    REQUIRE(before.IsCurrent());
    REQUIRE(heap->AllocFixed(guest, page, page, kAllocation, kWritable));
    *memory.TranslateVirtual<volatile uint32_t*>(guest) = 4;
    CHECK_FALSE(before.IsCurrent());
    const auto after = versions.Get(address, 16);
    REQUIRE(after.IsCurrent());
    *memory.TranslateVirtual<volatile uint32_t*>(guest) = 5;
    CHECK_FALSE(after.IsCurrent());
  }
  REQUIRE(heap->Release(guest, nullptr));
}

TEST_CASE("Frame input buffer versions retire a new overlapping alias", "[graphics][memory]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  auto* alias_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xE0000000));
  REQUIRE(heap);
  REQUIRE(alias_heap);
  const auto page = uint32_t(rex::memory::page_size());
  uint32_t guest = 0;
  REQUIRE(heap->Alloc(page * 2, page, kAllocation, kWritable, false, &guest));
  const uint32_t address = memory.GetPhysicalAddress(guest) + page;
  const uint32_t alias = 0xE0000000 + address - 0x1000;
  {
    TestSharedMemory shared(memory);
    rex::graphics::FrameInputBufferVersions versions(memory, shared);
    const auto before = versions.Get(address, 16);
    REQUIRE(before.IsCurrent());
    REQUIRE(alias_heap->AllocFixed(alias, page, page, kAllocation, kWritable));
    CHECK_FALSE(before.IsCurrent());
    const auto after = versions.Get(address, 16);
    REQUIRE(after.IsCurrent());
    *memory.TranslateVirtual<volatile uint32_t*>(alias) = 6;
    CHECK_FALSE(after.IsCurrent());
  }
  REQUIRE(alias_heap->Release(alias, nullptr));
  REQUIRE(heap->Release(guest, nullptr));
}

TEST_CASE("Frame input buffer versions bound ranges without evicting views", "[graphics][memory]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(heap);
  const auto page = uint32_t(rex::memory::page_size());
  uint32_t guest = 0;
  REQUIRE(heap->Alloc(page * 2, page, kAllocation, kWritable, false, &guest));
  const uint32_t address = guest & 0x1FFFFFFF;
  {
    TestSharedMemory shared(memory);
    rex::graphics::FrameInputBufferVersions versions(memory, shared);
    CHECK_FALSE(versions.Get(address, 0).IsCurrent());
    CHECK_FALSE(versions.Get(rex::graphics::SharedMemory::kBufferSize - 1, 2).IsCurrent());
    const auto first = versions.Get(address, 1);
    REQUIRE(first.IsCurrent());
    for (uint32_t i = 1; i < versions.kMaxRanges; ++i) {
      REQUIRE(versions.Get(address + i, 1).IsCurrent());
    }
    CHECK_FALSE(versions.Get(address + uint32_t(versions.kMaxRanges), 1).IsCurrent());
    CHECK(first.IsCurrent());
    CHECK(versions.Get(address, 1).identity == first.identity);
  }
  REQUIRE(heap->Release(guest, nullptr));
}
