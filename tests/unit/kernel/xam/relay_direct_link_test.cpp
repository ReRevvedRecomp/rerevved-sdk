#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/system/xsocket.h>
#include "kernel/xam/relay_system_link.h"
#include "kernel/xam/system_link_config.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace rex::kernel::xam::relay_link {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using system::XSocket;

long long Milliseconds(Clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// A loopback relay running the real core, with taps and filters on both sides.
struct TestRelay {
  using OutFilter = std::function<void(std::vector<relay::Datagram>&)>;
  using InFilter = std::function<bool(const relay::Datagram&)>;  // false drops before the core
  std::unique_ptr<relay::UdpSocket> socket;
  relay::Core core{{}, "secret", relay::Digest{42}};
  std::mutex mutex;
  OutFilter out;
  InFilter in;
  uint64_t unicast_data = 0;
  uint64_t broadcast_data = 0;
  std::map<relay::Type, uint64_t> received;
  // Arrival time of each CONNECT_REQUEST by source endpoint.
  std::map<relay::Endpoint, std::vector<Clock::time_point>> requests;
  std::jthread pump;

  TestRelay() {
    auto opened = relay::UdpSocket::Open("127.0.0.1", 0);
    REQUIRE(opened);
    socket = std::move(*opened);
    pump = std::jthread([this](std::stop_token stop) {
      const auto start = Clock::now();
      while (!stop.stop_requested()) {
        auto packet = socket->Receive(10);
        const auto now = static_cast<uint64_t>(Milliseconds(Clock::now() - start));
        std::vector<relay::Datagram> output;
        {
          std::lock_guard lock(mutex);
          core.Tick(now);
          output = core.TakeNotices();
          if (packet && *packet) {
            const auto& datagram = **packet;
            if (datagram.bytes.size() > 5 && !std::memcmp(datagram.bytes.data(), "RRLY", 4)) {
              const auto type = static_cast<relay::Type>(datagram.bytes[5]);
              ++received[type];
              if (type == relay::Type::kConnectRequest)
                requests[datagram.endpoint].push_back(Clock::now());
              if (auto message = relay::Decode(datagram.bytes);
                  message && message->type == relay::Type::kData)
                ++(message->ipv4 == 0xffffffffu ? broadcast_data : unicast_data);
            }
            if (!in || in(datagram)) {
              auto replies = core.Input(datagram.endpoint, datagram.bytes, now);
              if (out)
                out(replies);
              output.insert(output.end(), replies.begin(), replies.end());
            }
          }
        }
        for (const auto& datagram : output)
          (void)socket->Send(datagram);
      }
    });
  }
  ~TestRelay() {
    pump.request_stop();
    pump.join();
  }
  relay::Endpoint endpoint() const { return socket->endpoint(); }
  template <typename F>
  auto Locked(F f) {
    std::lock_guard lock(mutex);
    return f();
  }
};

DirectOptions Enabled(std::array<uint32_t, 3> backoff = {30000, 60000, 120000},
                      uint32_t drop_after = 0) {
  DirectOptions options;
  options.enabled = true;
  options.backoff_ms = backoff;
  options.drop_after_seconds = drop_after;
  return options;
}

bool WaitFor(const std::function<bool()>& condition, Clock::duration timeout) {
  const auto deadline = Clock::now() + timeout;
  while (!condition()) {
    if (Clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

// One guest UDP socket bound to a guest port on a client.
struct Guest {
  Client& client;
  XSocket socket{nullptr};
  Guest(Client& owner, uint16_t port) : client(owner) {
    REQUIRE(socket.Initialize(XSocket::X_AF_INET, XSocket::X_SOCK_DGRAM, XSocket::X_IPPROTO_UDP) ==
            X_STATUS_SUCCESS);
    REQUIRE(client.Bind(socket.native_handle(), port));
  }
  void Send(relay::Endpoint to, const relay::Bytes& payload) {
    auto sent = client.Send(socket.native_handle(), to, payload);
    REQUIRE(sent);
    CHECK(*sent == static_cast<int>(payload.size()));
  }
  // Returns false when nothing arrives within the timeout.
  bool Receive(relay::Bytes& payload, relay::Endpoint& source, long timeout_ms = 2000) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket.native_handle(), &readable);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(static_cast<int>(socket.native_handle() + 1), &readable, nullptr, nullptr,
               &timeout) != 1)
      return false;
    std::array<uint8_t, relay::kMaxPayload> buffer{};
    auto count = client.Receive(socket.native_handle(), buffer, 0, source);
    if (!count)
      return false;
    payload.assign(buffer.begin(), buffer.begin() + *count);
    return true;
  }
  void Expect(relay::Endpoint source, const relay::Bytes& payload) {
    relay::Bytes received;
    relay::Endpoint from{};
    REQUIRE(Receive(received, from));
    CHECK(from == source);
    CHECK(received == payload);
  }
};

relay::Bytes Payload(uint32_t index, size_t size = 64) {
  relay::Bytes payload(size);
  for (size_t i = 0; i < size; ++i)
    payload[i] = static_cast<uint8_t>(index * 31 + i);
  return payload;
}

TEST_CASE("relay clients move unicast onto an authenticated direct path",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  Client a, b;
  a.SetDirect(Enabled());
  b.SetDirect(Enabled());
  REQUIRE(a.Start(relay.endpoint(), "direct", "secret", {}));
  REQUIRE(b.Start(relay.endpoint(), "direct", "secret", {}));
  Guest ga(a, 1000), gb(b, 1000);
  const relay::Endpoint to_b{b.ipv4(), 1000}, to_a{a.ipv4(), 1000};
  CHECK(system_link::RelayStatus(a.stats()).find("direct") == std::string::npos);

  const auto first = Clock::now();
  ga.Send(to_b, Payload(0));
  gb.Expect(to_a, Payload(0));
  REQUIRE(WaitFor([&] { return a.stats().direct_peers == 1 && b.stats().direct_peers == 1; }, 6s));
  const auto established = Milliseconds(Clock::now() - first);
  std::printf("direct timing: path established %lld ms after the first unicast\n", established);
  CHECK(established < 2000);
  const auto relayed = relay.Locked([&] { return relay.unicast_data; });
  CHECK(relayed >= 1);

  for (uint32_t i = 1; i <= 20; ++i) {
    ga.Send(to_b, Payload(i, 1 + i * 50));
    gb.Expect(to_a, Payload(i, 1 + i * 50));
    gb.Send(to_a, Payload(100 + i));
    ga.Expect(to_b, Payload(100 + i));
  }
  // The largest guest payload travels direct as well.
  ga.Send(to_b, Payload(7, relay::kMaxPayload));
  gb.Expect(to_a, Payload(7, relay::kMaxPayload));
  CHECK(relay.Locked([&] { return relay.unicast_data; }) == relayed);
  CHECK(a.stats().direct_out >= 21);
  CHECK(b.stats().direct_in >= 21);
  CHECK(a.stats().direct_established == 1);
  auto without_direct = a.stats();
  without_direct.direct_attempted_peers = 0;
  CHECK(system_link::RelayStatus(a.stats()) ==
        system_link::RelayStatus(without_direct) + " (direct: 1 of 1 peers)");

  // Broadcast keeps using the relay.
  ga.Send({0xffffffffu, 1000}, Payload(9));
  gb.Expect(to_a, Payload(9));
  CHECK(relay.Locked([&] { return relay.broadcast_data; }) == 1);
  CHECK(relay.Locked([&] { return relay.unicast_data; }) == relayed);
  for (auto* stats : {&a, &b}) {
    const auto s = stats->stats();
    CHECK(s.direct_bad_mac == 0);
    CHECK(s.direct_replay == 0);
    CHECK(s.direct_malformed == 0);
    CHECK(s.direct_fallbacks == 0);
  }
  // Cleanup closes the path; the final stats keep the counters.
  const auto final_stats = a.Stop();
  CHECK(final_stats.direct_peers == 0);
  CHECK(final_stats.direct_established == 1);
  b.Stop();
}

TEST_CASE("relay clients stay on the relay when direct candidates never answer",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  uint16_t closed = 0;
  {
    auto probe = relay::UdpSocket::Open("127.0.0.1", 0);
    REQUIRE(probe);
    closed = (*probe)->endpoint().port;
  }
  // The relay rewrites every candidate to a closed loopback port. The tag covers
  // the endpoints, so the test follows each handshake to derive the recipient's
  // K_assoc and re-wraps the pair key as the relay itself would.
  std::map<relay::Endpoint, relay::Nonce> hellos, challenges;
  std::map<relay::Endpoint, relay::Digest> assocs;
  relay.in = [&](const relay::Datagram& datagram) {
    if (auto message = relay::Decode(datagram.bytes);
        message && message->type == relay::Type::kHello)
      hellos[datagram.endpoint] = message->nonce;
    return true;
  };
  relay.out = [&](std::vector<relay::Datagram>& output) {
    const std::string_view secret = "secret";
    for (auto& datagram : output) {
      auto message = relay::Decode(datagram.bytes);
      if (!message)
        continue;
      if (message->type == relay::Type::kChallenge)
        challenges[datagram.endpoint] = message->nonce;
      if (message->type == relay::Type::kWelcome)
        assocs[datagram.endpoint] = relay::AssociationKey(
            {reinterpret_cast<const uint8_t*>(secret.data()), secret.size()},
            hellos[datagram.endpoint], challenges[datagram.endpoint], message->peer_id);
      if (message->type != relay::Type::kConnectInfo)
        continue;
      const auto& assoc = assocs[datagram.endpoint];
      auto key = relay::UnwrapPairKey(assoc, *message);
      if (!key)
        continue;
      message->candidates.clear();
      message->observed = {htonl(INADDR_LOOPBACK), closed};
      relay::WrapPairKey(assoc, key->bytes(), *message);
      datagram.bytes = *relay::Encode(*message);
    }
  };
  Client a, b;
  a.SetDirect(Enabled({1500, 3000, 6000}));
  b.SetDirect(Enabled({1500, 3000, 6000}));
  REQUIRE(a.Start(relay.endpoint(), "fallback", "secret", {}));
  REQUIRE(b.Start(relay.endpoint(), "fallback", "secret", {}));
  Guest ga(a, 1000), gb(b, 1000);
  const relay::Endpoint to_b{b.ipv4(), 1000}, to_a{a.ipv4(), 1000};
  uint32_t sent = 0, received = 0;
  const auto start = Clock::now();
  while (Clock::now() - start < 8s) {
    ga.Send(to_b, Payload(sent));
    relay::Bytes payload;
    relay::Endpoint from{};
    REQUIRE(gb.Receive(payload, from));
    CHECK(payload == Payload(sent));
    CHECK(from == to_a);
    ++sent;
    ++received;
    std::this_thread::sleep_for(40ms);
  }
  CHECK(received == sent);
  CHECK(relay.Locked([&] { return relay.unicast_data; }) == sent);
  const auto stats = a.stats();
  CHECK(stats.direct_peers == 0);
  CHECK(stats.direct_attempts >= 1);
  CHECK(stats.direct_probe_failures >= 1);
  CHECK(stats.direct_established == 0);
  CHECK(system_link::RelayStatus(stats).ends_with(" (direct: 0 of 1 peers)"));
  // After the first failure, the next request waits for the first backoff.
  const auto requests = relay.Locked([&] { return relay.requests; });
  bool spaced = false;
  for (const auto& [endpoint, times] : requests) {
    for (size_t i = 1; i < times.size(); ++i) {
      const auto gap = Milliseconds(times[i] - times[i - 1]);
      // Retransmissions within one attempt are 1 s apart; a new attempt follows
      // the 5 s probe window plus the 1.5 s backoff.
      CHECK((gap < 1500 || gap >= 6400));
      spaced = spaced || gap >= 6400;
    }
  }
  CHECK(spaced);
}

// A peer driven by the test through the real relay, so it can forge direct frames.
struct FakePeer {
  std::unique_ptr<relay::UdpSocket> socket;
  relay::Endpoint server;
  uint64_t token = 0;
  uint32_t ipv4 = 0;
  relay::Digest assoc{};
  relay::TokenSecret pair_key;
  uint32_t attempt = 0;
  uint32_t other = 0;
  relay::Endpoint other_endpoint;

  FakePeer(relay::Endpoint relay_endpoint, std::string room) : server(relay_endpoint) {
    auto opened = relay::UdpSocket::Open("127.0.0.1", 0);
    REQUIRE(opened);
    socket = std::move(*opened);
    relay::Message hello;
    hello.type = relay::Type::kHello;
    hello.room = room;
    hello.nonce.fill(0x42);
    const auto challenge = Exchange(hello, relay::Type::kChallenge);
    relay::Message join;
    join.type = relay::Type::kJoin;
    join.auth = relay::JoinAuth("secret", hello.nonce, challenge.nonce, room, {});
    const auto welcome = Exchange(join, relay::Type::kWelcome);
    token = welcome.token;
    ipv4 = welcome.ipv4;
    const std::string_view secret = "secret";
    assoc = relay::AssociationKey({reinterpret_cast<const uint8_t*>(secret.data()), secret.size()},
                                  hello.nonce, challenge.nonce, welcome.peer_id);
    relay::Message candidates;
    candidates.type = relay::Type::kCandidates;
    candidates.token = token;
    Send(Signed(candidates));
  }
  relay::Message Signed(relay::Message message) const {
    REQUIRE(relay::SignConnect(assoc, message));
    return message;
  }
  void Send(const relay::Message& message) {
    REQUIRE(socket->Send({server, *relay::Encode(message)}));
  }
  // Resends at the given interval when nonzero, as the real client retries.
  relay::Message Exchange(const relay::Message& message, relay::Type expected,
                          Clock::duration resend = {}) {
    Send(message);
    const auto deadline = Clock::now() + 2s;
    auto next = Clock::now() + resend;
    while (Clock::now() < deadline) {
      if (resend != Clock::duration{} && Clock::now() >= next) {
        Send(message);
        next += resend;
      }
      auto packet = socket->Receive(50);
      REQUIRE(packet);
      if (!*packet)
        continue;
      auto reply = relay::Decode((**packet).bytes);
      if (reply && reply->type == expected)
        return *reply;
    }
    FAIL("No relay reply");
    return {};
  }
  void Connect(uint32_t target) {
    relay::Message request;
    request.type = relay::Type::kConnectRequest;
    request.token = token;
    request.ipv4 = target;
    // The target may not have announced its candidates yet; the relay then drops
    // the request as unavailable.
    const auto info = Exchange(Signed(request), relay::Type::kConnectInfo, 300ms);
    auto key = relay::UnwrapPairKey(assoc, info);
    REQUIRE(key);
    pair_key = *key;
    attempt = info.attempt_id;
    other = info.ipv4;
    other_endpoint = {info.observed.ipv4, info.observed.port};
  }
  void Bye() {
    relay::Message bye;
    bye.type = relay::Type::kBye;
    bye.token = token;
    Send(bye);
  }
  relay::Bytes Data(uint64_t sequence, uint16_t port, const relay::Bytes& payload) {
    relay::DirectFrame frame;
    frame.type = relay::DirectType::kData;
    frame.source_port = port;
    frame.destination_port = port;
    frame.payload = payload;
    return Frame(std::move(frame), sequence);
  }
  relay::Bytes Frame(relay::DirectType type, uint64_t sequence, uint64_t echo = 0,
                     uint32_t attempt_id = 0, uint32_t sender = 0) {
    relay::DirectFrame frame;
    frame.type = type;
    frame.echo = echo;
    return Frame(std::move(frame), sequence, attempt_id, sender);
  }
  relay::Bytes Frame(relay::DirectFrame frame, uint64_t sequence, uint32_t attempt_id = 0,
                     uint32_t sender = 0) {
    frame.attempt_id = attempt_id ? attempt_id : attempt;
    frame.sender_ipv4 = sender ? sender : ipv4;
    frame.sequence = sequence;
    auto bytes = relay::EncodeDirect(frame, relay::DirectKey(pair_key.bytes(), ipv4));
    REQUIRE(bytes);
    return *bytes;
  }
  // Collects direct frames from the real client for a while, verifying each MAC.
  std::vector<relay::DirectFrame> Collect(Clock::duration duration) {
    std::vector<relay::DirectFrame> frames;
    const auto key = relay::DirectKey(pair_key.bytes(), other);
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
      auto packet = socket->Receive(10);
      REQUIRE(packet);
      if (!*packet || (**packet).endpoint == server)
        continue;
      auto frame = relay::ParseDirect((**packet).bytes);
      REQUIRE(frame);
      REQUIRE(relay::VerifyDirect((**packet).bytes, key));
      frames.push_back(*frame);
    }
    return frames;
  }
  static bool Answered(const std::vector<relay::DirectFrame>& frames, uint64_t sequence) {
    return std::any_of(frames.begin(), frames.end(), [&](const relay::DirectFrame& frame) {
      return frame.type == relay::DirectType::kProbeAck && frame.echo == sequence;
    });
  }
};

TEST_CASE("relay client rejects spoofed, stale and replayed direct datagrams",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  Client a;
  a.SetDirect(Enabled());
  REQUIRE(a.Start(relay.endpoint(), "spoof", "secret", {}));
  FakePeer peer(relay.endpoint(), "spoof");
  peer.Connect(a.ipv4());
  // The real client probes the fake peer's observed endpoint with authenticated frames.
  const auto probes = peer.Collect(500ms);
  REQUIRE_FALSE(probes.empty());
  CHECK(probes[0].type == relay::DirectType::kProbe);
  CHECK(probes[0].sender_ipv4 == a.ipv4());
  CHECK(probes[0].attempt_id == peer.attempt);

  auto send = [&](const relay::Bytes& bytes, relay::UdpSocket* from = nullptr) {
    REQUIRE((from ? from : peer.socket.get())->Send({peer.other_endpoint, bytes}));
  };
  const auto valid = peer.Frame(relay::DirectType::kProbe, 1);
  send(valid);
  CHECK(FakePeer::Answered(peer.Collect(300ms), 1));
  CHECK(a.stats().direct_in == 1);

  auto bad_mac = peer.Frame(relay::DirectType::kProbe, 2);
  bad_mac.back() ^= 1;
  send(bad_mac);
  send(peer.Frame(relay::DirectType::kProbe, 3, 0, peer.attempt + 1));
  send(valid);  // replay
  send(peer.Frame(relay::DirectType::kProbe, 4, 0, 0, htonl(0xc6120063)));
  send({'R', 'R', 'D', 'T', 1, 9, 0});
  auto other = relay::UdpSocket::Open("127.0.0.1", 0);
  REQUIRE(other);
  auto unknown = peer.Frame(relay::DirectType::kProbe, 5);
  unknown[unknown.size() - 3] ^= 0x80;
  send(unknown, other->get());
  const auto replies = peer.Collect(400ms);
  for (uint64_t sequence : {1, 2, 3, 4, 5})
    CHECK_FALSE(FakePeer::Answered(replies, sequence));
  auto stranger = (*other)->Receive(100);
  REQUIRE(stranger);
  CHECK_FALSE(*stranger);
  const auto stats = a.stats();
  CHECK(stats.direct_bad_mac == 2);
  CHECK(stats.direct_stale_attempt == 1);
  CHECK(stats.direct_replay == 1);
  CHECK(stats.direct_unknown_peer == 1);
  CHECK(stats.direct_malformed == 1);
  CHECK(stats.direct_in == 1);

  // A later authenticated frame still works after the rejected ones.
  send(peer.Frame(relay::DirectType::kProbe, 6));
  CHECK(FakePeer::Answered(peer.Collect(300ms), 6));
}

TEST_CASE("relay client rejects a replayed CONNECT_INFO after CONNECT_CLOSE",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  std::vector<relay::Datagram> infos;
  relay.out = [&](std::vector<relay::Datagram>& output) {
    for (const auto& datagram : output)
      if (datagram.bytes.size() > 5 &&
          datagram.bytes[5] == static_cast<uint8_t>(relay::Type::kConnectInfo))
        infos.push_back(datagram);
  };
  Client a;
  a.SetDirect(Enabled());
  REQUIRE(a.Start(relay.endpoint(), "close", "secret", {}));
  Guest ga(a, 1000);
  FakePeer peer(relay.endpoint(), "close");
  peer.Connect(a.ipv4());
  REQUIRE(WaitFor([&] { return a.stats().direct_attempts == 1; }, 2s));
  // The CONNECT_INFO the relay sent the real client about the fake peer.
  const auto captured = relay.Locked([&] {
    for (const auto& datagram : infos)
      if (auto message = relay::Decode(datagram.bytes); message && message->ipv4 == peer.ipv4)
        return datagram;
    return relay::Datagram{};
  });
  REQUIRE_FALSE(captured.bytes.empty());

  const auto data = peer.Data(1, 1000, Payload(1));
  REQUIRE(peer.socket->Send({peer.other_endpoint, data}));
  ga.Expect({peer.ipv4, 1000}, Payload(1));

  // Leaving sends CONNECT_CLOSE, which removes the peer entry.
  peer.Bye();
  REQUIRE(WaitFor([&] { return a.stats().direct_attempted_peers == 0; }, 2s));
  const auto before = a.stats();

  // Replaying the captured CONNECT_INFO from the relay endpoint, then the old DATA.
  REQUIRE(relay.socket->Send(captured));
  REQUIRE(
      WaitFor([&] { return a.stats().direct_stale_attempt > before.direct_stale_attempt; }, 2s));
  REQUIRE(peer.socket->Send({peer.other_endpoint, data}));
  REQUIRE(WaitFor([&] { return a.stats().direct_unknown_peer > before.direct_unknown_peer; }, 2s));
  relay::Bytes received;
  relay::Endpoint from{};
  CHECK_FALSE(ga.Receive(received, from, 300));
  const auto after = a.stats();
  CHECK(after.direct_attempts == 1);
  CHECK(after.direct_attempted_peers == 0);
  CHECK(after.direct_in == before.direct_in);
}

TEST_CASE("relay client rejects a CONNECT_INFO delayed past its CONNECT_CLOSE",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  relay::Endpoint fake{};
  std::vector<relay::Datagram> held;
  // Hold back every CONNECT_INFO that is not addressed to the fake peer.
  relay.out = [&](std::vector<relay::Datagram>& output) {
    std::erase_if(output, [&](const relay::Datagram& datagram) {
      if (datagram.bytes.size() <= 5 || datagram.endpoint == fake ||
          datagram.bytes[5] != static_cast<uint8_t>(relay::Type::kConnectInfo))
        return false;
      held.push_back(datagram);
      return true;
    });
  };
  Client a;
  a.SetDirect(Enabled());
  REQUIRE(a.Start(relay.endpoint(), "delay", "secret", {}));
  Guest ga(a, 1000);
  FakePeer peer(relay.endpoint(), "delay");
  relay.Locked([&] { fake = peer.socket->endpoint(); });
  peer.Connect(a.ipv4());
  REQUIRE(relay.Locked([&] { return !held.empty(); }));

  // The CLOSE arrives first and finds no entry; its attempt id still becomes the floor.
  peer.Bye();
  REQUIRE(WaitFor([&] { return a.stats().direct_stale_attempt == 1; }, 2s));
  REQUIRE(relay.socket->Send(relay.Locked([&] { return held.front(); })));
  REQUIRE(WaitFor([&] { return a.stats().direct_stale_attempt == 2; }, 2s));
  REQUIRE(peer.socket->Send({peer.other_endpoint, peer.Data(1, 1000, Payload(1))}));
  REQUIRE(WaitFor([&] { return a.stats().direct_unknown_peer == 1; }, 2s));
  relay::Bytes received;
  relay::Endpoint from{};
  CHECK_FALSE(ga.Receive(received, from, 300));
  const auto stats = a.stats();
  CHECK(stats.direct_attempts == 0);
  CHECK(stats.direct_attempted_peers == 0);
  CHECK(stats.direct_in == 0);
  CHECK(stats.direct_bad_tag == 0);
}

TEST_CASE("relay client ignores a probe acknowledgement after the probing window",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  Client a;
  a.SetDirect(Enabled());
  REQUIRE(a.Start(relay.endpoint(), "late", "secret", {}));
  FakePeer peer(relay.endpoint(), "late");
  peer.Connect(a.ipv4());
  // Collect the real client's probes without answering any of them.
  std::vector<relay::DirectFrame> probes;
  REQUIRE(WaitFor(
      [&] {
        for (const auto& frame : peer.Collect(100ms))
          probes.push_back(frame);
        return a.stats().direct_probe_failures == 1;
      },
      7s));
  REQUIRE_FALSE(probes.empty());
  REQUIRE(probes.front().type == relay::DirectType::kProbe);
  // A valid but late acknowledgement neither establishes the path nor resets backoff.
  REQUIRE(peer.socket->Send(
      {peer.other_endpoint, peer.Frame(relay::DirectType::kProbeAck, 1, probes.front().sequence)}));
  REQUIRE(WaitFor([&] { return a.stats().direct_in == 1; }, 2s));
  std::this_thread::sleep_for(200ms);
  const auto stats = a.stats();
  CHECK(stats.direct_peers == 0);
  CHECK(stats.direct_established == 0);
}

TEST_CASE("relay client evicts unanswered peers so a real peer gets an entry",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  Client a, b;
  a.SetDirect(Enabled({500, 500, 500}));
  b.SetDirect(Enabled({500, 500, 500}));
  REQUIRE(a.Start(relay.endpoint(), "evict", "secret", {}));
  REQUIRE(b.Start(relay.endpoint(), "evict", "secret", {}));
  Guest ga(a, 1000), gb(b, 1000);
  // Fill the peer table with virtual addresses that are not room members.
  uint32_t filled = 0;
  for (uint32_t host = 0xc6120100u; filled < relay::kMaxMembers; ++host) {
    const auto vip = htonl(host);
    if (vip == a.ipv4() || vip == b.ipv4())
      continue;
    ga.Send({vip, 1000}, Payload(filled));
    ++filled;
  }
  REQUIRE(WaitFor([&] { return a.stats().direct_attempted_peers == relay::kMaxMembers; }, 2s));
  // Unanswered requests drain a's connect budget until they time out into backoff.
  const relay::Endpoint to_b{b.ipv4(), 1000}, to_a{a.ipv4(), 1000};
  REQUIRE(WaitFor(
      [&] {
        ga.Send(to_b, Payload(1));
        gb.Expect(to_a, Payload(1));
        std::this_thread::sleep_for(50ms);
        return a.stats().direct_peers == 1;
      },
      10s));
  CHECK(a.stats().direct_evicted >= 1);
}

TEST_CASE("relay client rejects CONNECT_INFO with a bad tag",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  relay.out = [](std::vector<relay::Datagram>& output) {
    for (auto& datagram : output)
      if (datagram.bytes.size() > 5 &&
          datagram.bytes[5] == static_cast<uint8_t>(relay::Type::kConnectInfo))
        datagram.bytes.back() ^= 1;
  };
  Client a, b;
  a.SetDirect(Enabled());
  b.SetDirect(Enabled());
  REQUIRE(a.Start(relay.endpoint(), "tag", "secret", {}));
  REQUIRE(b.Start(relay.endpoint(), "tag", "secret", {}));
  Guest ga(a, 1000), gb(b, 1000);
  ga.Send({b.ipv4(), 1000}, Payload(1));
  gb.Expect({a.ipv4(), 1000}, Payload(1));
  REQUIRE(WaitFor([&] { return a.stats().direct_bad_tag && b.stats().direct_bad_tag; }, 3s));
  CHECK(a.stats().direct_attempts == 0);
  CHECK(b.stats().direct_out == 0);
}

TEST_CASE("relay clients fall back to the relay when a direct path stops working",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  std::map<uint32_t, relay::Endpoint> observed;  // virtual IPv4 -> relay-observed endpoint
  relay.out = [&](std::vector<relay::Datagram>& output) {
    for (const auto& datagram : output)
      if (auto message = relay::Decode(datagram.bytes);
          message && message->type == relay::Type::kConnectInfo)
        observed[message->ipv4] = {message->observed.ipv4, message->observed.port};
  };
  Client a, b;
  // a drops every received direct datagram two seconds into its path.
  a.SetDirect(Enabled({2000, 4000, 8000}, 2));
  b.SetDirect(Enabled({2000, 4000, 8000}));
  REQUIRE(a.Start(relay.endpoint(), "loss", "secret", {}));
  REQUIRE(b.Start(relay.endpoint(), "loss", "secret", {}));
  Guest ga(a, 1000), gb(b, 1000);
  const relay::Endpoint to_b{b.ipv4(), 1000}, to_a{a.ipv4(), 1000};
  ga.Send(to_b, Payload(0));
  gb.Expect(to_a, Payload(0));
  REQUIRE(WaitFor([&] { return a.stats().direct_peers == 1 && b.stats().direct_peers == 1; }, 6s));
  const auto established = Clock::now();
  const auto fault = established + 2s;

  // Traffic both ways at 20 Hz; only what the receiver gets counts.
  std::atomic<bool> running = true;
  std::atomic<uint32_t> a_received = 0, b_received = 0;
  // Honors the stop token so a failed REQUIRE cannot hang the jthread destructor.
  std::jthread reader([&](std::stop_token stop) {
    while (running && !stop.stop_requested()) {
      relay::Bytes payload;
      relay::Endpoint from{};
      if (ga.Receive(payload, from, 50))
        ++a_received;
      if (gb.Receive(payload, from, 50))
        ++b_received;
    }
  });
  Clock::time_point a_down{}, b_down{};
  uint32_t index = 1;
  const auto deadline = fault + 20s;
  while (Clock::now() < deadline &&
         (a_down == Clock::time_point{} || b_down == Clock::time_point{})) {
    ga.Send(to_b, Payload(index));
    gb.Send(to_a, Payload(index + 1));
    index += 2;
    if (a_down == Clock::time_point{} && a.stats().direct_peers == 0)
      a_down = Clock::now();
    if (b_down == Clock::time_point{} && b.stats().direct_peers == 0)
      b_down = Clock::now();
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(a_down != Clock::time_point{});
  REQUIRE(b_down != Clock::time_point{});
  std::printf("direct timing: fault side fell back %lld ms after the fault, peer after %lld ms\n",
              Milliseconds(a_down - fault), Milliseconds(b_down - fault));
  // Neither side receives acknowledgements once the fault side drops everything.
  CHECK(a_down - fault <= 2500ms);
  CHECK(b_down - fault <= 2500ms);
  CHECK(a.stats().direct_fault_drops > 0);
  CHECK(a.stats().direct_fallbacks == 1);
  CHECK(b.stats().direct_fallbacks == 1);

  // Once both sides use the relay, every datagram arrives again.
  std::this_thread::sleep_for(300ms);
  const uint32_t a_before = a_received, b_before = b_received;
  const auto relayed_before = relay.Locked([&] { return relay.unicast_data; });
  for (uint32_t i = 0; i < 20; ++i) {
    ga.Send(to_b, Payload(i));
    gb.Send(to_a, Payload(i));
    std::this_thread::sleep_for(20ms);
  }
  REQUIRE(WaitFor([&] { return a_received - a_before == 20 && b_received - b_before == 20; }, 3s));
  CHECK(relay.Locked([&] { return relay.unicast_data; }) - relayed_before == 40);

  // The fault side's next request waits for its first backoff after the fallback;
  // guest traffic keeps flowing because only traffic triggers a retry.
  Clock::time_point retry{};
  const auto retry_deadline = Clock::now() + 6s;
  while (retry == Clock::time_point{} && Clock::now() < retry_deadline) {
    ga.Send(to_b, Payload(index++));
    std::this_thread::sleep_for(50ms);
    retry = relay.Locked([&] {
      const auto found = observed.find(a.ipv4());
      if (found == observed.end())
        return Clock::time_point{};
      const auto& times = relay.requests[found->second];
      const auto later = std::find_if(times.begin(), times.end(),
                                      [&](Clock::time_point time) { return time > a_down; });
      return later == times.end() ? Clock::time_point{} : *later;
    });
  }
  REQUIRE(retry != Clock::time_point{});
  std::printf("direct timing: next attempt %lld ms after the fallback\n",
              Milliseconds(retry - a_down));
  CHECK(retry - a_down >= 1900ms);
  running = false;
}

TEST_CASE("relay clients fall back when only one direction of a direct path fails",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  Client a, b;
  // b drops every received direct datagram two seconds into its path, so a's
  // sends to b are lost while b's sends to a still arrive.
  a.SetDirect(Enabled());
  b.SetDirect(Enabled({30000, 60000, 120000}, 2));
  REQUIRE(a.Start(relay.endpoint(), "oneway", "secret", {}));
  REQUIRE(b.Start(relay.endpoint(), "oneway", "secret", {}));
  Guest ga(a, 1000), gb(b, 1000);
  const relay::Endpoint to_b{b.ipv4(), 1000}, to_a{a.ipv4(), 1000};
  ga.Send(to_b, Payload(0));
  gb.Expect(to_a, Payload(0));
  REQUIRE(WaitFor([&] { return a.stats().direct_peers == 1 && b.stats().direct_peers == 1; }, 6s));
  const auto fault = Clock::now() + 2s;

  std::atomic<bool> running = true;
  std::jthread reader([&](std::stop_token stop) {
    while (running && !stop.stop_requested()) {
      relay::Bytes payload;
      relay::Endpoint from{};
      ga.Receive(payload, from, 5);
      gb.Receive(payload, from, 5);
    }
  });
  Clock::time_point a_down{}, b_down{};
  uint64_t a_in_at_fault = 0, a_in_at_b_down = 0;
  bool at_fault = false;
  uint32_t index = 1;
  auto next_send = Clock::now();
  while (Clock::now() < fault + 10s &&
         (a_down == Clock::time_point{} || b_down == Clock::time_point{})) {
    if (Clock::now() >= next_send) {
      ga.Send(to_b, Payload(index));
      gb.Send(to_a, Payload(index + 1));
      index += 2;
      next_send += 50ms;
    }
    const auto sa = a.stats(), sb = b.stats();
    const auto now = Clock::now();
    if (!at_fault && now >= fault) {
      at_fault = true;
      a_in_at_fault = sa.direct_in;
    }
    if (a_down == Clock::time_point{} && sa.direct_peers == 0)
      a_down = now;
    if (b_down == Clock::time_point{} && sb.direct_peers == 0) {
      b_down = now;
      a_in_at_b_down = sa.direct_in;
    }
    std::this_thread::sleep_for(5ms);
  }
  running = false;
  REQUIRE(a_down != Clock::time_point{});
  REQUIRE(b_down != Clock::time_point{});
  std::printf(
      "direct timing: one-way loss, sender switched %lld ms after the loss, "
      "receiver %lld ms\n",
      Milliseconds(a_down - fault), Milliseconds(b_down - fault));
  // a hears b directly after the loss starts, yet its own sends get no
  // acknowledgements, so received data does not keep its send path up.
  CHECK(a_in_at_b_down > a_in_at_fault);
  CHECK(a_down - fault <= 2500ms);
  CHECK(b_down - fault <= 2500ms);
  CHECK(b.stats().direct_fault_drops > 0);
  CHECK(a.stats().direct_fault_drops == 0);
  CHECK(a.stats().direct_fallbacks == 1);
  CHECK(b.stats().direct_fallbacks == 1);
}

TEST_CASE("relay clients keep relay transport without direct support",
          "[kernel][relay_client][direct][network]") {
  TestRelay relay;
  Client a, b;
  bool a_direct = true, b_direct = true;
  SECTION("option off") {
    a_direct = b_direct = false;
  }
  SECTION("peer without the option") {
    b_direct = false;
  }
  SECTION("relay without direct messages") {
    // Simulate a relay that drops unsupported direct path messages.
    relay.in = [](const relay::Datagram& datagram) {
      return datagram.bytes.size() <= 5 ||
             datagram.bytes[5] < static_cast<uint8_t>(relay::Type::kCandidates);
    };
  }
  a.SetDirect(a_direct ? Enabled() : DirectOptions{});
  b.SetDirect(b_direct ? Enabled() : DirectOptions{});
  REQUIRE(a.Start(relay.endpoint(), "plain", "secret", {}));
  REQUIRE(b.Start(relay.endpoint(), "plain", "secret", {}));
  Guest ga(a, 1000), gb(b, 1000);
  const relay::Endpoint to_b{b.ipv4(), 1000}, to_a{a.ipv4(), 1000};
  const auto start = Clock::now();
  uint32_t sent = 0;
  while (Clock::now() - start < 4s) {
    ga.Send(to_b, Payload(sent));
    gb.Expect(to_a, Payload(sent));
    gb.Send(to_a, Payload(sent));
    ga.Expect(to_b, Payload(sent));
    ++sent;
    std::this_thread::sleep_for(50ms);
  }
  CHECK(relay.Locked([&] { return relay.unicast_data; }) == 2 * sent);
  CHECK(a.stats().direct_peers == 0);
  CHECK(a.stats().direct_out == 0);
  CHECK(b.stats().direct_out == 0);
  const auto counters = relay.Locked([&] { return relay.core.counters(); });
  CHECK(counters.attempts == 0);
  if (!a_direct) {
    CHECK(relay.Locked([&] { return relay.received[relay::Type::kCandidates]; }) == 0);
    CHECK(relay.Locked([&] { return relay.received[relay::Type::kConnectRequest]; }) == 0);
    CHECK(system_link::RelayStatus(a.stats()).find("direct") == std::string::npos);
  } else {
    CHECK(a.stats().direct_connect_timeouts >= 1);
    CHECK(system_link::RelayStatus(a.stats()).ends_with(" (direct: 0 of 1 peers)"));
    if (!b_direct)
      CHECK(counters.connect_drops[static_cast<size_t>(relay::ConnectDrop::kUnavailable)] >= 1);
  }
}

TEST_CASE("relay client selects local candidates for loopback and remote relays",
          "[kernel][relay_client][direct]") {
  const auto loopback = LocalCandidates({htonl(INADDR_LOOPBACK), 30100}, 5000);
  REQUIRE(loopback.size() == 1);
  CHECK(loopback[0] == relay::Candidate{htonl(INADDR_LOOPBACK), 5000});
  const auto remote = LocalCandidates({htonl(0xc0000201), 30100}, 5000);
  CHECK(remote.size() <= relay::kMaxCandidates);
  for (const auto& candidate : remote) {
    const auto host = ntohl(candidate.ipv4);
    CHECK(candidate.port == 5000);
    CHECK((host >> 24) != 127);
    CHECK((host >> 16) != 0xa9fe);
    CHECK_FALSE(IsVirtualAddress(candidate.ipv4));
  }
}
}  // namespace
}  // namespace rex::kernel::xam::relay_link
