#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>

#include "core/relay_core.h"

using namespace rex::relay;

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
void Length(Bytes& bytes) {
  bytes[6] = static_cast<uint8_t>(bytes.size());
  bytes[7] = static_cast<uint8_t>(bytes.size() >> 8);
}
uint64_t Drops(const Core& core, Drop reason) {
  return core.counters().drops[static_cast<size_t>(reason)];
}
uint64_t ConnectDrops(const Core& core, ConnectDrop reason) {
  return core.counters().connect_drops[static_cast<size_t>(reason)];
}
std::span<const uint8_t> Raw(std::string_view text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

constexpr Endpoint kA{0x0100007f, 1000}, kB{0x0100007f, 1001}, kC{0x0200007f, 1000};

struct Member {
  Endpoint endpoint;
  uint64_t token = 0;
  uint32_t ipv4 = 0;
  uint32_t id = 0;
  Digest assoc{};
};

struct Fixture {
  Core core{{}, "secret", Digest{42}};
  uint64_t now = 0;
  std::vector<Datagram> Input(Endpoint endpoint, const Message& message) {
    return core.Input(endpoint, Wire(message), now);
  }
  Message One(Endpoint endpoint, const Message& message) {
    auto output = Input(endpoint, message);
    REQUIRE(output.size() == 1);
    return Read(output[0].bytes);
  }
  Member Join(Endpoint endpoint, std::string room = "room") {
    Message hello;
    hello.type = Type::kHello;
    hello.room = room;
    hello.nonce.fill(static_cast<uint8_t>(endpoint.port + endpoint.ipv4));
    const auto challenge = One(endpoint, hello);
    Message join;
    join.type = Type::kJoin;
    join.auth = JoinAuth("secret", hello.nonce, challenge.nonce, room, {});
    const auto welcome = One(endpoint, join);
    REQUIRE(welcome.type == Type::kWelcome);
    return {endpoint, welcome.token, welcome.ipv4, welcome.peer_id,
            AssociationKey(Raw("secret"), hello.nonce, challenge.nonce, welcome.peer_id)};
  }
  void Candidates(const Member& member, std::vector<Candidate> candidates) {
    Message message;
    message.type = Type::kCandidates;
    message.token = member.token;
    message.candidates = std::move(candidates);
    REQUIRE(SignConnect(member.assoc, message));
    CHECK(Input(member.endpoint, message).empty());
  }
  std::vector<Datagram> Request(const Member& member, uint32_t target) {
    Message message;
    message.type = Type::kConnectRequest;
    message.token = member.token;
    message.ipv4 = target;
    REQUIRE(SignConnect(member.assoc, message));
    return Input(member.endpoint, message);
  }
  // Returns the CONNECT_INFO addressed to each member, requester first.
  std::pair<Message, Message> Connect(const Member& from, const Member& to) {
    auto output = Request(from, to.ipv4);
    REQUIRE(output.size() == 2);
    CHECK(output[0].endpoint == from.endpoint);
    CHECK(output[1].endpoint == to.endpoint);
    return {Read(output[0].bytes), Read(output[1].bytes)};
  }
};

Message Info() {
  Message message;
  message.type = Type::kConnectInfo;
  message.attempt_id = 0x01020304;
  message.ipv4 = 0x020012c6;
  message.observed = {0x0100007f, 30100};
  message.candidates = {{0x0501a8c0, 30100}, {0x0100007f, 30100}};
  message.wrapped_key.fill(0xa5);
  message.key_tag.fill(0x5a);
  return message;
}
}  // namespace

TEST_CASE("relay direct messages round trip with documented sizes", "[relay][direct]") {
  Message candidates;
  candidates.type = Type::kCandidates;
  candidates.token = 7;
  Message request;
  request.type = Type::kConnectRequest;
  request.token = 7;
  request.ipv4 = 0x020012c6;
  request.key_tag.fill(4);
  Message close;
  close.type = Type::kConnectClose;
  close.attempt_id = 9;
  close.ipv4 = 0x020012c6;
  close.key_tag.fill(3);
  for (size_t count = 0; count <= kMaxCandidates; ++count) {
    candidates.candidates.assign(count, Candidate{0x0501a8c0, 1});
    candidates.key_tag.fill(static_cast<uint8_t>(count + 1));
    CHECK(Wire(candidates).size() == 33 + 6 * count);
    auto info = Info();
    info.candidates.assign(count, Candidate{0x0501a8c0, 1});
    CHECK(Wire(info).size() == 71 + 6 * count);
    for (const auto& message : {candidates, info}) {
      const auto bytes = Wire(message);
      const auto decoded = Read(bytes);
      CHECK(Wire(decoded) == bytes);
      CHECK(decoded.candidates == message.candidates);
      CHECK(decoded.key_tag == message.key_tag);
    }
  }
  CHECK(Wire(request).size() == 36);
  CHECK(Wire(close).size() == 32);
  const auto info = Read(Wire(Info()));
  CHECK(info.attempt_id == Info().attempt_id);
  CHECK(info.ipv4 == Info().ipv4);
  CHECK(info.observed == Info().observed);
  CHECK(info.wrapped_key == Info().wrapped_key);
  CHECK(info.key_tag == Info().key_tag);
  CHECK(Read(Wire(request)).ipv4 == request.ipv4);
  CHECK(Read(Wire(request)).key_tag == request.key_tag);
  CHECK(Read(Wire(close)).key_tag == close.key_tag);
  // Every truncation and any trailing byte is malformed, with or without a patched length.
  for (const auto& message : {candidates, request, Info(), close}) {
    const auto bytes = Wire(message);
    for (size_t length = 0; length < bytes.size(); ++length) {
      Bytes short_bytes(bytes.begin(), bytes.begin() + length);
      CHECK_FALSE(Decode(short_bytes));
      if (length >= kHeaderSize) {
        Length(short_bytes);
        CHECK_FALSE(Decode(short_bytes));
      }
    }
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK_FALSE(Decode(trailing));
    Length(trailing);
    CHECK_FALSE(Decode(trailing));
  }
}

TEST_CASE("relay direct message codecs reject invalid fields", "[relay][direct]") {
  Message candidates;
  candidates.type = Type::kCandidates;
  candidates.candidates.assign(kMaxCandidates + 1, Candidate{1, 1});
  CHECK_FALSE(Encode(candidates));
  candidates.candidates = {{0, 1}};
  CHECK_FALSE(Encode(candidates));
  candidates.candidates = {{1, 0}};
  CHECK_FALSE(Encode(candidates));
  auto info = Info();
  info.attempt_id = 0;
  CHECK_FALSE(Encode(info));
  info = Info();
  info.observed.port = 0;
  CHECK_FALSE(Encode(info));
  info = Info();
  info.candidates.assign(kMaxCandidates + 1, Candidate{1, 1});
  CHECK_FALSE(Encode(info));
  Message close;
  close.type = Type::kConnectClose;
  CHECK_FALSE(Encode(close));

  // Patched wire fields.
  candidates.candidates = {{1, 1}};
  auto bytes = Wire(candidates);
  SECTION("candidate count above the limit") {
    bytes[16] = kMaxCandidates + 1;
  }
  SECTION("candidate count beyond the datagram") {
    bytes[16] = 2;
  }
  SECTION("zero candidate port") {
    bytes[21] = bytes[22] = 0;
  }
  SECTION("oversized candidate list") {
    bytes[16] = kMaxCandidates + 1;
    for (size_t i = 0; i < kMaxCandidates; ++i)
      bytes.insert(bytes.end(), {1, 0, 0, 0, 1, 0});
    Length(bytes);
  }
  SECTION("zero attempt") {
    bytes = Wire(Info());
    std::fill_n(bytes.begin() + 8, 4, 0);
  }
  SECTION("info candidate count disagrees with length") {
    bytes = Wire(Info());
    bytes[22] = 3;
  }
  SECTION("zero close attempt") {
    close.attempt_id = 1;
    bytes = Wire(close);
    std::fill_n(bytes.begin() + 8, 4, 0);
  }
  CHECK_FALSE(Decode(bytes));
}

TEST_CASE("relay direct frames authenticate every byte", "[relay][direct]") {
  const auto key = DirectKey(Digest{1}, 0x020012c6);
  CHECK(key != DirectKey(Digest{1}, 0x010012c6));
  CHECK(key != DirectKey(Digest{2}, 0x020012c6));
  DirectFrame frame;
  frame.attempt_id = 5;
  frame.sender_ipv4 = 0x020012c6;
  frame.sequence = 0x0102030405060708;
  for (auto type : {DirectType::kProbe, DirectType::kProbeAck, DirectType::kData,
                    DirectType::kKeepalive, DirectType::kKeepaliveAck}) {
    CAPTURE(static_cast<int>(type));
    frame.type = type;
    frame.echo = type == DirectType::kProbeAck || type == DirectType::kKeepaliveAck ? 77 : 0;
    frame.source_port = type == DirectType::kData ? 1000 : 0;
    frame.destination_port = type == DirectType::kData ? 26900 : 0;
    frame.payload = type == DirectType::kData ? Bytes{0, 3, 1, 2, 3} : Bytes{};
    auto bytes = EncodeDirect(frame, key);
    REQUIRE(bytes);
    CHECK(bytes->size() ==
          (type == DirectType::kData ? kDirectDataOverhead + 5 : kDirectControlSize));
    CHECK(std::memcmp(bytes->data(), "RRDT", 4) == 0);
    auto parsed = ParseDirect(*bytes);
    REQUIRE(parsed);
    CHECK(parsed->type == type);
    CHECK(parsed->attempt_id == frame.attempt_id);
    CHECK(parsed->sender_ipv4 == frame.sender_ipv4);
    CHECK(parsed->sequence == frame.sequence);
    CHECK(parsed->echo == frame.echo);
    CHECK(parsed->source_port == frame.source_port);
    CHECK(parsed->destination_port == frame.destination_port);
    CHECK(parsed->payload == frame.payload);
    CHECK(VerifyDirect(*bytes, key));
    CHECK_FALSE(VerifyDirect(*bytes, DirectKey(Digest{1}, 0x010012c6)));
    for (size_t i = 0; i < bytes->size(); ++i) {
      auto changed = *bytes;
      changed[i] ^= 0x01;
      CHECK_FALSE((ParseDirect(changed) && VerifyDirect(changed, key)));
    }
    for (size_t length = 0; length < bytes->size(); ++length) {
      const Bytes short_bytes(bytes->begin(), bytes->begin() + length);
      CHECK_FALSE((ParseDirect(short_bytes) && VerifyDirect(short_bytes, key)));
    }
  }
  // Payload limit equals the relay DATA limit.
  frame.type = DirectType::kData;
  frame.echo = 0;
  frame.payload.assign(kMaxPayload, 9);
  auto largest = EncodeDirect(frame, key);
  REQUIRE(largest);
  CHECK(largest->size() == kMaxDirectDatagram);
  CHECK(ParseDirect(*largest));
  largest->push_back(0);
  CHECK_FALSE(ParseDirect(*largest));
  frame.payload.push_back(9);
  CHECK_FALSE(EncodeDirect(frame, key));
  frame.payload.clear();
  CHECK(EncodeDirect(frame, key)->size() == kDirectDataOverhead);
  // Requests carry a zero echo, acknowledgements a nonzero one; sequence and attempt are nonzero.
  frame.type = DirectType::kProbe;
  frame.echo = 1;
  CHECK_FALSE(EncodeDirect(frame, key));
  frame.type = DirectType::kProbeAck;
  frame.echo = 0;
  CHECK_FALSE(EncodeDirect(frame, key));
  frame.echo = 1;
  frame.sequence = 0;
  CHECK_FALSE(EncodeDirect(frame, key));
  frame.sequence = 1;
  frame.attempt_id = 0;
  CHECK_FALSE(EncodeDirect(frame, key));
  frame.attempt_id = 1;
  auto valid = *EncodeDirect(frame, key);
  auto patched = valid;
  patched[4] = 2;  // version
  CHECK_FALSE(ParseDirect(patched));
  patched = valid;
  patched[5] = 6;  // type
  CHECK_FALSE(ParseDirect(patched));
  patched = valid;
  std::fill_n(patched.begin() + 22, 8, 0);  // zero echo in an acknowledgement
  CHECK_FALSE(ParseDirect(patched));
  // Relay datagrams are not direct frames and vice versa.
  CHECK_FALSE(ParseDirect(Wire(Info())));
  CHECK_FALSE(Decode(valid));
}

TEST_CASE("relay direct replay window accepts each sequence once", "[relay][direct]") {
  ReplayWindow window;
  CHECK_FALSE(window.Fresh(0));
  for (uint64_t sequence : {1, 3, 2, 10}) {
    REQUIRE(window.Fresh(sequence));
    window.Accept(sequence);
    CHECK_FALSE(window.Fresh(sequence));
  }
  CHECK(window.Fresh(4));
  window.Accept(73);  // 10 is now 63 behind and still tracked
  CHECK_FALSE(window.Fresh(10));
  CHECK(window.Fresh(11));
  CHECK_FALSE(window.Fresh(9));  // 64 behind: older than the window
  window.Accept(1000);
  CHECK_FALSE(window.Fresh(73));
  CHECK_FALSE(window.Fresh(936));
  CHECK(window.Fresh(937));
  CHECK(window.Fresh(1001));
}

TEST_CASE("relay CONNECT_INFO wraps one pair key under each member's K_assoc", "[relay][direct]") {
  Fixture f;
  const auto a = f.Join(kA), b = f.Join(kB);
  const std::vector<Candidate> a_local{{0x0501a8c0, kA.port}, {0x0100007f, kA.port}};
  f.Candidates(a, a_local);
  f.Candidates(b, {});
  const auto [to_a, to_b] = f.Connect(a, b);
  // CONNECT_INFO exceeds its request but goes only to authenticated members.
  CHECK(Wire(to_a).size() > 20);
  CHECK(to_a.attempt_id == to_b.attempt_id);
  CHECK(to_a.ipv4 == b.ipv4);
  CHECK(to_b.ipv4 == a.ipv4);
  CHECK(to_a.observed == Candidate{kB.ipv4, kB.port});
  CHECK(to_b.observed == Candidate{kA.ipv4, kA.port});
  CHECK(to_a.candidates.empty());
  CHECK(to_b.candidates == a_local);
  const auto key_a = UnwrapPairKey(a.assoc, to_a);
  const auto key_b = UnwrapPairKey(b.assoc, to_b);
  REQUIRE(key_a);
  REQUIRE(key_b);
  CHECK(*key_a == *key_b);
  CHECK(to_a.wrapped_key != to_b.wrapped_key);
  // Wrong key, attempt, address, endpoint, candidate, tag or ciphertext fails the tag.
  CHECK_FALSE(UnwrapPairKey(b.assoc, to_a));
  const auto tampered = [&](auto change) {
    auto copy = to_b;
    change(copy);
    return UnwrapPairKey(b.assoc, copy).has_value();
  };
  CHECK_FALSE(tampered([](Message& m) { ++m.attempt_id; }));
  CHECK_FALSE(tampered([&](Message& m) { m.ipv4 = b.ipv4; }));
  CHECK_FALSE(tampered([](Message& m) { m.observed.ipv4 ^= 0x01000000; }));
  CHECK_FALSE(tampered([](Message& m) { ++m.observed.port; }));
  CHECK_FALSE(tampered([](Message& m) { m.candidates[0].ipv4 ^= 0x01000000; }));
  CHECK_FALSE(tampered([](Message& m) { ++m.candidates[1].port; }));
  CHECK_FALSE(tampered([](Message& m) { std::swap(m.candidates[0], m.candidates[1]); }));
  CHECK_FALSE(tampered([](Message& m) { m.candidates.pop_back(); }));
  CHECK_FALSE(tampered([](Message& m) { m.candidates.clear(); }));
  CHECK_FALSE(tampered([](Message& m) { m.key_tag[15] ^= 1; }));
  CHECK_FALSE(tampered([](Message& m) { m.wrapped_key[0] ^= 1; }));
  CHECK(tampered([](Message&) {}));
  CHECK(f.core.counters().attempts == 1);

  // Either member's request within 10 seconds repeats the attempt.
  f.now = kAttemptReuseMs - 1;
  const auto [again_b, again_a] = f.Connect(b, a);
  CHECK(again_a.attempt_id == to_a.attempt_id);
  CHECK(again_a.wrapped_key == to_a.wrapped_key);
  CHECK(again_b.wrapped_key == to_b.wrapped_key);
  f.now = kAttemptReuseMs;
  const auto [fresh_a, fresh_b] = f.Connect(a, b);
  CHECK(fresh_a.attempt_id != to_a.attempt_id);
  const auto fresh_key = UnwrapPairKey(a.assoc, fresh_a);
  REQUIRE(fresh_key);
  CHECK_FALSE(*fresh_key == *key_a);
  CHECK(f.core.counters().attempts == 1);
}

TEST_CASE("relay K_assoc for a token join uses the token secret", "[relay][direct]") {
  Fixture f;
  const auto host = f.Join(kA);
  Message publish;
  publish.type = Type::kPublish;
  publish.token = host.token;
  publish.request_id = 1;
  publish.sequence = 1;
  publish.record.session_id = 1;
  publish.record.max_public = 4;
  REQUIRE(f.One(kA, publish).status == Status::kOk);
  Message request;
  request.type = Type::kTokenRequest;
  request.token = host.token;
  request.request_id = 2;
  request.session_id = 1;
  const auto grant = f.One(kA, request);
  REQUIRE(grant.type == Type::kTokenGrant);
  Message hello;
  hello.type = Type::kHello;
  hello.token_join = true;
  hello.join_token = grant.join_token;
  hello.nonce.fill(0x33);
  const auto challenge = f.One(kC, hello);
  Message join;
  join.type = Type::kJoin;
  join.auth =
      TokenJoinAuth(grant.token_secret.bytes(), hello.nonce, challenge.nonce, grant.join_token, {});
  const auto welcome = f.One(kC, join);
  REQUIRE(welcome.type == Type::kWelcome);
  const Member guest{
      kC, welcome.token, welcome.ipv4, welcome.peer_id,
      AssociationKey(grant.token_secret.bytes(), hello.nonce, challenge.nonce, welcome.peer_id)};
  f.Candidates(host, {});
  const auto [to_guest, to_host] = f.Connect(guest, host);
  CHECK(UnwrapPairKey(guest.assoc, to_guest));
  CHECK(UnwrapPairKey(host.assoc, to_host));
  // The room secret does not derive a token member's K_assoc.
  CHECK_FALSE(UnwrapPairKey(
      AssociationKey(Raw("secret"), hello.nonce, challenge.nonce, welcome.peer_id), to_guest));
}

TEST_CASE("relay answers CONNECT_REQUEST only for direct-capable room members", "[relay][direct]") {
  Fixture f;
  const auto a = f.Join(kA), b = f.Join(kB), other = f.Join(kC, "other");
  f.Candidates(other, {});
  // b has not announced candidates, so it has not enabled direct paths.
  CHECK(f.Request(a, b.ipv4).empty());
  CHECK(f.Request(a, a.ipv4).empty());
  CHECK(f.Request(a, 0x090012c6).empty());
  CHECK(f.Request(a, other.ipv4).empty());  // another room, even at a reused address
  CHECK(ConnectDrops(f.core, ConnectDrop::kUnavailable) == 4);
  CHECK(f.core.counters().attempts == 0);
  // A requester becomes direct-capable without CANDIDATES.
  const auto [to_b, to_a] = f.Connect(b, a);
  CHECK(to_a.candidates.empty());
  // Wrong token, wrong endpoint and the relay-to-client direction are dropped.
  Message request;
  request.type = Type::kConnectRequest;
  request.token = a.token + 1;
  request.ipv4 = b.ipv4;
  CHECK(f.Input(kA, request).empty());
  CHECK(Drops(f.core, Drop::kAuth) == 1);
  CHECK(f.Input(kA, Info()).empty());
  CHECK(Drops(f.core, Drop::kDirection) == 1);
}

TEST_CASE("relay drops CANDIDATES and CONNECT_REQUEST without a valid K_assoc tag",
          "[relay][direct]") {
  Fixture f;
  const auto a = f.Join(kA), b = f.Join(kB), c = f.Join(kC);
  f.Candidates(a, {{3, 3}});
  f.Candidates(b, {});
  auto auth_drops = [&] { return ConnectDrops(f.core, ConnectDrop::kAuth); };
  // Valid session token, but signed under another member's K_assoc or with a flipped tag.
  Message candidates;
  candidates.type = Type::kCandidates;
  candidates.token = a.token;
  candidates.candidates = {{9, 9}};
  REQUIRE(SignConnect(b.assoc, candidates));
  CHECK(f.Input(kA, candidates).empty());
  REQUIRE(SignConnect(a.assoc, candidates));
  auto flipped = Wire(candidates);
  flipped.back() ^= 1;
  CHECK(f.core.Input(kA, flipped, f.now).empty());
  // c never sent a valid CANDIDATES, so a forged one must not make it direct-capable.
  Message for_c = candidates;
  for_c.token = c.token;
  REQUIRE(SignConnect(a.assoc, for_c));
  CHECK(f.Input(kC, for_c).empty());
  Message request;
  request.type = Type::kConnectRequest;
  request.token = a.token;
  request.ipv4 = b.ipv4;
  CHECK(f.Input(kA, request).empty());  // zero tag
  REQUIRE(SignConnect(b.assoc, request));
  CHECK(f.Input(kA, request).empty());
  CHECK(auth_drops() == 5);
  CHECK(f.core.counters().attempts == 0);
  CHECK(ConnectDrops(f.core, ConnectDrop::kRateLimited) == 0);
  // Stored candidates are unchanged, c is still unavailable, and the forged
  // requests did not drain a's connect bucket.
  CHECK(f.Request(b, c.ipv4).empty());
  CHECK(ConnectDrops(f.core, ConnectDrop::kUnavailable) == 1);
  for (uint32_t i = 1; i < kConnectsPerSecond; ++i)
    CHECK(f.Connect(a, b).second.candidates == std::vector<Candidate>{{3, 3}});
  CHECK(ConnectDrops(f.core, ConnectDrop::kRateLimited) == 0);
}

TEST_CASE("relay rate limits CONNECT_REQUEST and bounds stored candidates", "[relay][direct]") {
  Fixture f;
  const auto a = f.Join(kA), b = f.Join(kB);
  f.Candidates(b, {});
  // CANDIDATES and CONNECT_REQUEST share a bucket of kConnectsPerSecond with that burst;
  // b's CANDIDATES used one of its own tokens, not a's.
  for (uint32_t i = 0; i < kConnectsPerSecond; ++i)
    CHECK(f.Request(a, b.ipv4).size() == 2);
  CHECK(f.Request(a, b.ipv4).empty());
  CHECK(ConnectDrops(f.core, ConnectDrop::kRateLimited) == 1);
  f.now += 1000 / kConnectsPerSecond;
  CHECK(f.Request(a, b.ipv4).size() == 2);

  // Newer candidates replace older ones; more than four never decode.
  f.now += 1000;
  f.Candidates(a, {{1, 1}, {2, 2}});
  f.Candidates(a, {{3, 3}});
  CHECK(f.Connect(b, a).first.candidates == std::vector<Candidate>{{3, 3}});
  Message too_many;
  too_many.type = Type::kCandidates;
  too_many.token = a.token;
  too_many.candidates = {{1, 1}};
  auto bytes = Wire(too_many);
  bytes[16] = kMaxCandidates + 1;
  for (size_t i = 0; i < kMaxCandidates; ++i)
    bytes.insert(bytes.end(), {1, 0, 0, 0, 1, 0});
  Length(bytes);
  const auto malformed = Drops(f.core, Drop::kMalformed);
  CHECK(f.core.Input(kA, bytes, f.now).empty());
  CHECK(Drops(f.core, Drop::kMalformed) == malformed + 1);
  CHECK(f.Connect(b, a).first.candidates == std::vector<Candidate>{{3, 3}});
}

TEST_CASE("relay sends CONNECT_CLOSE when a paired member leaves", "[relay][direct]") {
  Fixture f;
  auto a = f.Join(kA), b = f.Join(kB), c = f.Join(kC);
  f.Candidates(a, {});
  f.Candidates(b, {});
  f.Candidates(c, {});
  const auto ab = f.Connect(a, b).first.attempt_id;
  const auto ac = f.Connect(a, c).first.attempt_id;
  REQUIRE(f.core.counters().attempts == 2);
  // BYE: the reply to the BYE carries the notice to the other member.
  Message bye;
  bye.type = Type::kBye;
  bye.token = b.token;
  auto output = f.Input(kB, bye);
  REQUIRE(output.size() == 1);
  CHECK(output[0].endpoint == kA);
  auto close = Read(output[0].bytes);
  CHECK(close.type == Type::kConnectClose);
  CHECK(close.attempt_id == ab);
  CHECK(close.ipv4 == b.ipv4);
  CHECK(close.key_tag == CloseTag(a.assoc, ab, b.ipv4));
  CHECK(close.key_tag != CloseTag(c.assoc, ab, b.ipv4));
  CHECK(f.core.counters().attempts == 1);
  // Idle expiry: Tick produces the notice for the transport to send.
  Message heartbeat;
  heartbeat.type = Type::kHeartbeat;
  heartbeat.token = a.token;
  f.now = 14000;
  REQUIRE(f.Input(kA, heartbeat).size() == 1);
  f.core.Tick(15000);
  const auto notices = f.core.TakeNotices();
  REQUIRE(notices.size() == 1);
  CHECK(notices[0].endpoint == kA);
  close = Read(notices[0].bytes);
  CHECK(close.attempt_id == ac);
  CHECK(close.ipv4 == c.ipv4);
  CHECK(close.key_tag == CloseTag(a.assoc, ac, c.ipv4));
  CHECK(f.core.TakeNotices().empty());
  CHECK(f.core.counters().attempts == 0);
}
