#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

#include <rex/system/xmemory.h>

#include "xgi_request.h"

namespace rex::kernel::xam::apps {

// XLiveBase in-process argument list: 32 entries of {be32 type, be32 pad,
// be64 value}, then the entry count. Values are sign-extended guest pointers.
constexpr uint32_t kArgumentEntrySize = 16;
constexpr uint32_t kArgumentEntryCapacity = 32;
constexpr uint32_t kArgumentCountOffset = kArgumentEntrySize * kArgumentEntryCapacity;
constexpr uint32_t kArgumentListSize = kArgumentCountOffset + 4;

// XLiveBase 0x00058020: pointers to the user index, first friend index and
// friend count, then the buffer-size and handle outputs.
constexpr uint32_t kFriendsEnumeratorArgumentCount = 5;

struct FriendsEnumeratorRequest {
  uint32_t user_index_ptr;
  uint32_t starting_index_ptr;
  uint32_t friend_count_ptr;
  uint32_t buffer_size_ptr;
  uint32_t handle_ptr;
};

inline uint32_t ArgumentPointer(std::span<const uint8_t> bytes, uint32_t index) {
  return static_cast<uint32_t>(LoadBe64(bytes, size_t(index) * kArgumentEntrySize + 8));
}

inline bool DecodeFriendsEnumeratorRequest(std::span<const uint8_t> bytes,
                                           FriendsEnumeratorRequest& out_request) {
  if (bytes.size() != kArgumentListSize ||
      LoadBe32(bytes, kArgumentCountOffset) != kFriendsEnumeratorArgumentCount) {
    return false;
  }

  out_request = {
      ArgumentPointer(bytes, 0), ArgumentPointer(bytes, 1), ArgumentPointer(bytes, 2),
      ArgumentPointer(bytes, 3), ArgumentPointer(bytes, 4),
  };
  return true;
}

inline bool TryDecodeFriendsEnumeratorRequest(rex::memory::Memory& memory, uint32_t address,
                                              FriendsEnumeratorRequest& out_request) {
  if (!IsReadableGuestRange(memory, address, kArgumentListSize)) {
    return false;
  }
  const auto* bytes = memory.TranslateVirtual<const uint8_t*>(address);
  FriendsEnumeratorRequest request{};
  if (!DecodeFriendsEnumeratorRequest(std::span<const uint8_t>(bytes, kArgumentListSize),
                                      request)) {
    return false;
  }
  for (uint32_t pointer : {request.user_index_ptr, request.starting_index_ptr,
                           request.friend_count_ptr, request.buffer_size_ptr, request.handle_ptr}) {
    if (!IsReadableGuestRange(memory, pointer, 4)) {
      return false;
    }
  }
  out_request = request;
  return true;
}

}  // namespace rex::kernel::xam::apps
