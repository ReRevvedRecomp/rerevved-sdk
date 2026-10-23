#pragma once

#include "relay_invite.h"

namespace rex::relay {

// Live invites use ASCII "rr1." plus unpadded base64url: version 1, session u64
// big-endian, token id[16], secret[32], then length-byte relay "host:port"; the
// relay derives the room.
inline constexpr std::string_view kLiveInvitePrefix = "rr1.";
inline constexpr uint8_t kLiveInviteVersion = 1;
// Discord's join secret limit; longer relay addresses cannot be encoded.
inline constexpr size_t kMaxLiveInviteLength = 128;

struct LiveInvite {
  uint64_t session_id = 0;
  TokenId token_id{};
  TokenSecret token_secret;
  InviteAddress address;
};

Result<std::string> EncodeLiveInvite(const LiveInvite& invite);
Result<LiveInvite> DecodeLiveInvite(std::string_view code);

}  // namespace rex::relay
