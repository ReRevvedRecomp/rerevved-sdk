

/**
 * @file        physical_write_watch_test.cpp
 * @brief       Unit tests for physical memory write notices
 *
 * Writes that reach guest physical bytes must reach the invalidation
 * callbacks of a watched range: guest stores, host writes through the
 * physical view, stores the stale-protection recovery lets through, and
 * stores through a guest view that did not allocate the page.
 */

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include "test_memory.h"

namespace {

constexpr uint32_t kAllocation =
    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
constexpr uint32_t kWritable = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

// Records every notice; unwatches only the pages it names. `during` runs once
// inside the next notice, as another thread holding the lock after it would.
struct Notices {
  rex::memory::Memory& memory;
  void* handle = nullptr;
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  std::function<void()> during;

  explicit Notices(rex::memory::Memory& owner) : memory(owner) {
    handle = memory.RegisterPhysicalMemoryInvalidationCallback(&Record, this);
  }
  ~Notices() { memory.UnregisterPhysicalMemoryInvalidationCallback(handle); }

  static std::pair<uint32_t, uint32_t> Record(void* context, uint32_t physical, uint32_t length,
                                              bool) {
    auto& self = *static_cast<Notices*>(context);
    self.ranges.emplace_back(physical, length);
    if (auto during = std::exchange(self.during, nullptr)) {
      during();
    }
    return {physical, length};
  }

  bool Covered(uint32_t physical) const {
    for (const auto& [start, length] : ranges) {
      if (physical >= start && physical - start < length) {
        return true;
      }
    }
    return false;
  }
};

struct Allocation {
  rex::memory::BaseHeap* heap = nullptr;
  uint32_t guest = 0;
  uint32_t physical = 0;

  Allocation(rex::memory::Memory& memory, uint32_t heap_base, uint32_t size) {
    heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(heap_base));
    REQUIRE(heap);
    REQUIRE(heap->Alloc(size, size, kAllocation, kWritable, false, &guest));
    physical = memory.GetPhysicalAddress(guest);
  }
  ~Allocation() { heap->Release(guest, nullptr); }
};

// Clears what a test left watched in the views that did not allocate a page.
void Unwatch(rex::memory::Memory& memory, uint32_t physical, uint32_t length) {
  for (const uint32_t base : {0xA0000000u, 0xC0000000u, 0xE0000000u - 0x1000u}) {
    memory.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                          base + physical, length, true, true);
  }
}

}  // namespace

TEST_CASE("Host physical writes notify watched ranges only", "[memory][watch]") {
  auto& memory = rex::testing::GetTestMemory();
  const auto page = uint32_t(rex::memory::page_size());
  Allocation block(memory, 0xA0000000, 0x10000);
  Notices notices(memory);

  // An unwatched range emits no notice.
  memory.NotifyPhysicalWrite(block.physical, 16);
  CHECK(notices.ranges.empty());

  // As the XMA decoder writes output: through the physical view, then the notice.
  memory.EnablePhysicalMemoryAccessCallbacks(block.physical + page, page, true, false);
  *memory.TranslatePhysical<volatile uint32_t*>(block.physical + page + 8) = 1;
  memory.NotifyPhysicalWrite(block.physical + page + 8, 4);
  REQUIRE(notices.ranges.size() == 1);
  CHECK(notices.ranges[0] == std::pair{block.physical + page, page});

  // The notice leaves the watch in place, so a guest store still reports.
  notices.ranges.clear();
  *memory.TranslateVirtual<volatile uint32_t*>(block.guest + page) = 2;
  CHECK(notices.Covered(block.physical + page));
  CHECK(*memory.TranslatePhysical<volatile uint32_t*>(block.physical + page) == 2);
}

TEST_CASE("Stale protection recovery reports the store", "[memory][watch]") {
  auto& memory = rex::testing::GetTestMemory();
  const auto page = uint32_t(rex::memory::page_size());
  Allocation block(memory, 0xA0000000, 0x10000);
  Notices notices(memory);

  // Read-only host protection with no watch: the recovery branch restores
  // write access, and the store must still be reported.
  REQUIRE(rex::memory::Protect(memory.TranslateVirtual(block.guest + page), page,
                               rex::memory::PageAccess::kReadOnly, nullptr));
  *memory.TranslateVirtual<volatile uint32_t*>(block.guest + page + 4) = 3;
  CHECK(notices.Covered(block.physical + page));
  CHECK_FALSE(notices.Covered(block.physical));
  CHECK(*memory.TranslatePhysical<volatile uint32_t*>(block.physical + page + 4) == 3);
}

TEST_CASE("Watches cover every guest view of a page", "[memory][watch]") {
  auto& memory = rex::testing::GetTestMemory();
  const auto page = uint32_t(rex::memory::page_size());
  Allocation block(memory, 0xA0000000, 0x10000);
  Notices notices(memory);
  const uint32_t physical = block.physical + page;
  // Views that did not allocate the page still reach its bytes.
  const uint32_t e0 = 0xE0000000 + physical - 0x1000;
  const uint32_t c0 = 0xC0000000 + physical;

  CHECK_FALSE(memory.IsPhysicalRangeWriteWatched(physical, page));
  memory.EnablePhysicalMemoryAccessCallbacks(physical, page, true, false);
  CHECK(memory.IsPhysicalRangeWriteWatched(physical, page));

  *memory.TranslateVirtual<volatile uint32_t*>(e0 + 4) = 7;
  CHECK(notices.Covered(physical));
  CHECK(*memory.TranslatePhysical<volatile uint32_t*>(physical + 4) == 7);
  CHECK_FALSE(memory.IsPhysicalRangeWriteWatched(physical, page));
  // Unwatching gave the alias its write access back.
  notices.ranges.clear();
  *memory.TranslateVirtual<volatile uint32_t*>(e0 + 8) = 8;
  CHECK(notices.ranges.empty());
  CHECK(*memory.TranslatePhysical<volatile uint32_t*>(physical + 8) == 8);

  memory.EnablePhysicalMemoryAccessCallbacks(physical, page, true, false);
  CHECK(memory.IsPhysicalRangeWriteWatched(physical, page));
  *memory.TranslateVirtual<volatile uint32_t*>(c0 + 12) = 9;
  CHECK(notices.Covered(physical));
  CHECK(*memory.TranslatePhysical<volatile uint32_t*>(physical + 12) == 9);
}

TEST_CASE("Committing an alias drops its stale watch", "[memory][watch]") {
  auto& memory = rex::testing::GetTestMemory();
  const auto page = uint32_t(rex::memory::page_size());
  Allocation block(memory, 0xA0000000, 0x10000);
  Notices notices(memory);
  const uint32_t physical = block.physical + page;
  const uint32_t e0 = 0xE0000000 + physical - 0x1000;
  auto* alias_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xE0000000));
  REQUIRE(alias_heap);

  memory.EnablePhysicalMemoryAccessCallbacks(physical, page, true, false);
  REQUIRE(alias_heap->AllocFixed(e0, page, page, kAllocation, kWritable));
  // The alias now has its own protection; a later watch must protect it again.
  CHECK_FALSE(memory.IsPhysicalRangeWriteWatched(physical, page));
  memory.EnablePhysicalMemoryAccessCallbacks(physical, page, true, false);
  CHECK(memory.IsPhysicalRangeWriteWatched(physical, page));
  notices.ranges.clear();
  *memory.TranslateVirtual<volatile uint32_t*>(e0) = 10;
  CHECK(notices.Covered(physical));
  CHECK(*memory.TranslatePhysical<volatile uint32_t*>(physical) == 10);
  REQUIRE(alias_heap->Release(e0, nullptr));
}

TEST_CASE("Allocating a watched range reports it", "[memory][watch]") {
  auto& memory = rex::testing::GetTestMemory();
  const auto page = uint32_t(rex::memory::page_size());
  auto* a0 = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  auto* e0 = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xE0000000));
  REQUIRE(a0);
  REQUIRE(e0);
  uint32_t guest = 0;
  REQUIRE(a0->Alloc(0x10000, 0x10000, kAllocation, kWritable, false, &guest));
  const uint32_t physical = memory.GetPhysicalAddress(guest) + page;
  REQUIRE(a0->Release(guest, nullptr));
  Notices notices(memory);

  // A free page can be watched; a fresh allocation of it makes its new view
  // writable, so the watchers are told rather than left with a silent view.
  memory.EnablePhysicalMemoryAccessCallbacks(physical, page, true, false);
  REQUIRE(memory.IsPhysicalRangeWriteWatched(physical, page));
  const uint32_t alias = 0xE0000000 + physical - 0x1000;
  uint32_t allocated = 0;
  REQUIRE(e0->AllocRange(alias, alias + page - 1, page, page, kAllocation, kWritable, false,
                         &allocated));
  CHECK(allocated == alias);
  CHECK(notices.Covered(physical));
  CHECK_FALSE(memory.IsPhysicalRangeWriteWatched(physical, page));
  REQUIRE(e0->Release(allocated, nullptr));

  // Leave no watch on the free page for later tests.
  Unwatch(memory, physical, page);
}

TEST_CASE("Recovery leaves no watch it then makes silent", "[memory][watch]") {
  auto& memory = rex::testing::GetTestMemory();
  const auto page = uint32_t(rex::memory::page_size());
  Allocation block(memory, 0xA0000000, 0x10000);
  Notices notices(memory);
  const uint32_t physical = block.physical + page;

  // A reader arms the page between the recovery's notice and its restore.
  REQUIRE(rex::memory::Protect(memory.TranslateVirtual(block.guest + page), page,
                               rex::memory::PageAccess::kReadOnly, nullptr));
  notices.during = [&] { memory.EnablePhysicalMemoryAccessCallbacks(physical, page, true, false); };
  *memory.TranslateVirtual<volatile uint32_t*>(block.guest + page) = 11;
  CHECK(notices.Covered(physical));
  CHECK(*memory.TranslatePhysical<volatile uint32_t*>(physical) == 11);
  // Write access is back, so the watch must not be trusted.
  CHECK_FALSE(memory.IsPhysicalRangeWriteWatched(physical, page));
  memory.EnablePhysicalMemoryAccessCallbacks(physical, page, true, false);
  REQUIRE(memory.IsPhysicalRangeWriteWatched(physical, page));
  notices.ranges.clear();
  *memory.TranslateVirtual<volatile uint32_t*>(block.guest + page + 4) = 12;
  CHECK(notices.Covered(physical));
  Unwatch(memory, physical, page);
}

TEST_CASE("Protect reports every guest page it replaces", "[memory][watch]") {
  auto& memory = rex::testing::GetTestMemory();
  const auto page = uint32_t(rex::memory::page_size());
  Allocation block(memory, 0xA0000000, 0x10000);
  Notices notices(memory);

  // The guest page is 64 KiB: protecting one system page of it replaces the
  // protection of all of them, including a watch armed during the notice.
  memory.EnablePhysicalMemoryAccessCallbacks(block.physical, 0x10000, true, false);
  REQUIRE(memory.IsPhysicalRangeWriteWatched(block.physical, 0x10000));
  notices.during = [&] {
    memory.EnablePhysicalMemoryAccessCallbacks(block.physical, 0x10000, true, false);
  };
  REQUIRE(block.heap->Protect(block.guest + page, page, kWritable));
  CHECK(notices.Covered(block.physical));
  CHECK(notices.Covered(block.physical + 0x8000));
  CHECK_FALSE(memory.IsPhysicalRangeWriteWatched(block.physical + 0x8000, page));

  memory.EnablePhysicalMemoryAccessCallbacks(block.physical, 0x10000, true, false);
  REQUIRE(memory.IsPhysicalRangeWriteWatched(block.physical, 0x10000));
  notices.ranges.clear();
  *memory.TranslateVirtual<volatile uint32_t*>(block.guest + 0x8000) = 13;
  CHECK(notices.Covered(block.physical + 0x8000));
  Unwatch(memory, block.physical, 0x10000);
}

TEST_CASE("An aligned allocation retires all it commits", "[memory][watch]") {
  auto& memory = rex::testing::GetTestMemory();
  constexpr uint32_t kAlignment = 0x100000;
  auto* a0 = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(a0);
  uint32_t guest = 0;
  REQUIRE(a0->Alloc(0x10000, kAlignment, kAllocation, kWritable, false, &guest));
  const uint32_t physical = memory.GetPhysicalAddress(guest);
  REQUIRE(a0->Release(guest, nullptr));
  Notices notices(memory);

  // The 64 KiB allocation commits its whole 1 MiB alignment in this view.
  memory.EnablePhysicalMemoryAccessCallbacks(physical, kAlignment, true, false);
  REQUIRE(memory.IsPhysicalRangeWriteWatched(physical, kAlignment));
  REQUIRE(a0->Alloc(0x10000, kAlignment, kAllocation, kWritable, false, &guest));
  REQUIRE(memory.GetPhysicalAddress(guest) == physical);
  CHECK(notices.Covered(physical + 0x80000));
  CHECK_FALSE(memory.IsPhysicalRangeWriteWatched(physical + 0x80000, 0x10000));
  REQUIRE(a0->Release(guest, nullptr));
  Unwatch(memory, physical, kAlignment);
}

TEST_CASE("Host OS writes go through the physical view", "[memory][watch]") {
  auto& memory = rex::testing::GetTestMemory();
  const auto page = uint32_t(rex::memory::page_size());
  Allocation block(memory, 0xA0000000, 0x10000);
  Notices notices(memory);

  // As a socket receive into a watched buffer: the OS writes the physical
  // view, which no watch protects, and the caller reports the bytes.
  memory.EnablePhysicalMemoryAccessCallbacks(block.physical + page, page, true, false);
  uint32_t physical = 0;
  auto* buffer = memory.TranslateForHostWrite(block.guest + page + 8, 8, &physical);
  CHECK(physical == block.physical + page + 8);
  REQUIRE(buffer == memory.TranslatePhysical(physical));
  buffer[0] = 14;
  memory.NotifyPhysicalWrite(physical, 1);
  CHECK(notices.Covered(block.physical + page));
  CHECK(*memory.TranslateVirtual<volatile uint8_t*>(block.guest + page + 8) == 14);

  // A range the guest may not write keeps the guest view and its failure.
  REQUIRE(block.heap->Protect(block.guest, 0x10000, rex::memory::kMemoryProtectRead));
  CHECK(memory.TranslateForHostWrite(block.guest + 8, 8, &physical) ==
        memory.TranslateVirtual(block.guest + 8));
  CHECK(physical == UINT32_MAX);
  Unwatch(memory, block.physical, 0x10000);
}
