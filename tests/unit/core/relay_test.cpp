#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <set>

#include <rex/cvar.h>
#include <rex/system/xam/user_profile.h>

#include "core/relay_core.h"
#include "core/relay_transport.h"
#include "rexglue/commands/relay_status.h"

namespace rex::kernel::xam {
uint32_t XamGetSystemVersion_entry();
}

using namespace rex::relay;

TEST_CASE("relay status reports player and room changes only", "[relay][relay_status]") {
  rexglue::cli::RelayStatus status;
  Counters c;
  TransportCounters t;
  auto messages = status.Update(c, t, 0, 0);
  CHECK(messages.status.empty());
  CHECK(messages.warning.empty());
  CHECK(messages.debug.empty());
  CHECK(status.Update(c, t, 0, 5000).status.empty());

  c.peers = 1;
  c.rooms = 1;
  CHECK(status.Update(c, t, 0, 5100).status == "Relay: 1 player in 1 room");
  c.peers = 2;
  CHECK(status.Update(c, t, 0, 5200).status == "Relay: 2 players in 1 room");
  c.rooms = 2;
  CHECK(status.Update(c, t, 0, 5300).status == "Relay: 2 players in 2 rooms");

  c.pending = 2;
  c.sessions = 1;
  c.tokens = 1;
  c.attempts = 1;
  c.packets_in = 100;
  c.bytes_in = 1000;
  c.packets_out = 90;
  c.bytes_out = 900;
  CHECK(status.Update(c, t, 0, 10000).status.empty());
  CHECK(status.Update(c, t, 0, 15000).status.empty());
  c.peers = 0;
  c.rooms = 0;
  CHECK(status.Update(c, t, 0, 15100).status == "Relay: 0 players in 0 rooms");
  CHECK(status.Update(c, t, 0, 20000).status.empty());
}

TEST_CASE("relay status warnings aggregate increases without flooding", "[relay][relay_status]") {
  rexglue::cli::RelayStatus status;
  Counters c;
  TransportCounters t;
  c.drops[static_cast<size_t>(Drop::kMalformed)] = 2;
  CHECK(status.Update(c, t, 0, 0).warning == "Relay problems: malformed=+2");
  c.drops[static_cast<size_t>(Drop::kMalformed)] = 5;
  c.drops[static_cast<size_t>(Drop::kAuth)] = 3;
  t.send_errors = 1;
  c.peers = c.rooms = 1;
  auto messages = status.Update(c, t, 0, 100);
  CHECK(messages.warning.empty());
  CHECK(messages.status == "Relay: 1 player in 1 room");
  c.drops[static_cast<size_t>(Drop::kMalformed)] = 10;
  CHECK(status.Update(c, t, 0, 4999).warning.empty());
  CHECK(status.Update(c, t, 0, 5000).warning ==
        "Relay problems: send_errors=+1 malformed=+8 auth=+3");
  CHECK(status.Update(c, t, 0, 10000).warning.empty());
  CHECK(status.Update(c, t, 0, 15000).warning.empty());
  c.drops[static_cast<size_t>(Drop::kAuth)] = 4;
  CHECK(status.Update(c, t, 0, 15100).warning == "Relay problems: auth=+1");
  CHECK(status.Update(c, t, 0, 20100).warning.empty());
}

TEST_CASE("relay status names every problem counter", "[relay][relay_status]") {
  constexpr std::array<std::string_view, 16> kDrops{
      "version",          "malformed",         "auth",         "room_full",
      "too_many_rooms",   "fingerprint",       "rate_limited", "destination",
      "direction",        "expired_challenge", "idle",         "directory_rate_limited",
      "session_conflict", "session_limit",     "token_denied", "token_limit"};
  constexpr std::array<std::string_view, 4> kConnectDrops{
      "connect_rate_limited", "connect_unavailable", "connect_limit", "connect_auth"};
  for (size_t i = 0; i < kDrops.size(); ++i) {
    CAPTURE(kDrops[i]);
    rexglue::cli::RelayStatus status;
    Counters c;
    c.drops[i] = 7;
    const auto messages = status.Update(c, {}, 0, 0);
    if (i == static_cast<size_t>(Drop::kIdle))
      CHECK(messages.warning.empty());
    else
      CHECK(messages.warning == "Relay problems: " + std::string(kDrops[i]) + "=+7");
    CHECK(messages.debug == "Relay counters: " + std::string(kDrops[i]) + "=7");
  }
  for (size_t i = 0; i < kConnectDrops.size(); ++i) {
    CAPTURE(kConnectDrops[i]);
    rexglue::cli::RelayStatus status;
    Counters c;
    c.connect_drops[i] = 9;
    const auto messages = status.Update(c, {}, 0, 0);
    CHECK(messages.warning == "Relay problems: " + std::string(kConnectDrops[i]) + "=+9");
    CHECK(messages.debug == "Relay counters: " + std::string(kConnectDrops[i]) + "=9");
  }
  rexglue::cli::RelayStatus status;
  CHECK(status.Update({}, {2, 3}, 4, 0).warning ==
        "Relay problems: receive_errors=+2 send_errors=+3 connection_resets=+4");
}

TEST_CASE("relay status debug dumps nonzero counters every five seconds", "[relay][relay_status]") {
  rexglue::cli::RelayStatus status;
  CHECK(status.Update({}, {}, 0, 0).debug.empty());
  Counters c;
  c.rooms = 1;
  c.peers = 2;
  c.pending = 3;
  c.sessions = 4;
  c.tokens = 5;
  c.attempts = 6;
  c.packets_in = 7;
  c.bytes_in = 8;
  c.packets_out = 9;
  c.bytes_out = 10;
  CHECK(status.Update(c, {}, 0, 4999).debug.empty());
  CHECK(status.Update(c, {11, 12}, 13, 5000).debug ==
        "Relay counters: rooms=1 peers=2 pending=3 sessions=4 tokens=5 attempts=6 packets_in=7 "
        "bytes_in=8 packets_out=9 bytes_out=10 receive_errors=11 send_errors=12 "
        "connection_resets=13");
  CHECK(status.Update(c, {}, 0, 9999).debug.empty());
  c = {};
  c.packets_in = 7;
  CHECK(status.Update(c, {}, 0, 10000).debug == "Relay counters: packets_in=7");
  CHECK(status.Update(c, {}, 0, 30000).debug == "Relay counters: packets_in=7");
  CHECK(status.Update(c, {}, 0, 30001).debug.empty());
}

TEST_CASE("relay status shutdown summarizes totals", "[relay][relay_status]") {
  rexglue::cli::RelayStatus status;
  Counters c;
  c.drops.fill(1);
  c.drops[static_cast<size_t>(Drop::kIdle)] = 100;
  c.connect_drops.fill(2);
  c.packets_in = 50;
  c.bytes_in = 5000;
  c.packets_out = 40;
  c.bytes_out = 4000;
  status.Update(c, {}, 0, 0);
  ++c.drops[static_cast<size_t>(Drop::kMalformed)];
  CHECK(status.Update(c, {}, 0, 100).warning.empty());
  CHECK(rexglue::cli::RelayStatus::Summary(c, {2, 3}, 4) ==
        "Relay stopped: packets_in=50 bytes_in=5000 packets_out=40 bytes_out=4000 rejected=24 "
        "receive_errors=2 send_errors=3 connection_resets=4");
  CHECK(rexglue::cli::RelayStatus::Summary({}, {}, 0) ==
        "Relay stopped: packets_in=0 bytes_in=0 packets_out=0 bytes_out=0 rejected=0 "
        "receive_errors=0 send_errors=0 connection_resets=0");
}

namespace {
Bytes Wire(const Message& message) {
  auto bytes = Encode(message);
  REQUIRE(bytes);
  return *bytes;
}
Message Read(const Bytes& bytes) {
  auto message = Decode(bytes);
  REQUIRE(message);
  return *message;
}
std::string Hex(const Digest& digest) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  for (const auto byte : digest) {
    text += digits[byte >> 4];
    text += digits[byte & 15];
  }
  return text;
}
Bytes Raw(std::string_view text) {
  return Bytes(text.begin(), text.end());
}

struct Fixture {
  Core core;
  uint64_t now = 0;
  explicit Fixture(Config config = {}) : core(config, "secret", Digest{42}) {}
  std::vector<Datagram> Input(Endpoint endpoint, const Message& message) {
    auto input = Wire(message);
    auto output = core.Input(endpoint, input, now);
    REQUIRE(output.size() <= kMaxMembers);
    for (const auto& datagram : output)
      REQUIRE(datagram.bytes.size() <= input.size());
    return output;
  }
  Message Begin(Endpoint endpoint, Message hello, std::string_view secret = "secret") {
    hello.type = Type::kHello;
    if (hello.room.empty())
      hello.room = "room";
    auto challenge = Input(endpoint, hello);
    REQUIRE(challenge.size() == 1);
    REQUIRE(Read(challenge[0].bytes).type == Type::kChallenge);
    Message join;
    join.type = Type::kJoin;
    join.auth = JoinAuth(secret, hello.nonce, Read(challenge[0].bytes).nonce, hello.room,
                         hello.fingerprint);
    return join;
  }
  Message Join(Endpoint endpoint, std::string room = "room", Digest fingerprint = {}) {
    Message hello;
    hello.room = room;
    hello.fingerprint = fingerprint;
    auto response = Input(endpoint, Begin(endpoint, hello));
    REQUIRE(response.size() == 1);
    return Read(response[0].bytes);
  }
};
constexpr Endpoint kA{1, 1000}, kB{1, 1001}, kC{2, 1000}, kD{3, 1000};
Message Data(uint64_t token, uint32_t destination = 0xffffffff) {
  Message message;
  message.type = Type::kData;
  message.token = token;
  message.ipv4 = destination;
  message.source_port = 26900;
  message.destination_port = 27000;
  message.payload = {0, 1, 2, 255};
  return message;
}
Message Control(Type type, uint64_t token) {
  Message message;
  message.type = type;
  message.token = token;
  return message;
}
uint64_t Drops(const Core& core, Drop reason) {
  return core.counters().drops[static_cast<size_t>(reason)];
}
void Length(Bytes& bytes) {
  bytes[6] = static_cast<uint8_t>(bytes.size());
  bytes[7] = static_cast<uint8_t>(bytes.size() >> 8);
}
}  // namespace

TEST_CASE("relay codec round trips every message", "[relay]") {
  for (uint8_t type = 1; type <= 10; ++type) {
    CAPTURE(type);
    Message message;
    message.type = static_cast<Type>(type);
    message.nonce.fill(0xa5);
    message.room = "test";
    message.fingerprint.fill(0xb6);
    message.auth.fill(0xc7);
    message.peer_id = 0x12345678;
    message.token = 0x0102030405060708;
    message.ipv4 = 0x010012c6;
    message.source_port = 0x1234;
    message.destination_port = 0x5678;
    message.broadcast = true;
    message.reason = Reason::kAuth;
    message.payload = {0, 7, 255};
    const auto bytes = Wire(message);
    CHECK(Wire(Read(bytes)) == bytes);
    CHECK(bytes[4] == kProtocolVersion);
    CHECK(bytes[5] == type);
    if (message.type == Type::kWelcome) {
      CHECK(Bytes(bytes.begin() + 8, bytes.end()) ==
            Bytes{0x78, 0x56, 0x34, 0x12, 8, 7, 6, 5, 4, 3, 2, 1, 0xc6, 0x12, 0, 1});
    }
    for (size_t length = 0; length < bytes.size(); ++length) {
      Bytes short_bytes(bytes.begin(), bytes.begin() + length);
      CHECK_FALSE(Decode(short_bytes));
      if (length >= kHeaderSize) {
        Length(short_bytes);
        if (message.type != Type::kData && message.type != Type::kDeliver)
          CHECK_FALSE(Decode(short_bytes));
      }
    }
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK_FALSE(Decode(trailing));
  }
  Message hello;
  hello.room = "a";
  CHECK(Wire(hello).size() >= Wire(Control(Type::kChallenge, 0)).size());
}

TEST_CASE("relay codec rejects malformed bounds and headers", "[relay]") {
  auto bytes = Wire(Data(1));
  SECTION("magic") {
    bytes[0] = 'X';
  }
  SECTION("version") {
    bytes[4] = 1;
    REQUIRE_FALSE(Decode(bytes));
    CHECK(Decode(bytes).error() == Reason::kVersion);
  }
  SECTION("type") {
    bytes[5] = 0xff;
  }
  SECTION("length") {
    ++bytes[6];
  }
  SECTION("oversize") {
    bytes.resize(kMaxDatagram + 1);
    Length(bytes);
  }
  SECTION("short data with matching length") {
    bytes.resize(23);
    Length(bytes);
  }
  SECTION("hello room length") {
    Message hello;
    hello.room = "room";
    bytes = Wire(hello);
    bytes[24] = 0;
  }
  SECTION("deliver flag") {
    auto delivered = Data(1);
    delivered.type = Type::kDeliver;
    bytes = Wire(delivered);
    bytes[16] = 2;
  }
  SECTION("reject reason") {
    bytes = Wire(Control(Type::kReject, 0));
    bytes[8] = 8;
  }
  SECTION("fixed size mismatch") {
    bytes = Wire(Control(Type::kBye, 1));
    bytes.push_back(0);
    Length(bytes);
  }
  CHECK_FALSE(Decode(bytes));
  Message hello;
  CHECK_FALSE(Encode(hello));
  hello.room.assign(33, 'x');
  CHECK_FALSE(Encode(hello));
  hello.room.assign(32, 'x');
  CHECK(Decode(Wire(hello)));
  auto data = Data(1);
  data.payload.resize(kMaxPayload + 1);
  CHECK_FALSE(Encode(data));
  data.type = Type::kDeliver;
  CHECK_FALSE(Encode(data));
}

TEST_CASE("relay HMAC RFC 4231 cases 1 and 2", "[relay]") {
  // RFC 4231 sections 4.2 and 4.3.
  CHECK(Hex(HmacSha256(Bytes(20, 0x0b), Raw("Hi There"))) ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  CHECK(Hex(HmacSha256(Raw("Jefe"), Raw("what do ya want for nothing?"))) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  const Digest zero{};
  CHECK(ConstantTimeEqual(zero, zero));
  for (size_t i = 0; i < zero.size(); ++i) {
    auto changed = zero;
    changed[i] = 1;
    CHECK_FALSE(ConstantTimeEqual(zero, changed));
  }
}

TEST_CASE("relay HMAC RFC 4231 cases 6 and 7", "[relay]") {
  // https://www.rfc-editor.org/rfc/rfc4231.html#section-4.7 and section 4.8.
  const Bytes key(131, 0xaa);
  CHECK(Hex(HmacSha256(key, Raw("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  CHECK(Hex(HmacSha256(key, Raw("This is a test using a larger than block-size key and a larger "
                                "than block-size data. The key needs to be hashed before being "
                                "used by the HMAC algorithm."))) ==
        "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

TEST_CASE("relay handshake succeeds deterministically", "[relay]") {
  Fixture fixture, same;
  const auto first = fixture.Join(kA);
  CHECK(first.type == Type::kWelcome);
  CHECK(first.token != 0);
  CHECK(first.peer_id != 0);
  CHECK(Wire(first) == Wire(same.Join(kA)));
  const auto second = fixture.Join(kB);
  CHECK(first.ipv4 != second.ipv4);
  CHECK(first.token != second.token);
  const auto bytes = Wire(first);
  CHECK(bytes[20] == 198);
  CHECK(bytes[21] == 18);
  CHECK(fixture.core.counters().rooms == 1);
  CHECK(fixture.core.counters().peers == 2);
  CHECK(fixture.core.counters().pending == 0);
}

TEST_CASE("relay JOIN retries recover the same WELCOME until confirmed or expired", "[relay]") {
  Fixture fixture;
  fixture.now = 100;
  const auto join = fixture.Begin(kA, {});
  const auto first = fixture.Input(kA, join);
  REQUIRE(first.size() == 1);
  const auto welcome = Read(first[0].bytes);
  REQUIRE(welcome.type == Type::kWelcome);
  fixture.now = 1600;
  const auto retry = fixture.Input(kA, join);
  REQUIRE(retry.size() == 1);
  CHECK(retry[0].bytes == first[0].bytes);
  CHECK(fixture.core.counters().peers == 1);
  CHECK(fixture.core.counters().rooms == 1);
  CHECK(fixture.core.counters().pending == 0);
  CHECK(Drops(fixture.core, Drop::kAuth) == 0);

  SECTION("DATA confirms receipt") {
    fixture.Input(kA, Data(welcome.token));
  }
  SECTION("HEARTBEAT confirms receipt") {
    fixture.Input(kA, Control(Type::kHeartbeat, welcome.token));
  }
  SECTION("expiry is fixed at five seconds after JOIN") {
    fixture.now = 5099;
    const auto last = fixture.Input(kA, join);
    REQUIRE(last.size() == 1);
    CHECK(last[0].bytes == first[0].bytes);
    fixture.now = 5100;
  }
  const auto replay = fixture.Input(kA, join);
  REQUIRE(replay.size() == 1);
  CHECK(Read(replay[0].bytes).reason == Reason::kAuth);
  CHECK(fixture.core.counters().peers == 1);
  CHECK(fixture.core.counters().pending == 0);
  CHECK(Drops(fixture.core, Drop::kAuth) == 1);
}

TEST_CASE("relay retained WELCOME requires the original endpoint and proof", "[relay]") {
  Fixture fixture;
  const auto join = fixture.Begin(kA, {});
  const auto first = fixture.Input(kA, join);
  REQUIRE(first.size() == 1);
  const auto welcome = Read(first[0].bytes);
  CHECK(Read(fixture.Input(kB, join)[0].bytes).reason == Reason::kAuth);
  auto wrong = join;
  wrong.auth[0] ^= 1;
  CHECK(Read(fixture.Input(kA, wrong)[0].bytes).reason == Reason::kAuth);
  fixture.Input(kA, Data(welcome.token ^ 1));
  CHECK(fixture.Input(kA, join)[0].bytes == first[0].bytes);
  CHECK(fixture.core.counters().peers == 1);
}

TEST_CASE("relay client restart from a new endpoint leaves no phantom member", "[relay]") {
  Fixture fixture(Config{1, 2});
  const auto other = fixture.Join(kB);
  const auto join = fixture.Begin(kA, {});
  const auto first = fixture.Input(kA, join);
  REQUIRE(first.size() == 1);
  const auto old = Read(first[0].bytes);
  REQUIRE(old.type == Type::kWelcome);
  CHECK(fixture.Input(kA, join)[0].bytes == first[0].bytes);
  fixture.Input(kA, Control(Type::kBye, old.token));
  CHECK(Read(fixture.Input(kA, join)[0].bytes).reason == Reason::kAuth);
  fixture.now = 100;
  const auto restarted = fixture.Join(kC);
  REQUIRE(restarted.type == Type::kWelcome);
  CHECK(restarted.ipv4 == old.ipv4);
  CHECK(restarted.token != old.token);
  CHECK(fixture.core.counters().peers == 2);
  CHECK(fixture.core.counters().pending == 0);
  const auto delivered = fixture.Input(kB, Data(other.token));
  REQUIRE(delivered.size() == 1);
  CHECK(delivered[0].endpoint == kC);
}

TEST_CASE("relay rejects wrong secret and expired challenge", "[relay]") {
  Fixture fixture;
  auto join = fixture.Begin(kA, {});
  SECTION("wrong secret") {
    join = fixture.Begin(kA, {}, "wrong");
    CHECK(Read(fixture.Input(kA, join)[0].bytes).reason == Reason::kAuth);
    CHECK(fixture.core.counters().pending == 1);
    CHECK(Read(fixture.Input(kA, join)[0].bytes).reason == Reason::kAuth);
  }
  SECTION("expired challenge") {
    fixture.now = 5000;
  }
  auto result = fixture.Input(kA, join);
  REQUIRE(result.size() == 1);
  CHECK(Read(result[0].bytes).reason == Reason::kAuth);
  CHECK(fixture.core.counters().peers == 0);
  CHECK(fixture.core.counters().rooms == 0);
  CHECK(fixture.core.counters().pending == 0);
  CHECK(Read(fixture.Input(kA, join)[0].bytes).reason == Reason::kAuth);
}

TEST_CASE("relay bounds pending handshakes and expires them", "[relay]") {
  Fixture fixture;
  Message hello;
  hello.room = "room";
  for (uint16_t i = 0; i < kMaxPending; ++i)
    REQUIRE(Read(fixture.Input({uint32_t(i + 1), i}, hello)[0].bytes).type == Type::kChallenge);
  CHECK(fixture.core.counters().pending == 64);
  CHECK(fixture.Input(kA, hello).empty());
  CHECK(Drops(fixture.core, Drop::kRateLimited) == 1);
  fixture.core.Tick(4999);
  CHECK(fixture.core.counters().pending == 64);
  fixture.core.Tick(5000);
  CHECK(fixture.core.counters().pending == 0);
  CHECK(Read(fixture.Input(kA, hello)[0].bytes).type == Type::kChallenge);
}

TEST_CASE("relay HELLO retries retain expiry and challenge", "[relay]") {
  Fixture fixture;
  Message hello;
  hello.room = "room";
  fixture.now = 100;
  const auto first = fixture.Input(kA, hello);
  fixture.now = 5099;
  CHECK(fixture.Input(kA, hello)[0].bytes == first[0].bytes);
  SECTION("new nonce retains original expiry") {
    hello.nonce[0] = 1;
    CHECK(fixture.Input(kA, hello)[0].bytes != first[0].bytes);
  }
  fixture.core.Tick(5100);
  CHECK(fixture.core.counters().pending == 0);
  CHECK(Drops(fixture.core, Drop::kExpiredChallenge) == 1);
}

TEST_CASE("relay pending cap applies per source IPv4", "[relay]") {
  Fixture fixture;
  Message hello;
  hello.room = "room";
  for (uint16_t port = 1; port <= kMaxPendingPerIp; ++port)
    REQUIRE(fixture.Input({1, port}, hello).size() == 1);
  CHECK(fixture.Input({1, 5}, hello).empty());
  CHECK(fixture.Input({2, 5}, hello).size() == 1);
  CHECK(fixture.Input({1, 1}, hello).size() == 1);
  CHECK(fixture.core.counters().pending == 5);
  CHECK(Drops(fixture.core, Drop::kRateLimited) == 1);
}

TEST_CASE("relay failed JOIN preserves current nonce until three failures", "[relay]") {
  Fixture fixture;
  const auto stale = fixture.Begin(kA, {});
  Message hello;
  hello.nonce[0] = 1;
  auto current = fixture.Begin(kA, hello);
  for (size_t attempt = 1; attempt <= 2; ++attempt) {
    CHECK(Read(fixture.Input(kA, stale)[0].bytes).reason == Reason::kAuth);
    CHECK(fixture.core.counters().pending == 1);
    // A new HELLO cannot reset the failed JOIN budget.
    ++hello.nonce[0];
    current = fixture.Begin(kA, hello);
  }
  SECTION("current proof still succeeds") {
    CHECK(Read(fixture.Input(kA, current)[0].bytes).type == Type::kWelcome);
  }
  SECTION("third failure drops the challenge") {
    CHECK(Read(fixture.Input(kA, stale)[0].bytes).reason == Reason::kAuth);
    CHECK(fixture.core.counters().pending == 0);
    CHECK(Read(fixture.Input(kA, current)[0].bytes).reason == Reason::kAuth);
    CHECK(fixture.core.counters().peers == 0);
  }
}

TEST_CASE("relay joined endpoint can rejoin at capacity", "[relay]") {
  Fixture fixture(Config{1, 2});
  const auto old = fixture.Join(kA);
  const auto other = fixture.Join(kB);
  const auto replacement = fixture.Join(kA);
  REQUIRE(replacement.type == Type::kWelcome);
  CHECK(replacement.ipv4 == old.ipv4);
  CHECK(replacement.token != old.token);
  CHECK(replacement.peer_id != old.peer_id);
  CHECK(fixture.core.counters().peers == 2);
  CHECK(fixture.Input(kA, Data(old.token)).empty());
  CHECK(fixture.Input(kA, Data(replacement.token, other.ipv4)).size() == 1);
  CHECK(fixture.Join(kA, "other").reason == Reason::kTooManyRooms);
  CHECK(fixture.Input(kA, Data(replacement.token, other.ipv4)).size() == 1);
  fixture.Input(kB, Control(Type::kBye, other.token));
  CHECK(fixture.Join(kA, "other", Digest{1}).type == Type::kWelcome);
  CHECK(fixture.core.counters().rooms == 1);
  CHECK(fixture.core.counters().peers == 1);
}

TEST_CASE("relay backward clocks cannot refill buckets or expire state", "[relay]") {
  Fixture fixture(Config{16, 4, 1, 16});
  fixture.now = 1000;
  const auto a = fixture.Join(kA);
  fixture.Begin(kB, {});
  CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).size() == 1);
  fixture.now = 0;
  CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).empty());
  CHECK(fixture.core.counters().peers == 1);
  CHECK(fixture.core.counters().pending == 1);
  fixture.now = 2000;
  CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).size() == 1);
  fixture.core.Tick(5999);
  CHECK(fixture.core.counters().pending == 1);
  fixture.core.Tick(6000);
  CHECK(fixture.core.counters().pending == 0);
  fixture.core.Tick(16999);
  CHECK(fixture.core.counters().peers == 1);
  fixture.core.Tick(17000);
  CHECK(fixture.core.counters().peers == 0);
}

TEST_CASE("relay assigned addresses stay inside the full benchmark subnet", "[relay]") {
  Fixture fixture(Config{1, kMaxMembers});
  std::set<uint32_t> addresses;
  for (uint16_t port = 1; port <= kMaxMembers; ++port) {
    const auto peer = fixture.Join({1, port});
    REQUIRE(peer.type == Type::kWelcome);
    std::array<uint8_t, 4> octets;
    std::memcpy(octets.data(), &peer.ipv4, 4);
    const uint32_t address = (uint32_t(octets[0]) << 24) | (uint32_t(octets[1]) << 16) |
                             (uint32_t(octets[2]) << 8) | octets[3];
    CHECK(address >= 0xc6120000);
    CHECK(address <= 0xc613ffff);
    CHECK(addresses.insert(address).second);
  }
}

TEST_CASE("relay invalid magic version and malformed messages stay silent", "[relay]") {
  Fixture fixture;
  auto bytes = Wire(Data(1));
  SECTION("magic") {
    bytes[0] = 'X';
  }
  SECTION("version") {
    bytes[4] = 1;
  }
  SECTION("length") {
    ++bytes[6];
  }
  SECTION("type") {
    bytes[5] = 255;
  }
  CHECK(fixture.core.Input(kA, bytes, 0).empty());
  CHECK(fixture.core.counters().packets_out == 0);
}

TEST_CASE("relay options enable System Link profile identity", "[relay]") {
  rex::cvar::testing::ScopedLifecycleOverride lifecycle_override;
  REQUIRE(rex::cvar::SetFlagByName("system_link_lan_enabled", "false"));
  const char* option = "system_link_relay_address";
  SECTION("address only") {}
  SECTION("room only") {
    option = "system_link_relay_room";
  }
  SECTION("secret file only") {
    option = "system_link_relay_secret_file";
  }
  REQUIRE(rex::cvar::SetFlagByName(option, "test"));
  REQUIRE(rex::cvar::SetFlagByName("system_link_xuid", "12771645593269305345"));
  CHECK_FALSE(rex::cvar::SetFlagByName("system_link_profile_name", "1234567890123456"));
  REQUIRE(rex::cvar::SetFlagByName("system_link_profile_name", "123456789012345"));
  rex::system::xam::UserProfile profile;
  CHECK(profile.xuid() == 0xB13E000000000001);
  CHECK(profile.name() == "123456789012345");
  CHECK(rex::kernel::xam::XamGetSystemVersion_entry() == 0x20449700);
  REQUIRE(rex::cvar::SetFlagByName(option, ""));
  rex::system::xam::UserProfile disabled;
  CHECK(disabled.xuid() == 0xB13EBABEBABEBABE);
  CHECK(disabled.name() == "User");
  CHECK(rex::kernel::xam::XamGetSystemVersion_entry() == 0);
  REQUIRE(rex::cvar::SetFlagByName("system_link_lan_enabled", "true"));
  CHECK(rex::kernel::xam::XamGetSystemVersion_entry() == 0x20449700);
  REQUIRE(rex::cvar::SetFlagByName("system_link_lan_enabled", "false"));
  REQUIRE(rex::cvar::SetFlagByName("system_link_xuid", "0"));
  REQUIRE(rex::cvar::SetFlagByName("system_link_profile_name", ""));
}

TEST_CASE("relay enforces room capacity and fingerprints", "[relay]") {
  Fixture fixture(Config{1, 1});
  REQUIRE(fixture.Join(kA).type == Type::kWelcome);
  CHECK(fixture.Join(kB).reason == Reason::kRoomFull);
  CHECK(fixture.Join(kB, "other").reason == Reason::kTooManyRooms);
  CHECK(fixture.Join(kB, "room", Digest{1}).reason == Reason::kFingerprintMismatch);
  CHECK(fixture.core.counters().rooms == 1);
  CHECK(fixture.core.counters().peers == 1);
}

TEST_CASE("relay drops spoofed endpoint and token", "[relay]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  CHECK(fixture.Input(kC, Data(a.token)).empty());
  CHECK(fixture.Input(kA, Data(b.token)).empty());
  CHECK(fixture.Input(kA, Data(0)).empty());
  CHECK(Drops(fixture.core, Drop::kAuth) == 3);
  fixture.now = 14999;
  CHECK(fixture.Input(kA, Control(Type::kHeartbeat, b.token)).empty());
  fixture.core.Tick(15000);
  CHECK(fixture.core.counters().peers == 0);
}

TEST_CASE("relay routes broadcast and unicast within a room", "[relay]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  fixture.Join(kC);
  fixture.Join(kD, "other");
  auto outgoing = fixture.Input(kA, Data(a.token));
  REQUIRE(outgoing.size() == 2);
  for (const auto& datagram : outgoing) {
    CHECK((datagram.endpoint == kB || datagram.endpoint == kC));
    const auto delivered = Read(datagram.bytes);
    CHECK(delivered.type == Type::kDeliver);
    CHECK(delivered.ipv4 == a.ipv4);
    CHECK(delivered.source_port == 26900);
    CHECK(delivered.destination_port == 27000);
    CHECK(delivered.broadcast);
    CHECK(delivered.payload == Data(a.token).payload);
  }
  outgoing = fixture.Input(kA, Data(a.token, b.ipv4));
  REQUIRE(outgoing.size() == 1);
  CHECK(outgoing[0].endpoint == kB);
  CHECK_FALSE(Read(outgoing[0].bytes).broadcast);
  const auto other = fixture.core.counters();
  CHECK(fixture.Input(kA, Data(a.token, 0)).empty());
  CHECK(Drops(fixture.core, Drop::kDestination) == 1);
  CHECK(fixture.core.counters().bytes_out == other.bytes_out);
}

TEST_CASE("relay isolates rooms even with reused virtual addresses", "[relay]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  const auto c = fixture.Join(kC, "other");
  CHECK(c.ipv4 == a.ipv4);
  CHECK(fixture.Input(kC, Data(c.token)).empty());
  CHECK(fixture.Input(kC, Data(c.token, b.ipv4)).empty());
  CHECK(Drops(fixture.core, Drop::kDestination) == 1);
}

TEST_CASE("relay enforces payload and per peer packet and byte limits", "[relay]") {
  SECTION("payload limit") {
    Fixture fixture;
    const auto a = fixture.Join(kA);
    fixture.Join(kB);
    auto data = Data(a.token);
    data.payload.resize(kMaxPayload, 42);
    auto outgoing = fixture.Input(kA, data);
    REQUIRE(outgoing.size() == 1);
    CHECK(Read(outgoing[0].bytes).payload.size() == kMaxPayload);
    auto oversized = Wire(data);
    oversized.push_back(42);
    Length(oversized);
    const auto rejected = fixture.core.Input(kA, oversized, 0);
    CHECK(rejected.empty());
    CHECK(Drops(fixture.core, Drop::kMalformed) == 1);
  }
  SECTION("packet bucket") {
    Fixture fixture(Config{16, 4, 2, 262144});
    const auto a = fixture.Join(kA);
    const auto b = fixture.Join(kB);
    CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).size() == 1);
    CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).size() == 1);
    CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).empty());
    CHECK(fixture.Input(kB, Control(Type::kHeartbeat, b.token)).size() == 1);
    fixture.now = 500;
    CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).size() == 1);
    CHECK(Drops(fixture.core, Drop::kRateLimited) == 1);
    fixture.Input(kA, Control(Type::kBye, a.token));
    CHECK(fixture.core.counters().peers == 1);
  }
  SECTION("byte bucket") {
    Fixture fixture(Config{16, 4, 500, 32});
    const auto a = fixture.Join(kA);
    CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).size() == 1);
    CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).size() == 1);
    CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).empty());
    fixture.now = 500;
    CHECK(fixture.Input(kA, Control(Type::kHeartbeat, a.token)).size() == 1);
    CHECK(Drops(fixture.core, Drop::kRateLimited) == 1);
  }
}

TEST_CASE("relay heartbeat refreshes idle deadline and BYE reuses addresses", "[relay]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  fixture.now = 14999;
  const auto ack = fixture.Input(kA, Control(Type::kHeartbeat, a.token));
  REQUIRE(ack.size() == 1);
  CHECK(Read(ack[0].bytes).type == Type::kAck);
  CHECK(Read(ack[0].bytes).token == a.token);
  fixture.core.Tick(15000);
  CHECK(fixture.core.counters().peers == 1);
  CHECK(Drops(fixture.core, Drop::kIdle) == 1);
  fixture.now = 15000;
  const auto c = fixture.Join(kC);
  CHECK(c.ipv4 == b.ipv4);
  CHECK(fixture.Input(kA, Control(Type::kBye, a.token)).empty());
  CHECK(fixture.core.counters().peers == 1);
  const auto d = fixture.Join(kD);
  CHECK(d.ipv4 == a.ipv4);
  fixture.Input(kC, Control(Type::kBye, c.token));
  fixture.Input(kD, Control(Type::kBye, d.token));
  CHECK(fixture.core.counters().rooms == 0);
  CHECK(fixture.Join(kA, "room", Digest{99}).type == Type::kWelcome);
}

TEST_CASE("relay never enlarges a response to malformed traffic", "[relay]") {
  Fixture fixture;
  for (size_t size = 0; size <= kMaxDatagram + 1; ++size) {
    Bytes bytes(size, 0);
    const auto output = fixture.core.Input(kA, bytes, 0);
    CHECK(output.empty());
    for (const auto& datagram : output)
      CHECK(datagram.bytes.size() <= size);
  }
  for (const auto type :
       {Type::kChallenge, Type::kWelcome, Type::kReject, Type::kDeliver, Type::kAck}) {
    CHECK(fixture.Input(kA, Control(type, 1)).empty());
  }
  CHECK(Drops(fixture.core, Drop::kDirection) == 5);
}

TEST_CASE("relay blocking pump wakes on idle socket traffic", "[relay][network]") {
  auto server = UdpSocket::Open("127.0.0.1", 0);
  auto client = UdpSocket::Open("127.0.0.1", 0);
  if (!server || !client)
    SKIP("Loopback UDP sockets unavailable");
  Core core({}, "secret", Digest{42});
  TransportCounters counters;
  Message hello;
  hello.room = "room";
  const Datagram datagram{(*server)->endpoint(), Wire(hello)};
  bool sent = false;
  std::jthread sender([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sent = bool((*client)->Send(datagram));
  });
  const auto start = std::chrono::steady_clock::now();
  const bool received = Pump(**server, core, 0, counters, 2000);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  sender.join();
  REQUIRE(sent);
  REQUIRE(received);
  CHECK(elapsed < std::chrono::milliseconds(1500));
  auto response = (*client)->Receive(100);
  REQUIRE(response);
  REQUIRE(*response);
  CHECK(Read((**response).bytes).type == Type::kChallenge);
  CHECK(counters.receive_errors == 0);
  CHECK(counters.send_errors == 0);
}

TEST_CASE("relay loopback uses the CLI network loop", "[relay][network]") {
  auto server = UdpSocket::Open("127.0.0.1", 0);
  auto a = UdpSocket::Open("127.0.0.1", 0);
  auto b = UdpSocket::Open("127.0.0.1", 0);
  if (!server || !a || !b)
    SKIP("Loopback UDP sockets unavailable");
  Core core({}, "secret", Digest{42});
  TransportCounters counters;
  auto exchange = [&](UdpSocket& sender, UdpSocket& receiver, const Message& message) {
    REQUIRE(sender.Send({(*server)->endpoint(), Wire(message)}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      Pump(**server, core, 0, counters, 100);
      auto received = receiver.Receive(1);
      REQUIRE(received);
      if (*received)
        return Read((**received).bytes);
    }
    FAIL("Timed out waiting for loopback relay datagram");
    return Message{};
  };
  auto join = [&](UdpSocket& client) {
    Message hello;
    hello.room = "loopback";
    const auto challenge = exchange(client, client, hello);
    REQUIRE(challenge.type == Type::kChallenge);
    Message request;
    request.type = Type::kJoin;
    request.auth = JoinAuth("secret", hello.nonce, challenge.nonce, hello.room, hello.fingerprint);
    auto welcome = exchange(client, client, request);
    REQUIRE(welcome.type == Type::kWelcome);
    return welcome;
  };
  const auto first = join(**a);
  const auto second = join(**b);
  const auto delivered = exchange(**a, **b, Data(first.token, second.ipv4));
  CHECK(delivered.type == Type::kDeliver);
  CHECK(delivered.ipv4 == first.ipv4);
  CHECK(delivered.payload == Data(first.token).payload);
  const auto reply = exchange(**b, **a, Data(second.token));
  CHECK(reply.broadcast);
  CHECK(reply.ipv4 == second.ipv4);
  CHECK(counters.receive_errors == 0);
  CHECK(counters.send_errors == 0);
}
