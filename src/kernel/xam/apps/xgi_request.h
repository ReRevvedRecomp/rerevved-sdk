#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <rex/system/xmemory.h>

namespace rex::kernel::xam::apps {

// XSessionSearchByID: user index, the 64-bit session id at +0x04 (4-byte
// aligned), the result buffer size at +0x0C and the result buffer at +0x10.
// A size probe stores the required size back at +0x0C.
constexpr uint32_t kSearchByIdRequestSize = 20;

struct SearchByIdRequest {
  uint32_t user_index;
  uint64_t session_id;
  uint32_t results_buffer_size;
  uint32_t search_results_ptr;
};

inline uint32_t LoadBe32(std::span<const uint8_t> bytes, size_t offset) {
  return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
         (uint32_t(bytes[offset + 2]) << 8) | uint32_t(bytes[offset + 3]);
}

inline uint64_t LoadBe64(std::span<const uint8_t> bytes, size_t offset) {
  return (uint64_t(LoadBe32(bytes, offset)) << 32) | LoadBe32(bytes, offset + 4);
}

inline bool DecodeSearchByIdRequest(std::span<const uint8_t> bytes,
                                    SearchByIdRequest& out_request) {
  if (bytes.size() != kSearchByIdRequestSize) {
    return false;
  }

  out_request = {
      LoadBe32(bytes, 0),
      LoadBe64(bytes, 4),
      LoadBe32(bytes, 12),
      LoadBe32(bytes, 16),
  };
  return true;
}

// XSessionWriteStats: the word at +0x04 is padding before the 8-byte XUID.
constexpr uint32_t kWriteStatsRequestSize = 24;

struct WriteStatsRequest {
  uint32_t session_ptr;
  uint64_t xuid;
  uint32_t num_views;
  uint32_t views_ptr;
};

inline bool DecodeWriteStatsRequest(std::span<const uint8_t> bytes,
                                    WriteStatsRequest& out_request) {
  if (bytes.size() != kWriteStatsRequestSize) {
    return false;
  }

  out_request = {
      LoadBe32(bytes, 0),
      LoadBe64(bytes, 8),
      LoadBe32(bytes, 16),
      LoadBe32(bytes, 20),
  };
  return true;
}

inline bool IsReadableGuestRange(rex::memory::Memory& memory, uint32_t address, uint32_t size) {
  if (!address || !size || address > std::numeric_limits<uint32_t>::max() - (size - 1)) {
    return false;
  }

  const uint32_t last_address = address + size - 1;
  uint64_t segment_start = address;
  while (segment_start <= last_address) {
    auto* heap = memory.LookupHeap(static_cast<uint32_t>(segment_start));
    if (!heap) {
      return false;
    }

    const uint64_t heap_last = uint64_t(heap->heap_base()) + heap->heap_size() - 1;
    const uint32_t segment_last =
        static_cast<uint32_t>(std::min<uint64_t>(last_address, heap_last));

    const uint64_t next_segment = uint64_t(segment_last) + 1;
    if (next_segment <= last_address) {
      const auto segment_last_host =
          reinterpret_cast<uintptr_t>(memory.TranslateVirtual(segment_last));
      const auto next_segment_host =
          reinterpret_cast<uintptr_t>(memory.TranslateVirtual(static_cast<uint32_t>(next_segment)));
      if (next_segment_host - segment_last_host != next_segment - segment_last) {
        return false;
      }
    }

    const uint32_t page_size = heap->page_size();
    const uint64_t first_page = segment_start & ~(uint64_t(page_size) - 1);
    const uint64_t last_page = segment_last & ~(uint64_t(page_size) - 1);
    for (uint64_t page = first_page; page <= last_page; page += page_size) {
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(static_cast<uint32_t>(page), &info) ||
          !(info.state & rex::memory::kMemoryAllocationCommit) ||
          !(info.protect & rex::memory::kMemoryProtectRead)) {
        return false;
      }
    }

    segment_start = next_segment;
  }
  return true;
}

inline bool TryDecodeSearchByIdRequest(rex::memory::Memory& memory, uint32_t address, uint32_t size,
                                       SearchByIdRequest& out_request) {
  if (size != kSearchByIdRequestSize || !IsReadableGuestRange(memory, address, size)) {
    return false;
  }
  const auto* bytes = memory.TranslateVirtual<const uint8_t*>(address);
  return DecodeSearchByIdRequest(std::span<const uint8_t>(bytes, size), out_request);
}

inline bool TryDecodeWriteStatsRequest(rex::memory::Memory& memory, uint32_t address, uint32_t size,
                                       WriteStatsRequest& out_request) {
  if (size != kWriteStatsRequestSize || !IsReadableGuestRange(memory, address, size)) {
    return false;
  }
  const auto* bytes = memory.TranslateVirtual<const uint8_t*>(address);
  return DecodeWriteStatsRequest(std::span<const uint8_t>(bytes, size), out_request);
}

}  // namespace rex::kernel::xam::apps
