#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/shared_memory.h>
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

}  // namespace

TEST_CASE("Shared memory ownership follows GPU writes and CPU reuse", "[graphics][memory]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(heap != nullptr);
  const auto page = uint32_t(rex::memory::page_size());
  uint32_t guest_address = 0;
  REQUIRE(heap->Alloc(page * 66, page * 64,
                      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                      &guest_address));
  const uint32_t address = guest_address & 0x1FFFFFFF;
  {
    TestSharedMemory shared(memory);
    CHECK_FALSE(shared.IsRangeWrittenByGpu(address, page * 66));
    shared.RangeWrittenByGpu(address + page * 63, page * 2);
    CHECK(shared.IsRangeWrittenByGpu(address + page * 63, page * 2));
    CHECK_FALSE(shared.IsRangeWrittenByGpu(address, page * 63));
    CHECK_FALSE(shared.IsRangeWrittenByGpu(address + page * 65, page));
    // Simulate the next frame's upload invalidation, which must preserve GPU
    // data rather than declaring all previous-frame bytes CPU-readable.
    shared.SetSystemPageBlocksValidWithGpuDataWritten();
    CHECK(shared.IsRangeWrittenByGpu(address + page * 63, page));
    // Exercise the actual protected-memory write callback, not a synthetic
    // reset of the queried flags.
    *memory.TranslateVirtual<volatile uint32_t*>(guest_address + page * 63) = 0x12345678;
    CHECK_FALSE(shared.IsRangeWrittenByGpu(address + page * 63, page));
    CHECK(shared.IsRangeWrittenByGpu(address + page * 64, page));
    *memory.TranslateVirtual<volatile uint32_t*>(guest_address + page * 64) = 0x87654321;
    CHECK_FALSE(shared.IsRangeWrittenByGpu(address, page * 66));
    CHECK_FALSE(shared.IsRangeWrittenByGpu(address, 0));
    CHECK(shared.IsRangeWrittenByGpu(rex::graphics::SharedMemory::kBufferSize - 1, 2));
  }
  REQUIRE(heap->Release(guest_address, nullptr));
}
