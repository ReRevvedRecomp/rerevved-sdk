#include "live_invite.h"

#include <algorithm>

namespace rex::relay {
namespace {
constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
// Version, session id, token id, token secret and the address length byte.
constexpr size_t kFixedSize = 1 + 8 + sizeof(TokenId) + TokenSecret::kSize + 1;
constexpr size_t kMaxBytes = (kMaxLiveInviteLength - kLiveInvitePrefix.size()) * 6 / 8;

// Payload bytes hold the token secret, so every copy is wiped on destruction.
struct Payload {
  std::array<uint8_t, kMaxBytes> bytes{};
  size_t size = 0;
  ~Payload() { SecureWipe(bytes); }
};

Error Damaged() {
  return {ErrorCategory::Config, "Live invite code is damaged or incomplete"};
}
int Sextet(char c) {
  const auto found = kAlphabet.find(c);
  return found == std::string_view::npos ? -1 : static_cast<int>(found);
}
}  // namespace

Result<std::string> EncodeLiveInvite(const LiveInvite& invite) {
  const std::string address = invite.address.host + ":" + std::to_string(invite.address.port);
  if (!invite.session_id || !ParseInviteAddress(address))
    return std::unexpected(Error{ErrorCategory::Config, "Live invite needs a session and address"});
  if (kFixedSize + address.size() > kMaxBytes)
    return std::unexpected(
        Error{ErrorCategory::Config, "Relay address is too long for a Live invite code"});
  Payload payload;
  auto put = [&](uint8_t byte) { payload.bytes[payload.size++] = byte; };
  put(kLiveInviteVersion);
  for (int shift = 56; shift >= 0; shift -= 8)
    put(static_cast<uint8_t>(invite.session_id >> shift));
  for (uint8_t byte : invite.token_id)
    put(byte);
  for (uint8_t byte : invite.token_secret.bytes())
    put(byte);
  put(static_cast<uint8_t>(address.size()));
  for (unsigned char c : address)
    put(c);
  std::string code(kLiveInvitePrefix);
  uint32_t bits = 0;
  unsigned count = 0;
  for (size_t i = 0; i < payload.size; ++i) {
    bits = (bits << 8) | payload.bytes[i];
    count += 8;
    while (count >= 6) {
      count -= 6;
      code += kAlphabet[(bits >> count) & 63];
    }
  }
  if (count)
    code += kAlphabet[(bits << (6 - count)) & 63];
  if (code.size() > kMaxLiveInviteLength)
    return std::unexpected(
        Error{ErrorCategory::Config, "Relay address is too long for a Live invite code"});
  return code;
}

Result<LiveInvite> DecodeLiveInvite(std::string_view code) {
  if (!code.starts_with(kLiveInvitePrefix))
    return std::unexpected(Error{ErrorCategory::Config, "not a Live invite code"});
  const auto text = code.substr(kLiveInvitePrefix.size());
  // A single trailing character cannot complete a byte.
  if (code.size() > kMaxLiveInviteLength || text.size() % 4 == 1)
    return std::unexpected(Damaged());
  Payload payload;
  uint32_t bits = 0;
  unsigned count = 0;
  for (char c : text) {
    const int value = Sextet(c);
    if (value < 0)
      return std::unexpected(Damaged());
    bits = (bits << 6) | static_cast<uint32_t>(value);
    count += 6;
    if (count >= 8) {
      count -= 8;
      payload.bytes[payload.size++] = static_cast<uint8_t>(bits >> count);
    }
  }
  // Leftover bits are padding and must be zero, so each payload has one spelling.
  if (bits & ((1u << count) - 1))
    return std::unexpected(Damaged());
  if (payload.size < 1)
    return std::unexpected(Damaged());
  if (payload.bytes[0] != kLiveInviteVersion)
    return std::unexpected(
        Error{ErrorCategory::Config, "Live invite code was made for another version"});
  if (payload.size < kFixedSize || payload.size != kFixedSize + payload.bytes[kFixedSize - 1])
    return std::unexpected(Damaged());
  LiveInvite invite;
  size_t offset = 1;
  for (int i = 0; i < 8; ++i)
    invite.session_id = (invite.session_id << 8) | payload.bytes[offset++];
  std::copy_n(payload.bytes.begin() + offset, invite.token_id.size(), invite.token_id.begin());
  offset += invite.token_id.size();
  invite.token_secret = TokenSecret(std::span<const uint8_t, TokenSecret::kSize>(
      payload.bytes.data() + offset, TokenSecret::kSize));
  offset += TokenSecret::kSize + 1;
  const std::string_view address(reinterpret_cast<const char*>(payload.bytes.data() + offset),
                                 payload.size - offset);
  if (!invite.session_id)
    return std::unexpected(Damaged());
  auto parsed = ParseInviteAddress(address);
  if (address.empty() || !parsed)
    return std::unexpected(Error{ErrorCategory::Config, "Live invite relay address is invalid"});
  invite.address = std::move(*parsed);
  return invite;
}

}  // namespace rex::relay
