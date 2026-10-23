#include <algorithm>
#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xmemory.h>

#include "kernel/xam/apps/xlivebase_request.h"
#include "test_memory.h"

namespace rex::kernel::xam::apps {
namespace {

void StoreBe32(uint8_t* bytes, uint32_t value) {
  bytes[0] = uint8_t(value >> 24);
  bytes[1] = uint8_t(value >> 16);
  bytes[2] = uint8_t(value >> 8);
  bytes[3] = uint8_t(value);
}

// Builds the list the way the guest does: type 4 and a sign-extended pointer
// per entry, then the count after the 32 entries.
void BuildArgumentList(uint8_t* bytes, const uint32_t* pointers, uint32_t count) {
  std::fill_n(bytes, kArgumentListSize, uint8_t{0xA5});
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* entry = bytes + i * kArgumentEntrySize;
    StoreBe32(entry, 4);
    const uint32_t high = (pointers[i] & 0x80000000u) ? 0xFFFFFFFFu : 0;
    StoreBe32(entry + 8, high);
    StoreBe32(entry + 12, pointers[i]);
  }
  StoreBe32(bytes + kArgumentCountOffset, count);
}

TEST_CASE("XLiveBase friends request decodes five sign-extended pointers",
          "[kernel][xam][xlivebase]") {
  const std::array<uint32_t, 5> pointers = {0x7018E4E4, 0x7018E4EC, 0x7018E4F4, 0x8312B8D4,
                                            0x7018E3B0};
  std::array<uint8_t, kArgumentListSize> bytes{};
  BuildArgumentList(bytes.data(), pointers.data(), 5);

  FriendsEnumeratorRequest request{};
  REQUIRE(DecodeFriendsEnumeratorRequest(bytes, request));
  CHECK(request.user_index_ptr == 0x7018E4E4);
  CHECK(request.starting_index_ptr == 0x7018E4EC);
  CHECK(request.friend_count_ptr == 0x7018E4F4);
  CHECK(request.buffer_size_ptr == 0x8312B8D4);
  CHECK(request.handle_ptr == 0x7018E3B0);

  BuildArgumentList(bytes.data(), pointers.data(), 4);
  CHECK_FALSE(DecodeFriendsEnumeratorRequest(bytes, request));
  CHECK_FALSE(DecodeFriendsEnumeratorRequest(
      std::span<const uint8_t>(bytes.data(), kArgumentListSize - 1), request));
}

TEST_CASE("XLiveBase friends request requires readable argument pointers",
          "[kernel][xam][xlivebase]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = memory.LookupHeap(0x10000000);
  REQUIRE(heap != nullptr);
  uint32_t allocation = 0;
  REQUIRE(heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &allocation));

  const uint32_t values = allocation + kArgumentListSize;
  std::array<uint32_t, 5> pointers = {values, values + 4, values + 8, values + 12, values + 16};
  auto* list = memory.TranslateVirtual<uint8_t*>(allocation);
  BuildArgumentList(list, pointers.data(), 5);

  FriendsEnumeratorRequest request{};
  REQUIRE(TryDecodeFriendsEnumeratorRequest(memory, allocation, request));
  CHECK(request.handle_ptr == values + 16);

  pointers[4] = 0;
  BuildArgumentList(list, pointers.data(), 5);
  FriendsEnumeratorRequest unchanged{};
  CHECK_FALSE(TryDecodeFriendsEnumeratorRequest(memory, allocation, unchanged));
  CHECK(unchanged.handle_ptr == 0);

  CHECK_FALSE(TryDecodeFriendsEnumeratorRequest(memory, allocation + 4096 - 16, request));
  REQUIRE(heap->Release(allocation, nullptr));
}

}  // namespace
}  // namespace rex::kernel::xam::apps
