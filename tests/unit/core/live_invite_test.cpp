#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "core/live_invite.h"

using namespace rex::relay;
namespace {

constexpr std::string_view kBase64Url =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Independent unpadded base64url, so tests can build malformed payloads.
std::string Code(const Bytes& payload) {
  std::string result = "rr1.";
  for (size_t i = 0; i < payload.size(); i += 3) {
    uint32_t chunk = uint32_t(payload[i]) << 16;
    if (i + 1 < payload.size())
      chunk |= uint32_t(payload[i + 1]) << 8;
    if (i + 2 < payload.size())
      chunk |= payload[i + 2];
    const size_t chars = std::min<size_t>(4, (payload.size() - i) * 8 / 6 + 1);
    for (size_t c = 0; c < chars; ++c)
      result += kBase64Url[(chunk >> (18 - 6 * c)) & 63];
  }
  return result;
}

TokenSecret Secret(uint8_t first) {
  std::array<uint8_t, TokenSecret::kSize> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<uint8_t>(first + i);
  return TokenSecret(bytes);
}

LiveInvite Make(std::string host = "relay.example.net", uint16_t port = 30100) {
  LiveInvite invite;
  invite.session_id = 0x0123456789ABCDEFull;
  for (size_t i = 0; i < invite.token_id.size(); ++i)
    invite.token_id[i] = static_cast<uint8_t>(0xA0 + i);
  invite.token_secret = Secret(0x40);
  invite.address = {std::move(host), port};
  return invite;
}

Bytes Payload(const LiveInvite& invite, std::string_view address) {
  Bytes payload{kLiveInviteVersion};
  for (int shift = 56; shift >= 0; shift -= 8)
    payload.push_back(static_cast<uint8_t>(invite.session_id >> shift));
  payload.insert(payload.end(), invite.token_id.begin(), invite.token_id.end());
  const auto secret = invite.token_secret.bytes();
  payload.insert(payload.end(), secret.begin(), secret.end());
  payload.push_back(static_cast<uint8_t>(address.size()));
  payload.insert(payload.end(), address.begin(), address.end());
  return payload;
}

}  // namespace

TEST_CASE("Live invite codes round trip every field", "[relay][live_invite]") {
  const auto invite = Make();
  auto code = EncodeLiveInvite(invite);
  REQUIRE(code);
  CHECK(code->starts_with("rr1."));
  CHECK(code->size() <= kMaxLiveInviteLength);
  CHECK(std::all_of(code->begin() + 4, code->end(),
                    [](char c) { return kBase64Url.find(c) != std::string_view::npos; }));
  CHECK(*code == Code(Payload(invite, "relay.example.net:30100")));
  auto decoded = DecodeLiveInvite(*code);
  REQUIRE(decoded);
  CHECK(decoded->session_id == invite.session_id);
  CHECK(decoded->token_id == invite.token_id);
  CHECK(decoded->token_secret == invite.token_secret);
  CHECK(decoded->address.host == "relay.example.net");
  CHECK(decoded->address.port == 30100);

  auto ipv4 = EncodeLiveInvite(Make("127.0.0.1", 1));
  REQUIRE(ipv4);
  auto decoded_ipv4 = DecodeLiveInvite(*ipv4);
  REQUIRE(decoded_ipv4);
  CHECK(decoded_ipv4->address.host == "127.0.0.1");
  CHECK(decoded_ipv4->address.port == 1);
}

TEST_CASE("Live invite codes fit Discord's 128-character join secret", "[relay][live_invite]") {
  // 58 fixed bytes plus a 35-byte address encode to exactly 124 base64url characters.
  const std::string longest_host(29, 'h');
  auto longest = EncodeLiveInvite(Make(longest_host, 30100));
  REQUIRE(longest);
  CHECK(longest->size() == kMaxLiveInviteLength);
  CHECK(DecodeLiveInvite(*longest));
  auto too_long = EncodeLiveInvite(Make(longest_host + "h", 30100));
  REQUIRE_FALSE(too_long);
  CHECK(too_long.error().message.find("too long") != std::string::npos);
  auto huge = EncodeLiveInvite(Make(std::string(200, 'h'), 30100));
  CHECK_FALSE(huge);
  // A decoder never accepts more than the limit, even for a well-formed payload.
  const auto oversized = Code(Payload(Make(), longest_host + "h:30100"));
  CHECK(oversized.size() > kMaxLiveInviteLength);
  CHECK_FALSE(DecodeLiveInvite(oversized));
}

TEST_CASE("Live invite encoding refuses an unusable session or address", "[relay][live_invite]") {
  auto no_session = Make();
  no_session.session_id = 0;
  CHECK_FALSE(EncodeLiveInvite(no_session));
  CHECK_FALSE(EncodeLiveInvite(Make("", 30100)));
  CHECK_FALSE(EncodeLiveInvite(Make("relay.example.net", 0)));
  CHECK_FALSE(EncodeLiveInvite(Make("bad host", 30100)));
}

TEST_CASE("Live invite decoding rejects every malformed code", "[relay][live_invite]") {
  const auto invite = Make();
  const auto valid = Payload(invite, "relay.example.net:30100");
  REQUIRE(DecodeLiveInvite(Code(valid)));

  SECTION("other prefixes") {
    const auto code = Code(valid);
    for (const std::string prefix : {"RR1.", "rr2.", "rr1-", "rr1", "", "RXR1-"})
      CHECK_FALSE(DecodeLiveInvite(prefix + code.substr(4)));
    CHECK_FALSE(DecodeLiveInvite(" " + code));
  }
  SECTION("other versions") {
    for (uint8_t version : {0, 2, 255}) {
      auto payload = valid;
      payload[0] = version;
      auto decoded = DecodeLiveInvite(Code(payload));
      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().message.find("version") != std::string::npos);
    }
  }
  SECTION("bad base64url") {
    auto code = Code(valid);
    for (char bad : {'+', '/', '=', '.', ' ', '!'}) {
      auto changed = code;
      changed[10] = bad;
      CHECK_FALSE(DecodeLiveInvite(changed));
    }
    CHECK_FALSE(DecodeLiveInvite(code + "="));
    // One trailing character cannot complete a byte.
    CHECK_FALSE(DecodeLiveInvite(code + "A"));
    // Nonzero padding bits give a second spelling of the same payload.
    const auto unaligned = Payload(invite, "relay.example.net:3010");
    REQUIRE((unaligned.size() * 8) % 6 != 0);
    auto padded = Code(unaligned);
    REQUIRE(DecodeLiveInvite(padded));
    const auto last = kBase64Url.find(padded.back());
    padded.back() = kBase64Url[last | 1];
    CHECK_FALSE(DecodeLiveInvite(padded));
  }
  SECTION("short or trailing bytes") {
    CHECK_FALSE(DecodeLiveInvite("rr1."));
    for (size_t size = 1; size < valid.size(); ++size)
      CHECK_FALSE(DecodeLiveInvite(Code(Bytes(valid.begin(), valid.begin() + size))));
    auto trailing = valid;
    trailing.push_back('0');
    CHECK_FALSE(DecodeLiveInvite(Code(trailing)));
  }
  SECTION("zero session id") {
    auto payload = valid;
    std::fill(payload.begin() + 1, payload.begin() + 9, uint8_t{0});
    CHECK_FALSE(DecodeLiveInvite(Code(payload)));
  }
  SECTION("empty or unparseable address") {
    for (std::string_view address : {"", ":30100", "relay.example.net", "relay.example.net:0",
                                     "relay.example.net:70000", "relay example:1", "a:b"}) {
      CHECK_FALSE(DecodeLiveInvite(Code(Payload(invite, address))));
    }
  }
}

TEST_CASE("Live invite secrets are wiped storage end to end", "[relay][live_invite]") {
  auto invite = Make();
  auto code = EncodeLiveInvite(invite);
  REQUIRE(code);
  auto decoded = DecodeLiveInvite(*code);
  REQUIRE(decoded);
  CHECK(decoded->token_secret == Secret(0x40));
  LiveInvite moved = std::move(*decoded);
  CHECK(moved.token_secret == Secret(0x40));
  // A moved-from secret is wiped.
  CHECK(decoded->token_secret == TokenSecret{});
}
