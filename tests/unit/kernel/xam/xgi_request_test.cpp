#include <algorithm>
#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xmemory.h>

#include "kernel/xam/apps/xgi_request.h"
#include "test_memory.h"

namespace rex::kernel::xam::apps {
namespace {

constexpr SearchByIdRequest kUnchangedRequest = {
    0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD, 0xEEEEEEEE,
};

bool RequestsEqual(const SearchByIdRequest& left, const SearchByIdRequest& right) {
  return left.user_index == right.user_index && left.num_session_ids == right.num_session_ids &&
         left.session_ids_ptr == right.session_ids_ptr &&
         left.results_buffer_size == right.results_buffer_size &&
         left.search_results_ptr == right.search_results_ptr;
}

TEST_CASE("XGI search-by-ID rejects malformed guest ranges", "[kernel][xam][xgi]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = memory.LookupHeap(0x10000000);
  REQUIRE(heap != nullptr);

  SearchByIdRequest request = kUnchangedRequest;
  CHECK_FALSE(TryDecodeSearchByIdRequest(memory, 0, kSearchByIdRequestSize, request));
  CHECK(RequestsEqual(request, kUnchangedRequest));

  uint32_t address = 0;
  REQUIRE(heap->Alloc(kSearchByIdRequestSize, 4096, rex::memory::kMemoryAllocationReserve,
                      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                      &address));
  CHECK_FALSE(TryDecodeSearchByIdRequest(memory, address, kSearchByIdRequestSize, request));
  CHECK(RequestsEqual(request, kUnchangedRequest));
  REQUIRE(heap->Release(address, nullptr));

  CHECK_FALSE(IsReadableGuestRange(memory, 0xDFFFFFFF, 2));
}

TEST_CASE("XGI search-by-ID rejects every short request before its guard", "[kernel][xam][xgi]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = memory.LookupHeap(0x10000000);
  REQUIRE(heap != nullptr);

  uint32_t allocation = 0;
  REQUIRE(heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &allocation));
  const uint32_t address = allocation + 4096 - (kSearchByIdRequestSize - 1);
  auto* bytes = memory.TranslateVirtual<uint8_t*>(address);
  std::fill_n(bytes, kSearchByIdRequestSize - 1, uint8_t{0xA5});

  for (uint32_t size = 0; size < kSearchByIdRequestSize; ++size) {
    SearchByIdRequest request = kUnchangedRequest;
    CHECK_FALSE(TryDecodeSearchByIdRequest(memory, address, size, request));
    CHECK(RequestsEqual(request, kUnchangedRequest));
  }
  REQUIRE(heap->Release(allocation, nullptr));
}

TEST_CASE("XGI search-by-ID decodes exactly 20 big-endian bytes", "[kernel][xam][xgi]") {
  const std::array<uint8_t, kSearchByIdRequestSize + 1> bytes = {
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x32, 0x54,
      0x76, 0x00, 0x00, 0x05, 0x36, 0x82, 0x10, 0x20, 0x30, 0xA5,
  };

  SearchByIdRequest request{};
  REQUIRE(DecodeSearchByIdRequest(std::span<const uint8_t>(bytes.data(), kSearchByIdRequestSize),
                                  request));
  CHECK(request.user_index == 0x01234567);
  CHECK(request.num_session_ids == 0x89ABCDEF);
  CHECK(request.session_ids_ptr == 0x10325476);
  CHECK(request.results_buffer_size == 1334);
  CHECK(request.search_results_ptr == 0x82102030);

  auto changed_sentinel = bytes;
  changed_sentinel[kSearchByIdRequestSize] = 0x5A;
  SearchByIdRequest changed_request{};
  REQUIRE(DecodeSearchByIdRequest(
      std::span<const uint8_t>(changed_sentinel.data(), kSearchByIdRequestSize), changed_request));
  CHECK(RequestsEqual(changed_request, request));

  auto& memory = rex::testing::GetTestMemory();
  auto* heap = memory.LookupHeap(0x10000000);
  REQUIRE(heap != nullptr);
  uint32_t allocation = 0;
  REQUIRE(heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &allocation));
  const uint32_t guarded_address = allocation + 4096 - kSearchByIdRequestSize;
  std::copy_n(bytes.data(), kSearchByIdRequestSize,
              memory.TranslateVirtual<uint8_t*>(guarded_address));
  SearchByIdRequest guarded_request{};
  REQUIRE(
      TryDecodeSearchByIdRequest(memory, guarded_address, kSearchByIdRequestSize, guarded_request));
  CHECK(RequestsEqual(guarded_request, request));
  REQUIRE(heap->Release(allocation, nullptr));
}

TEST_CASE("XGI search-by-ID rejects oversized requests and ignores trailing sentinels",
          "[kernel][xam][xgi]") {
  std::array<uint8_t, 32> bytes{};
  std::fill(bytes.begin() + kSearchByIdRequestSize, bytes.end(), uint8_t{0xA5});

  for (uint32_t size : {21u, 24u, 32u}) {
    SearchByIdRequest request = kUnchangedRequest;
    CHECK_FALSE(DecodeSearchByIdRequest(std::span<const uint8_t>(bytes.data(), size), request));
    CHECK(RequestsEqual(request, kUnchangedRequest));

    std::fill(bytes.begin() + kSearchByIdRequestSize, bytes.end(), uint8_t{0x5A});
    CHECK_FALSE(DecodeSearchByIdRequest(std::span<const uint8_t>(bytes.data(), size), request));
    CHECK(RequestsEqual(request, kUnchangedRequest));
  }
}

}  // namespace
}  // namespace rex::kernel::xam::apps
