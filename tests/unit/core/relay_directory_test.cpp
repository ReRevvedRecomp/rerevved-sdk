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

SessionRecord Record(uint64_t id, uint32_t flags = 0x2e, uint16_t open_public = 3) {
  SessionRecord record;
  record.session_id = id;
  record.key.fill(static_cast<uint8_t>(id));
  record.flags = flags;
  record.max_public = 4;
  record.max_private = 2;
  record.open_public = open_public;
  record.open_private = 1;
  record.properties = {1, 2, 3};
  record.contexts = {4, 5};
  record.qos = {6};
  return record;
}
SessionRecord Largest(uint64_t id) {
  auto record = Record(id);
  record.properties.assign(kMaxRecordProperties, 0x11);
  record.contexts.assign(kMaxRecordContexts, 0x22);
  record.qos.assign(kMaxRecordQos, 0x33);
  return record;
}

Message Control(Type type, uint64_t token) {
  Message message;
  message.type = type;
  message.token = token;
  return message;
}

constexpr Endpoint kA{1, 1000}, kB{1, 1001}, kC{2, 1000}, kD{3, 1000};

struct Fixture {
  Core core;
  uint64_t now = 0;
  uint32_t next_request = 0;
  explicit Fixture(Config config = {}) : core(config, "secret", Digest{42}) {}
  std::vector<Datagram> Input(Endpoint endpoint, const Message& message) {
    auto output = core.Input(endpoint, Wire(message), now);
    for (const auto& datagram : output)
      REQUIRE(datagram.bytes.size() <= kMaxDatagram);
    return output;
  }
  Message Challenge(Endpoint endpoint, Message& hello) {
    hello.type = Type::kHello;
    auto challenge = Input(endpoint, hello);
    REQUIRE(challenge.size() == 1);
    auto message = Read(challenge[0].bytes);
    REQUIRE(message.type == Type::kChallenge);
    return message;
  }
  Message Join(Endpoint endpoint, std::string room = "room", Digest fingerprint = {}) {
    Message hello;
    hello.room = room;
    hello.fingerprint = fingerprint;
    const auto challenge = Challenge(endpoint, hello);
    Message join;
    join.type = Type::kJoin;
    join.auth = JoinAuth("secret", hello.nonce, challenge.nonce, room, fingerprint);
    auto response = Input(endpoint, join);
    REQUIRE(response.size() == 1);
    return Read(response[0].bytes);
  }
  Message TokenJoin(Endpoint endpoint, const Message& grant, Digest fingerprint = {},
                    bool wrong_secret = false) {
    Message hello;
    hello.token_join = true;
    hello.join_token = grant.join_token;
    hello.fingerprint = fingerprint;
    hello.nonce[0] = static_cast<uint8_t>(endpoint.port);
    const auto challenge = Challenge(endpoint, hello);
    auto secret = grant.token_secret;
    if (wrong_secret)
      secret.data()[0] ^= 1;
    Message join;
    join.type = Type::kJoin;
    join.auth =
        TokenJoinAuth(secret.bytes(), hello.nonce, challenge.nonce, grant.join_token, fingerprint);
    auto response = Input(endpoint, join);
    REQUIRE(response.size() == 1);
    return Read(response[0].bytes);
  }
  // Returns the single reply to a directory message.
  Message One(Endpoint endpoint, Message message) {
    auto output = Input(endpoint, message);
    REQUIRE(output.size() == 1);
    CHECK(output[0].endpoint == endpoint);
    return Read(output[0].bytes);
  }
  Status Publish(Endpoint endpoint, uint64_t token, const SessionRecord& record) {
    Message message;
    message.type = Type::kPublish;
    message.token = token;
    message.request_id = ++next_request;
    message.sequence = message.request_id;
    message.record = record;
    const auto reply = One(endpoint, message);
    REQUIRE(reply.type == Type::kStatus);
    CHECK(reply.request_id == message.request_id);
    return reply.status;
  }
  Status Remove(Endpoint endpoint, uint64_t token, uint64_t session_id) {
    Message message;
    message.type = Type::kRemove;
    message.token = token;
    message.request_id = ++next_request;
    message.sequence = message.request_id;
    message.session_id = session_id;
    const auto reply = One(endpoint, message);
    REQUIRE(reply.type == Type::kStatus);
    return reply.status;
  }
  std::vector<SessionRecord> Collect(const std::vector<Datagram>& output, uint32_t request_id) {
    REQUIRE_FALSE(output.empty());
    std::vector<SessionRecord> records;
    for (size_t i = 0; i < output.size(); ++i) {
      const auto page = Read(output[i].bytes);
      REQUIRE(page.type == Type::kResults);
      CHECK(page.request_id == request_id);
      CHECK(page.page == i);
      CHECK(page.page_count == output.size());
      records.insert(records.end(), page.records.begin(), page.records.end());
    }
    return records;
  }
  std::vector<SessionRecord> Search(Endpoint endpoint, uint64_t token, uint16_t max = 200,
                                    uint32_t flags = 0) {
    Message message;
    message.type = Type::kSearch;
    message.token = token;
    message.request_id = ++next_request;
    message.max_results = max;
    message.required_flags = flags;
    return Collect(Input(endpoint, message), message.request_id);
  }
  std::vector<SessionRecord> Lookup(Endpoint endpoint, uint64_t token, uint64_t session_id) {
    Message message;
    message.type = Type::kLookup;
    message.token = token;
    message.request_id = ++next_request;
    message.session_id = session_id;
    return Collect(Input(endpoint, message), message.request_id);
  }
  Message RequestToken(Endpoint endpoint, uint64_t token, uint64_t session_id) {
    Message message;
    message.type = Type::kTokenRequest;
    message.token = token;
    message.request_id = ++next_request;
    message.session_id = session_id;
    return One(endpoint, message);
  }
};
}  // namespace

TEST_CASE("relay session record codec round trips at the size limits", "[relay][directory]") {
  for (const auto& record : {Record(1), Largest(0xffffffffffffffff)}) {
    auto bytes = EncodeRecord(record);
    REQUIRE(bytes);
    CHECK(bytes->size() == kRecordFixedSize + record.properties.size() +
                               record.contexts.size() + record.qos.size());
    auto decoded = DecodeRecord(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == record);
    for (size_t length = 0; length < bytes->size(); ++length)
      CHECK_FALSE(DecodeRecord({bytes->data(), length}));
    auto trailing = *bytes;
    trailing.push_back(0);
    CHECK_FALSE(DecodeRecord(trailing));
  }
  CHECK(EncodeRecord(Largest(1))->size() == kMaxRecordSize);
  CHECK(kMaxRecordSize == 818);

  Message publish;
  publish.type = Type::kPublish;
  publish.token = 7;
  publish.request_id = 9;
  publish.sequence = 1;
  publish.record = Largest(5);
  publish.record.host_peer_id = 3;
  publish.record.host_ipv4 = 0x010012c6;
  const auto wire = Wire(publish);
  CHECK(wire.size() == 24 + kMaxRecordSize);
  CHECK(wire.size() <= kMaxDatagram);
  CHECK(Read(wire).record == publish.record);
  CHECK(Wire(Read(wire)) == wire);
}

TEST_CASE("relay session record codec rejects every invalid field", "[relay][directory]") {
  auto record = Record(1);
  SECTION("zero session id") {
    record.session_id = 0;
  }
  SECTION("open public above max") {
    record.open_public = 5;
  }
  SECTION("open private above max") {
    record.open_private = 3;
  }
  SECTION("properties") {
    record.properties.resize(kMaxRecordProperties + 1);
  }
  SECTION("contexts") {
    record.contexts.resize(kMaxRecordContexts + 1);
  }
  SECTION("qos") {
    record.qos.resize(kMaxRecordQos + 1);
  }
  CHECK_FALSE(EncodeRecord(record));
  Message publish;
  publish.type = Type::kPublish;
  publish.sequence = 1;
  publish.record = record;
  CHECK_FALSE(Encode(publish));
  Message results;
  results.type = Type::kResults;
  results.page_count = 1;
  results.records = {record};
  CHECK_FALSE(Encode(results));
  CHECK_FALSE(EncodeResults(1, 0, results.records));
}

TEST_CASE("relay session record decode rejects patched wire fields", "[relay][directory]") {
  auto bytes = *EncodeRecord(Largest(1));
  // Offsets: id 0, key 8, flags 24, slots 28-35, host 36-43, properties length 44.
  SECTION("zero session id") {
    std::fill_n(bytes.begin(), 8, 0);
  }
  SECTION("open public above max") {
    bytes[32] = 5;
  }
  SECTION("open private above max") {
    bytes[34] = 3;
  }
  SECTION("oversize properties") {
    bytes[44] = static_cast<uint8_t>(kMaxRecordProperties + 1);
    bytes[45] = static_cast<uint8_t>((kMaxRecordProperties + 1) >> 8);
    bytes.insert(bytes.begin() + 46, 0);
  }
  SECTION("oversize contexts") {
    const size_t at = 46 + kMaxRecordProperties;
    bytes[at] = static_cast<uint8_t>(kMaxRecordContexts + 1);
    bytes.insert(bytes.begin() + at + 2, 0);
  }
  SECTION("oversize qos") {
    const size_t at = 48 + kMaxRecordProperties + kMaxRecordContexts;
    bytes[at] = static_cast<uint8_t>(kMaxRecordQos + 1);
    bytes[at + 1] = static_cast<uint8_t>((kMaxRecordQos + 1) >> 8);
    bytes.insert(bytes.begin() + at + 2, 0);
  }
  CHECK_FALSE(DecodeRecord(bytes));
}

TEST_CASE("relay directory messages round trip and reject bad framing", "[relay][directory]") {
  std::vector<Message> messages;
  Message message;
  message.token = 0x0102030405060708;
  message.request_id = 0x11223344;
  message.sequence = 0x55667788;
  message.snapshot = 0x01020304;
  message.session_id = 0x99;
  message.max_results = kMaxSearchResults;
  message.required_flags = 0x2e;
  message.record = Record(3);
  message.page = 1;
  message.page_count = 2;
  message.records = {Record(4), Largest(5)};
  message.join_token.fill(0x5a);
  std::fill_n(message.token_secret.data(), TokenSecret::kSize, 0xa5);
  message.expiry_seconds = 900;
  message.status = Status::kNotFound;
  for (const auto type : {Type::kPublish, Type::kRemove, Type::kSearch, Type::kLookup,
                          Type::kResults, Type::kTokenRequest, Type::kTokenGrant, Type::kStatus}) {
    CAPTURE(static_cast<int>(type));
    message.type = type;
    const auto bytes = Wire(message);
    CHECK(bytes[4] == kProtocolVersion);
    CHECK(Wire(Read(bytes)) == bytes);
    for (size_t length = kHeaderSize; length < bytes.size(); ++length) {
      Bytes short_bytes(bytes.begin(), bytes.begin() + length);
      Length(short_bytes);
      CHECK_FALSE(Decode(short_bytes));
    }
    auto trailing = bytes;
    trailing.push_back(0);
    Length(trailing);
    CHECK_FALSE(Decode(trailing));
    auto old = bytes;
    old[4] = 1;
    CHECK(Decode(old).error() == Reason::kVersion);
  }
  message.type = Type::kSearch;
  for (const uint16_t bad : {uint16_t{0}, uint16_t{kMaxSearchResults + 1}}) {
    message.max_results = 1;
    auto bytes = Wire(message);
    bytes[20] = static_cast<uint8_t>(bad);
    bytes[21] = static_cast<uint8_t>(bad >> 8);
    CHECK_FALSE(Decode(bytes));
    message.max_results = bad;
    CHECK_FALSE(Encode(message));
  }
  for (const auto type : {Type::kPublish, Type::kRemove}) {
    message.type = type;
    message.sequence = 1;
    auto bytes = Wire(message);
    std::fill_n(bytes.begin() + 20, 4, 0);
    CHECK_FALSE(Decode(bytes));
    message.sequence = 0;
    CHECK_FALSE(Encode(message));
    message.sequence = 1;
  }
  for (const auto type : {Type::kRemove, Type::kLookup, Type::kTokenRequest}) {
    message.type = type;
    message.session_id = 0;
    CHECK_FALSE(Encode(message));
  }
  message.type = Type::kResults;
  message.page = 2;
  CHECK_FALSE(Encode(message));
  message.page = 0;
  message.page_count = 0;
  CHECK_FALSE(Encode(message));
  message.type = Type::kStatus;
  message.status = static_cast<Status>(4);
  CHECK_FALSE(Encode(message));
  message.status = Status::kOk;
  auto status = Wire(message);
  status[12] = 4;
  CHECK_FALSE(Decode(status));
}

TEST_CASE("relay token HELLO round trips and stays distinct from room HELLO",
          "[relay][directory]") {
  Message hello;
  hello.token_join = true;
  hello.join_token.fill(0x42);
  hello.fingerprint.fill(7);
  hello.nonce.fill(9);
  const auto bytes = Wire(hello);
  CHECK(bytes.size() == 73);
  CHECK(bytes[24] == 0);
  const auto decoded = Read(bytes);
  CHECK(decoded.token_join);
  CHECK(decoded.room.empty());
  CHECK(decoded.join_token == hello.join_token);
  CHECK(decoded.fingerprint == hello.fingerprint);
  hello.room = "room";
  CHECK_FALSE(Encode(hello));
  auto room = Read(Wire([] {
    Message m;
    m.room.assign(16, 'r');
    return m;
  }()));
  CHECK_FALSE(room.token_join);
  auto shorter = bytes;
  shorter.pop_back();
  Length(shorter);
  CHECK_FALSE(Decode(shorter));
}

TEST_CASE("relay RESULTS paging fills datagrams to the size limit", "[relay][directory]") {
  const auto empty = EncodeResults(7, 0, {});
  REQUIRE(empty);
  REQUIRE(empty->size() == 1);
  CHECK((*empty)[0].size() == kResultsHeaderSize);
  const auto page = Read((*empty)[0]);
  CHECK(page.page == 0);
  CHECK(page.page_count == 1);
  CHECK(page.records.empty());

  // A largest record plus one 389-byte record fills a page exactly.
  auto filler = Record(2);
  filler.properties.assign(kMaxDatagram - kResultsHeaderSize - kMaxRecordSize - kRecordFixedSize -
                               filler.contexts.size() - filler.qos.size(),
                           1);
  std::vector<SessionRecord> exact{Largest(1), filler, Record(3)};
  auto pages = EncodeResults(8, 0, exact);
  REQUIRE(pages);
  REQUIRE(pages->size() == 2);
  CHECK((*pages)[0].size() == kMaxDatagram);
  CHECK(Read((*pages)[0]).records.size() == 2);
  CHECK(Read((*pages)[1]).records == std::vector<SessionRecord>{Record(3)});

  std::vector<SessionRecord> many;
  for (uint64_t id = 1; id <= kMaxSearchResults; ++id) {
    auto record = Record(id);
    record.properties.clear();
    record.contexts.clear();
    record.qos.clear();
    many.push_back(record);
  }
  pages = EncodeResults(9, 0, many);
  REQUIRE(pages);
  std::vector<SessionRecord> joined;
  for (size_t i = 0; i < pages->size(); ++i) {
    CHECK((*pages)[i].size() <= kMaxDatagram);
    const auto decoded = Read((*pages)[i]);
    CHECK(decoded.page == i);
    CHECK(decoded.page_count == pages->size());
    joined.insert(joined.end(), decoded.records.begin(), decoded.records.end());
  }
  // 24 fixed-size records fit in each page: 21 + 24 * 50 = 1221.
  CHECK(pages->size() == 9);
  CHECK(joined == many);

  std::vector<SessionRecord> largest(kMaxSearchResults, Largest(1));
  pages = EncodeResults(10, 0, largest);
  REQUIRE(pages);
  CHECK(pages->size() == kMaxSearchResults);
  CHECK((*pages)[0].size() == kResultsHeaderSize + kMaxRecordSize);
  largest.push_back(Largest(1));
  CHECK_FALSE(EncodeResults(11, 0, largest));
}

TEST_CASE("relay search pages a full room for a 200-result request", "[relay][directory]") {
  Fixture fixture(Config{1, kMaxMembers});
  std::vector<Message> hosts;
  for (uint16_t port = 1; port <= kMaxSessionsPerRoom / kMaxSessionsPerPeer; ++port) {
    hosts.push_back(fixture.Join({9, port}));
    for (uint64_t i = 0; i < kMaxSessionsPerPeer; ++i)
      REQUIRE(fixture.Publish({9, port}, hosts.back().token, Largest(port * 10 + i)) ==
              Status::kOk);
  }
  const auto searcher = fixture.Join(kA);
  CHECK(fixture.core.counters().sessions == kMaxSessionsPerRoom);
  Message search;
  search.type = Type::kSearch;
  search.token = searcher.token;
  search.request_id = 77;
  search.max_results = kMaxSearchResults;
  const auto output = fixture.Input(kA, search);
  REQUIRE(output.size() == kMaxSessionsPerRoom);
  const auto records = fixture.Collect(output, 77);
  REQUIRE(records.size() == kMaxSessionsPerRoom);
  for (const auto& datagram : output)
    CHECK(datagram.bytes.size() == kResultsHeaderSize + kMaxRecordSize);
  // Newest publish first.
  CHECK(records.front().session_id == 321);
  CHECK(records.back().session_id == 10);
}

TEST_CASE("relay directory isolates rooms and excludes the requester", "[relay][directory]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  const auto c = fixture.Join(kC, "other");
  REQUIRE(fixture.Publish(kA, a.token, Record(1)) == Status::kOk);
  const auto found = fixture.Search(kB, b.token);
  REQUIRE(found.size() == 1);
  CHECK(found[0].host_peer_id == a.peer_id);
  CHECK(found[0].host_ipv4 == a.ipv4);
  CHECK(fixture.Search(kA, a.token).empty());
  CHECK(fixture.Lookup(kA, a.token, 1).size() == 1);
  CHECK(fixture.Search(kC, c.token).empty());
  CHECK(fixture.Lookup(kC, c.token, 1).empty());
  // The same session id in another room is a separate session.
  REQUIRE(fixture.Publish(kC, c.token, Record(1)) == Status::kOk);
  CHECK(fixture.Lookup(kC, c.token, 1)[0].host_peer_id == c.peer_id);
  CHECK(fixture.Lookup(kB, b.token, 1)[0].host_peer_id == a.peer_id);
  CHECK(fixture.core.counters().sessions == 2);
}

TEST_CASE("relay search applies flag and open-slot filters newest first", "[relay][directory]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  const auto searcher = fixture.Join(kC);
  REQUIRE(fixture.Publish(kA, a.token, Record(1, 0x2e)) == Status::kOk);
  REQUIRE(fixture.Publish(kA, a.token, Record(2, 0x20)) == Status::kOk);
  REQUIRE(fixture.Publish(kB, b.token, Record(3, 0x2e, 0)) == Status::kOk);
  REQUIRE(fixture.Publish(kB, b.token, Record(4, 0x2f)) == Status::kOk);
  auto ids = [](const std::vector<SessionRecord>& records) {
    std::vector<uint64_t> result;
    for (const auto& record : records)
      result.push_back(record.session_id);
    return result;
  };
  CHECK(ids(fixture.Search(kC, searcher.token)) == std::vector<uint64_t>{4, 2, 1});
  CHECK(ids(fixture.Search(kC, searcher.token, 200, 0x08)) == std::vector<uint64_t>{4, 1});
  CHECK(ids(fixture.Search(kC, searcher.token, 1, 0x08)) == std::vector<uint64_t>{4});
  CHECK(fixture.Search(kC, searcher.token, 200, 0x100).empty());
  // Lookup ignores open slots; republishing moves a session to the front.
  fixture.now = 1000;
  CHECK(fixture.Lookup(kC, searcher.token, 3).size() == 1);
  REQUIRE(fixture.Publish(kA, a.token, Record(1, 0x2e)) == Status::kOk);
  CHECK(ids(fixture.Search(kC, searcher.token)) == std::vector<uint64_t>{1, 4, 2});
}

TEST_CASE("relay directory drops spoofed sources and wrong tokens", "[relay][directory]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  Message publish;
  publish.type = Type::kPublish;
  publish.sequence = 1;
  publish.record = Record(1);
  publish.token = a.token;
  CHECK(fixture.Input(kC, publish).empty());
  publish.token = b.token;
  CHECK(fixture.Input(kA, publish).empty());
  Message search;
  search.type = Type::kSearch;
  search.max_results = 1;
  search.token = a.token;
  CHECK(fixture.Input(kD, search).empty());
  search.token = 0;
  CHECK(fixture.Input(kA, search).empty());
  CHECK(Drops(fixture.core, Drop::kAuth) == 4);
  CHECK(fixture.core.counters().sessions == 0);
  // Relay-to-client directory types are never accepted.
  for (const auto type : {Type::kResults, Type::kTokenGrant, Type::kStatus}) {
    Message reply;
    reply.type = type;
    reply.page_count = 1;
    CHECK(fixture.Input(kA, reply).empty());
  }
  CHECK(Drops(fixture.core, Drop::kDirection) == 3);
}

TEST_CASE("relay directory enforces session ownership", "[relay][directory]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  auto record = Record(1);
  record.host_peer_id = b.peer_id;  // A client-chosen host identity is overwritten.
  record.host_ipv4 = b.ipv4;
  REQUIRE(fixture.Publish(kA, a.token, record) == Status::kOk);
  CHECK(fixture.Lookup(kB, b.token, 1)[0].host_ipv4 == a.ipv4);
  CHECK(fixture.Publish(kB, b.token, Record(1)) == Status::kConflict);
  CHECK(fixture.Remove(kB, b.token, 1) == Status::kConflict);
  CHECK(Drops(fixture.core, Drop::kSessionConflict) == 2);
  CHECK(fixture.Lookup(kB, b.token, 1)[0].host_peer_id == a.peer_id);
  CHECK(fixture.RequestToken(kB, b.token, 1).status == Status::kNotFound);
  CHECK(Drops(fixture.core, Drop::kTokenDenied) == 1);
}

TEST_CASE("relay directory enforces per-peer and per-room session limits",
          "[relay][directory]") {
  Fixture fixture(Config{1, kMaxMembers});
  const auto a = fixture.Join(kA);
  REQUIRE(fixture.Publish(kA, a.token, Record(1)) == Status::kOk);
  REQUIRE(fixture.Publish(kA, a.token, Record(2)) == Status::kOk);
  CHECK(fixture.Publish(kA, a.token, Record(3)) == Status::kLimit);
  CHECK(fixture.Publish(kA, a.token, Record(2, 0x20)) == Status::kOk);
  CHECK(Drops(fixture.core, Drop::kSessionLimit) == 1);
  for (uint16_t port = 1; port < kMaxSessionsPerRoom / kMaxSessionsPerPeer; ++port) {
    const auto peer = fixture.Join({9, port});
    REQUIRE(fixture.Publish({9, port}, peer.token, Record(port * 10)) == Status::kOk);
    REQUIRE(fixture.Publish({9, port}, peer.token, Record(port * 10 + 1)) == Status::kOk);
  }
  CHECK(fixture.core.counters().sessions == kMaxSessionsPerRoom);
  const auto late = fixture.Join(kB);
  CHECK(fixture.Publish(kB, late.token, Record(9999)) == Status::kLimit);
  CHECK(Drops(fixture.core, Drop::kSessionLimit) == 2);
  REQUIRE(fixture.Remove(kA, a.token, 1) == Status::kOk);
  CHECK(fixture.Publish(kB, late.token, Record(9999)) == Status::kOk);
}

TEST_CASE("relay directory rate limits queries and updates per peer", "[relay][directory]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  Message search;
  search.type = Type::kSearch;
  search.token = a.token;
  search.max_results = 1;
  for (uint32_t i = 0; i < kQueriesPerSecond; ++i)
    CHECK(fixture.Input(kA, search).size() == 1);
  CHECK(fixture.Input(kA, search).empty());
  Message lookup;
  lookup.type = Type::kLookup;
  lookup.token = a.token;
  lookup.session_id = 1;
  CHECK(fixture.Input(kA, lookup).empty());
  CHECK(Drops(fixture.core, Drop::kDirectoryRateLimited) == 2);
  // Another peer has its own budget, and updates use a separate budget.
  CHECK(fixture.Input(kB, search).empty());
  search.token = b.token;
  CHECK(fixture.Input(kB, search).size() == 1);
  for (uint32_t i = 0; i < kUpdatesPerSecond; ++i)
    CHECK(fixture.Remove(kA, a.token, 1) == Status::kOk);
  Message remove;
  remove.type = Type::kRemove;
  remove.sequence = 1000;
  remove.token = a.token;
  remove.session_id = 1;
  CHECK(fixture.Input(kA, remove).empty());
  CHECK(Drops(fixture.core, Drop::kDirectoryRateLimited) == 3);
  fixture.now = 250;
  search.token = a.token;
  CHECK(fixture.Input(kA, search).size() == 1);
  CHECK(fixture.Input(kA, search).empty());
  CHECK(fixture.Input(kA, remove).size() == 1);
}

TEST_CASE("relay sessions end on remove, BYE, idle expiry and empty room", "[relay][directory]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  REQUIRE(fixture.Publish(kA, a.token, Record(1)) == Status::kOk);
  REQUIRE(fixture.Publish(kB, b.token, Record(2)) == Status::kOk);
  const auto grant = fixture.RequestToken(kA, a.token, 1);
  REQUIRE(grant.type == Type::kTokenGrant);
  CHECK(fixture.core.counters().tokens == 1);
  SECTION("REMOVE is idempotent and revokes tokens") {
    CHECK(fixture.Remove(kA, a.token, 1) == Status::kOk);
    CHECK(fixture.Remove(kA, a.token, 1) == Status::kOk);
    CHECK(fixture.core.counters().sessions == 1);
  }
  SECTION("BYE") {
    fixture.Input(kA, Control(Type::kBye, a.token));
    CHECK(fixture.core.counters().sessions == 1);
    CHECK(fixture.Lookup(kB, b.token, 1).empty());
  }
  SECTION("idle expiry") {
    fixture.now = 14000;
    fixture.Input(kB, Control(Type::kHeartbeat, b.token));
    fixture.core.Tick(15000);
    CHECK(fixture.core.counters().peers == 1);
    CHECK(fixture.core.counters().sessions == 1);
  }
  SECTION("rejoin from the same endpoint replaces the host") {
    CHECK(fixture.Join(kA).type == Type::kWelcome);
    CHECK(fixture.core.counters().sessions == 1);
  }
  SECTION("empty room") {
    fixture.Input(kB, Control(Type::kBye, b.token));
    fixture.Input(kA, Control(Type::kBye, a.token));
    CHECK(fixture.core.counters().rooms == 0);
    CHECK(fixture.core.counters().sessions == 0);
  }
  CHECK(fixture.core.counters().tokens == 0);
  CHECK(fixture.TokenJoin(kD, grant).reason == Reason::kAuth);
}

TEST_CASE("relay join tokens admit members to the session's room", "[relay][directory]") {
  Fixture fixture(Config{2, 4, 500, 256 * 1024, 10});
  const auto host = fixture.Join(kA, "private", Digest{5});
  fixture.Join(kB, "other");
  REQUIRE(fixture.Publish(kA, host.token, Record(1)) == Status::kOk);
  CHECK(fixture.RequestToken(kA, host.token, 2).status == Status::kNotFound);
  const auto grant = fixture.RequestToken(kA, host.token, 1);
  REQUIRE(grant.type == Type::kTokenGrant);
  CHECK(grant.expiry_seconds == 10);
  CHECK(grant.join_token != TokenId{});

  SECTION("success and reuse") {
    const auto first = fixture.TokenJoin(kC, grant, Digest{5});
    REQUIRE(first.type == Type::kWelcome);
    CHECK(first.ipv4 != host.ipv4);
    const auto lookup = fixture.Lookup(kC, first.token, 1);
    REQUIRE(lookup.size() == 1);
    CHECK(lookup[0].host_ipv4 == host.ipv4);
    REQUIRE(fixture.TokenJoin(kD, grant, Digest{5}).type == Type::kWelcome);
    CHECK(fixture.core.counters().rooms == 2);
    CHECK(fixture.core.counters().peers == 4);
  }
  SECTION("wrong secret") {
    CHECK(fixture.TokenJoin(kC, grant, Digest{5}, true).reason == Reason::kAuth);
  }
  SECTION("unknown token") {
    auto unknown = grant;
    unknown.join_token[0] ^= 1;
    CHECK(fixture.TokenJoin(kC, unknown, Digest{5}).reason == Reason::kAuth);
  }
  SECTION("room secret does not prove a token") {
    auto room_secret = grant;
    std::fill_n(room_secret.token_secret.data(), TokenSecret::kSize, 0);
    std::memcpy(room_secret.token_secret.data(), "secret", 6);
    CHECK(fixture.TokenJoin(kC, room_secret, Digest{5}).reason == Reason::kAuth);
  }
  SECTION("fingerprint") {
    CHECK(fixture.TokenJoin(kC, grant, Digest{6}).reason == Reason::kFingerprintMismatch);
  }
  SECTION("expiry") {
    fixture.now = 9999;
    REQUIRE(fixture.TokenJoin(kC, grant, Digest{5}).type == Type::kWelcome);
    fixture.now = 10000;
    CHECK(fixture.TokenJoin(kD, grant, Digest{5}).reason == Reason::kAuth);
    CHECK(fixture.core.counters().tokens == 0);
  }
  SECTION("revocation") {
    REQUIRE(fixture.Remove(kA, host.token, 1) == Status::kOk);
    CHECK(fixture.TokenJoin(kC, grant, Digest{5}).reason == Reason::kAuth);
  }
  SECTION("room capacity") {
    Fixture small(Config{2, 1});
    const auto only = small.Join(kA);
    REQUIRE(small.Publish(kA, only.token, Record(1)) == Status::kOk);
    const auto token = small.RequestToken(kA, only.token, 1);
    CHECK(small.TokenJoin(kC, token).reason == Reason::kRoomFull);
  }
}

TEST_CASE("relay limits live tokens per session", "[relay][directory]") {
  Fixture fixture(Config{16, 4, 500, 256 * 1024, 1});
  const auto host = fixture.Join(kA);
  REQUIRE(fixture.Publish(kA, host.token, Record(1)) == Status::kOk);
  std::vector<TokenId> ids;
  for (size_t i = 0; i < kMaxTokensPerSession; ++i) {
    const auto grant = fixture.RequestToken(kA, host.token, 1);
    REQUIRE(grant.type == Type::kTokenGrant);
    CHECK(std::ranges::find(ids, grant.join_token) == ids.end());
    ids.push_back(grant.join_token);
  }
  CHECK(fixture.RequestToken(kA, host.token, 1).status == Status::kLimit);
  CHECK(Drops(fixture.core, Drop::kTokenLimit) == 1);
  fixture.now = 1000;
  CHECK(fixture.RequestToken(kA, host.token, 1).type == Type::kTokenGrant);
  CHECK(fixture.core.counters().tokens == 1);
}

TEST_CASE("relay answers a v1 HELLO with a v1 version REJECT", "[relay][directory]") {
  Fixture fixture;
  Message hello;
  hello.room = "room";
  auto bytes = Wire(hello);
  bytes[4] = 1;
  const auto output = fixture.core.Input(kA, bytes, 0);
  REQUIRE(output.size() == 1);
  CHECK(output[0].bytes == Bytes{'R', 'R', 'L', 'Y', 1, 5, 9, 0, 1});
  CHECK(output[0].bytes.size() <= bytes.size());
  CHECK(Drops(fixture.core, Drop::kVersion) == 1);
  CHECK(fixture.core.counters().pending == 0);
  bytes[4] = 3;
  CHECK(fixture.core.Input(kA, bytes, 0).empty());
  bytes[4] = 1;
  bytes.pop_back();
  Length(bytes);
  CHECK(fixture.core.Input(kA, bytes, 0).empty());
  CHECK(Drops(fixture.core, Drop::kVersion) == 3);
}

TEST_CASE("relay applies directory updates in sequence order", "[relay][directory]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  Message publish;
  publish.type = Type::kPublish;
  publish.token = a.token;
  publish.request_id = 1;
  publish.sequence = 1;
  publish.record = Record(1);
  Message remove;
  remove.type = Type::kRemove;
  remove.token = a.token;
  remove.request_id = 2;
  remove.sequence = 2;
  remove.session_id = 1;
  CHECK(fixture.One(kA, publish).status == Status::kOk);
  CHECK(fixture.core.counters().sessions == 1);
  CHECK(fixture.One(kA, remove).status == Status::kOk);
  // A delayed retransmission of the earlier PUBLISH is acknowledged but not applied.
  const auto replay = fixture.One(kA, publish);
  CHECK(replay.type == Type::kStatus);
  CHECK(replay.request_id == 1);
  CHECK(replay.status == Status::kOk);
  CHECK(fixture.core.counters().sessions == 0);
  CHECK(fixture.Lookup(kB, b.token, 1).empty());
  // A repeated latest update returns its original outcome without applying again.
  Message conflict = publish;
  conflict.token = b.token;
  conflict.request_id = 3;
  conflict.sequence = 7;
  fixture.next_request = 100;  // Fixture sequences follow request ids.
  REQUIRE(fixture.Publish(kA, a.token, Record(9)) == Status::kOk);
  conflict.record = Record(9);
  CHECK(fixture.One(kB, conflict).status == Status::kConflict);
  CHECK(fixture.One(kB, conflict).status == Status::kConflict);
  CHECK(Drops(fixture.core, Drop::kSessionConflict) == 1);
  // Sequences are per peer: another peer's lower sequence still applies.
  conflict.record = Record(10);
  conflict.sequence = 8;
  CHECK(fixture.One(kB, conflict).status == Status::kOk);
  CHECK(fixture.core.counters().sessions == 2);
}

TEST_CASE("relay repeats a token grant for a retried request", "[relay][directory]") {
  Fixture fixture(Config{16, 4, 500, 256 * 1024, 10});
  const auto host = fixture.Join(kA);
  REQUIRE(fixture.Publish(kA, host.token, Record(1)) == Status::kOk);
  Message request;
  request.type = Type::kTokenRequest;
  request.token = host.token;
  request.request_id = 500;
  request.session_id = 1;
  const auto first = fixture.One(kA, request);
  REQUIRE(first.type == Type::kTokenGrant);
  CHECK(first.expiry_seconds == 10);
  for (size_t i = 1; i < kMaxTokensPerSession; ++i)
    REQUIRE(fixture.RequestToken(kA, host.token, 1).type == Type::kTokenGrant);
  CHECK(fixture.core.counters().tokens == kMaxTokensPerSession);
  // The table is full, but a retry of the first request still receives its grant.
  fixture.now = 2500;
  const auto retry = fixture.One(kA, request);
  REQUIRE(retry.type == Type::kTokenGrant);
  CHECK(retry.join_token == first.join_token);
  CHECK(retry.token_secret == first.token_secret);
  CHECK(retry.expiry_seconds == 8);
  CHECK(fixture.core.counters().tokens == kMaxTokensPerSession);
  CHECK(Drops(fixture.core, Drop::kTokenLimit) == 0);
  request.request_id = 501;
  CHECK(fixture.One(kA, request).status == Status::kLimit);
}

TEST_CASE("relay RESULTS carry the directory snapshot", "[relay][directory]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  const auto b = fixture.Join(kB);
  Message search;
  search.type = Type::kSearch;
  search.token = b.token;
  search.max_results = 1;
  const auto before = Read(fixture.Input(kB, search)[0].bytes).snapshot;
  CHECK(Read(fixture.Input(kB, search)[0].bytes).snapshot == before);
  REQUIRE(fixture.Publish(kA, a.token, Record(1)) == Status::kOk);
  const auto published = Read(fixture.Input(kB, search)[0].bytes).snapshot;
  CHECK(published != before);
  REQUIRE(fixture.Remove(kA, a.token, 1) == Status::kOk);
  CHECK(Read(fixture.Input(kB, search)[0].bytes).snapshot != published);
}

TEST_CASE("relay token secrets wipe moved-from copies", "[relay][directory]") {
  Digest bytes{};
  bytes.fill(0x7e);
  TokenSecret secret(bytes);
  const TokenSecret zero;
  const auto copy = secret;
  CHECK(copy == secret);
  TokenSecret moved(std::move(secret));
  CHECK(moved == copy);
  CHECK(secret == zero);  // NOLINT(bugprone-use-after-move): verifying the wipe.
  TokenSecret assigned;
  assigned = std::move(moved);
  CHECK(assigned == copy);
  CHECK(moved == zero);  // NOLINT(bugprone-use-after-move)
  // A decoded grant returned through std::expected keeps the secret intact.
  Message grant;
  grant.type = Type::kTokenGrant;
  grant.token_secret = copy;
  CHECK(Read(Wire(grant)).token_secret == copy);
}

TEST_CASE("relay update sequences compare across wrap", "[relay][directory]") {
  Fixture fixture;
  const auto a = fixture.Join(kA);
  Message publish;
  publish.type = Type::kPublish;
  publish.token = a.token;
  publish.record = Record(1);
  // Serial-number order accepts steps below 2^31, so climb to UINT32_MAX first.
  uint16_t step = 0;
  for (const uint32_t sequence : {1u, 0x40000000u, 0x80000000u, 0xc0000000u, 0xffffffffu}) {
    publish.sequence = sequence;
    publish.record.open_public = step++;
    REQUIRE(fixture.One(kA, publish).status == Status::kOk);
    CHECK(fixture.Lookup(kA, a.token, 1)[0].open_public == publish.record.open_public);
    fixture.now += 1000;  // Stay inside the query budget.
  }
  Message remove;
  remove.type = Type::kRemove;
  remove.token = a.token;
  remove.session_id = 1;
  remove.sequence = 1;  // The client skips zero after UINT32_MAX.
  CHECK(fixture.One(kA, remove).status == Status::kOk);
  CHECK(fixture.core.counters().sessions == 0);
  publish.sequence = 0xffffffffu;  // Now older than 1.
  CHECK(fixture.One(kA, publish).status == Status::kOk);
  CHECK(fixture.core.counters().sessions == 0);
}
