#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/system/xsocket.h>
#include "kernel/xam/relay_system_link.h"
#include "kernel/xam/lan_system_link.h"
#include "kernel/xam/system_link_config.h"
#include "core/relay_invite.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#endif

namespace rex::kernel::xam::relay_link {
namespace {
using namespace std::chrono_literals;
using system::XSocket;

bool Ready(XSocket& socket) {
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(socket.native_handle(), &readable);
  timeval timeout{2, 0};
  return select(static_cast<int>(socket.native_handle() + 1), &readable, nullptr, nullptr,
                &timeout) == 1;
}
void Nonblocking(XSocket& socket) {
#if defined(_WIN32)
  u_long enabled = 1;
  REQUIRE(ioctlsocket(socket.native_handle(), FIONBIO, &enabled) == 0);
#else
  REQUIRE(fcntl(socket.native_handle(), F_SETFL, O_NONBLOCK) == 0);
#endif
}

TEST_CASE("relay client envelope round trip and rejection", "[kernel][relay_client]") {
  const relay::Endpoint source{htonl(0xc6120001), 1000};
  relay::Bytes payload{0, 3, 0x12, 0x34, 0x56};
  auto bytes = Wrap(0x123456789abcdef0, source, payload);
  auto decoded = Unwrap(0x123456789abcdef0, bytes);
  REQUIRE(decoded);
  CHECK(decoded->endpoint == source);
  CHECK(decoded->bytes == payload);
  CHECK_FALSE(Unwrap(1, bytes));
  CHECK_FALSE(Unwrap(0, relay::Bytes(kEnvelopeSize - 1)));
  CHECK_FALSE(Unwrap(0, relay::Bytes(kEnvelopeSize + relay::kMaxPayload + 1)));
  CHECK(Wrap(0, source, relay::Bytes(relay::kMaxPayload + 1)).empty());
  REQUIRE(Unwrap(0, Wrap(0, source, relay::Bytes(relay::kMaxPayload))));
  CHECK(Unwrap(0, Wrap(0, source, {}))->bytes.empty());
}

TEST_CASE("relay client port table owns collisions ephemeral and release",
          "[kernel][relay_client]") {
  PortTable table;
  REQUIRE(table.Bind(1, 26900, 31000) == 26900);
  CHECK(table.Bind(2, 26900, 31001).error() == kAddressInUse);
  CHECK(table.Bind(1, 26901, 31000).error() == 10022);
  auto ephemeral = table.Bind(2, 0, 31001);
  REQUIRE(ephemeral);
  CHECK(*ephemeral >= 49152);
  CHECK(table.GuestPort(2) == *ephemeral);
  CHECK(table.LoopbackPort(26900) == 31000);
  table.Release(1);
  REQUIRE(table.Bind(3, 26900, 31002) == 26900);
  table.Clear();
  CHECK(table.GuestPort(2) == 0);
  CHECK(table.LoopbackPort(26900) == 0);
  for (size_t i = 0; i < PortTable::kMaxSockets; ++i)
    REQUIRE(table.Bind(i, 0, 1));
  CHECK(table.Bind(PortTable::kMaxSockets, 0, 1).error() == 10055);
}

TEST_CASE("relay client routes only broadcast and virtual unicast", "[kernel][relay_client]") {
  CHECK(CanRoute(0xffffffff));
  CHECK(CanRoute(htonl(0xc6120001)));
  CHECK(CanRoute(htonl(0xc613ffff)));
  CHECK_FALSE(CanRoute(htonl(0xc6140001)));
  CHECK_FALSE(CanRoute(htonl(0x7f000001)));
  CHECK_FALSE(CanRoute(htonl(0xc0a80101)));
  CHECK_FALSE(CanRoute(0));
}

TEST_CASE("relay client fingerprint is stable and address options are validated",
          "[kernel][relay_client]") {
  auto first = HostFingerprint();
  REQUIRE(first);
  CHECK(*HostFingerprint() == *first);
  CHECK_FALSE(Resolve("localhost"));
  CHECK_FALSE(Resolve("localhost:0"));
  CHECK_FALSE(Resolve("localhost:65536"));
  CHECK_FALSE(Resolve("localhost:123junk"));
  CHECK_FALSE(Resolve(":30100"));
}

TEST_CASE("relay socket classification survives startup and cleanup",
          "[kernel][relay_client][.network]") {
  auto startup = relay::UdpSocket::Open("127.0.0.1", 0);
  REQUIRE(startup);
  lan::LanSystemLinkState state;
  Client client;
  client.RegisterSocket(1, false);
  client.RegisterSocket(2, true);
  client.RegisterSocket(3, true);
  for (int startup = 0; startup < 2; ++startup) {
    CHECK_FALSE(client.IsDatagramSocket(1));
    CHECK(client.IsDatagramSocket(2));
    CHECK(client.IsDatagramSocket(3));
    REQUIRE(state.Configure("198.18.0.1", 1));
    CHECK(state.socket_kind(2) == lan::SocketKind::kUnknown);
    XSocket stream(nullptr);
    REQUIRE(stream.Initialize(XSocket::X_AF_INET, XSocket::X_SOCK_STREAM, XSocket::X_IPPROTO_TCP) ==
            X_STATUS_SUCCESS);
    system::N_XSOCKADDR_IN address{};
    address.sin_family = XSocket::X_AF_INET;
    address.sin_port = 0;
    address.sin_addr = INADDR_ANY;
    REQUIRE(stream.Bind(&address, sizeof(address)) == X_STATUS_SUCCESS);
    uint32_t size = sizeof(address);
    REQUIRE(stream.GetSockName(&address, &size) == X_STATUS_SUCCESS);
    CHECK(static_cast<uint32_t>(address.sin_addr) == INADDR_ANY);
    state.Reset();
    client.Stop();
  }
  client.ForgetSocket(2);
  CHECK_FALSE(client.IsDatagramSocket(2));
}

TEST_CASE("relay client secret file reads retain bounds and trimming", "[kernel][relay_client]") {
  struct File {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("relay-secret-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / "fixture";
    File() { REQUIRE(std::filesystem::create_directory(directory)); }
    ~File() {
      std::error_code error;
      std::filesystem::remove(path, error);
      std::filesystem::remove(directory, error);
    }
  } file;
  auto write = [&](std::string_view bytes) {
    std::ofstream stream(file.path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.write(bytes.data(), bytes.size()));
  };
  CHECK_FALSE(ReadSecret(file.path.string()));
  write(" \t\r\n secret \t\r\n");
  auto secret = ReadSecret(file.path.string());
  REQUIRE(secret);
  CHECK(secret->view() == "secret");
  secret->Clear();
  CHECK(secret->view().empty());
  write(std::string(65536, 'x'));
  auto maximum = ReadSecret(file.path.string());
  REQUIRE(maximum);
  CHECK(maximum->view().size() == 65536);
  write(std::string(65537, 'x'));
  CHECK_FALSE(ReadSecret(file.path.string()));
  write(" \t\r\n");
  CHECK_FALSE(ReadSecret(file.path.string()));
  write("");
  CHECK_FALSE(ReadSecret(file.path.string()));
}

TEST_CASE("relay client tolerates delayed replies and a dropped WELCOME",
          "[kernel][relay_client][network]") {
  auto delay = 300ms;
  bool drop_welcome = false;
  SECTION("300 ms replies") {}
  SECTION("600 ms replies") {
    delay = 600ms;
  }
  SECTION("first WELCOME lost") {
    drop_welcome = true;
  }
  auto server = relay::UdpSocket::Open("127.0.0.1", 0);
  REQUIRE(server);
  relay::Core core({}, "secret", relay::Digest{42});
  std::atomic<unsigned> hellos_after_join = 0;
  std::atomic<unsigned> joins = 0;
  std::atomic<bool> io_failed = false;
  std::jthread pump([&](std::stop_token stop) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::deque<std::pair<Clock::time_point, relay::Datagram>> pending;
    while (!stop.stop_requested()) {
      auto packet = (*server)->Receive(10);
      if (!packet) {
        io_failed = true;
        return;
      }
      if (*packet) {
        auto message = relay::Decode((**packet).bytes);
        if (message && message->type == relay::Type::kHello && joins)
          ++hellos_after_join;
        if (message && message->type == relay::Type::kJoin)
          ++joins;
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        for (auto& reply : core.Input((**packet).endpoint, (**packet).bytes, now.count())) {
          auto decoded = relay::Decode(reply.bytes);
          if (drop_welcome && decoded && decoded->type == relay::Type::kWelcome) {
            drop_welcome = false;
            continue;
          }
          pending.emplace_back(Clock::now() + delay, std::move(reply));
        }
      }
      while (!pending.empty() && pending.front().first <= Clock::now()) {
        if (!(*server)->Send(pending.front().second))
          io_failed = true;
        pending.pop_front();
      }
    }
  });
  Client client;
  auto joined = client.Start((*server)->endpoint(), "delayed", "secret", {});
  INFO((joined ? "joined" : joined.error().message));
  CHECK(joined);
  CHECK(hellos_after_join == 0);
  CHECK(joins <= 3);
  CHECK_FALSE(io_failed);
}

TEST_CASE("relay client loopback discovery and VDP use native select",
          "[kernel][relay_client][network]") {
  auto server = relay::UdpSocket::Open("127.0.0.1", 0);
  if (!server)
    SKIP("Loopback UDP socket unavailable");
  XSocket sweep(nullptr), host_query(nullptr), collision(nullptr), first_vdp(nullptr),
      second_vdp(nullptr);
  for (auto* socket : {&sweep, &host_query, &collision})
    if (socket->Initialize(XSocket::X_AF_INET, XSocket::X_SOCK_DGRAM, XSocket::X_IPPROTO_UDP) !=
        X_STATUS_SUCCESS)
      SKIP("Guest UDP socket unavailable");
  for (auto* socket : {&first_vdp, &second_vdp})
    if (socket->Initialize(XSocket::X_AF_INET, XSocket::X_SOCK_DGRAM, XSocket::X_IPPROTO_VDP) !=
        X_STATUS_SUCCESS)
      SKIP("Guest VDP socket unavailable");
  relay::Core core({}, "secret", relay::Digest{42});
  relay::TransportCounters counters;
  std::atomic<bool> service = true;
  std::atomic<size_t> peers = 0;
  std::jthread pump([&](std::stop_token stop) {
    auto start = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
      auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
      if (!service || !relay::Pump(**server, core, now, counters))
        std::this_thread::sleep_for(1ms);
      peers = core.counters().peers;
    }
  });
  Client first, second, wrong;
  const auto resolved = Resolve("localhost:" + std::to_string((*server)->endpoint().port));
  REQUIRE(resolved);
  CHECK(*resolved == (*server)->endpoint());
  REQUIRE(first.Start(*resolved, "test", "secret", relay::Digest{1}));
  REQUIRE(second.Start((*server)->endpoint(), "test", "secret", relay::Digest{1}));
  auto rejected = wrong.Start((*server)->endpoint(), "test", "wrong", relay::Digest{1});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().message == "wrong secret");
  CHECK(system_link::RelayStatus(wrong.stats()) == "Relay failed: wrong secret");
  CHECK(wrong.Stop().state == Association::kFailed);
  CHECK(system_link::RelayStatus(wrong.stats()) == "Relay failed: wrong secret");
  wrong.Stop();
  CHECK(system_link::RelayStatus(wrong.stats()) == "Relay failed: wrong secret");
  REQUIRE(wrong.Start((*server)->endpoint(), "test", "secret", relay::Digest{1}));
  CHECK(wrong.stats().last_failure.empty());
  CHECK(wrong.Stop().state == Association::kLive);
  CHECK(system_link::RelayStatus(wrong.stats()) == "Relay: disconnected");
  auto bye_deadline = std::chrono::steady_clock::now() + 2s;
  while (peers != 2 && std::chrono::steady_clock::now() < bye_deadline)
    std::this_thread::sleep_for(1ms);
  CHECK(peers == 2);
  CHECK(first.ipv4() != second.ipv4());
  auto original_ip = first.ipv4();
  REQUIRE(first.Start((*server)->endpoint(), "test", "secret", relay::Digest{1}));
  CHECK(first.ipv4() == original_ip);
  REQUIRE(second.Bind(host_query.native_handle(), 26900));
  CHECK(second.Bind(collision.native_handle(), 26900).error() == kAddressInUse);
  REQUIRE(second.Bind(collision.native_handle(), 26901));
  REQUIRE(first.Bind(first_vdp.native_handle(), 1000));
  REQUIRE(second.Bind(second_vdp.native_handle(), 1000));
  REQUIRE(first.Start((*server)->endpoint(), "test", "secret", relay::Digest{1}));
  CHECK(first.GuestPort(first_vdp.native_handle()) == 1000);

  auto receive = [&](Client& client, XSocket& socket, relay::Endpoint expected,
                     const relay::Bytes& payload) {
    REQUIRE(Ready(socket));
    std::array<uint8_t, relay::kMaxPayload> buffer{};
    relay::Endpoint source;
    auto count = client.Receive(socket.native_handle(), buffer, 0, source);
    REQUIRE(count);
    CHECK(static_cast<size_t>(*count) == payload.size());
    CHECK(source == expected);
    CHECK(relay::Bytes(buffer.begin(), buffer.begin() + *count) == payload);
  };
  const relay::Bytes query{0xfe, 0xfd, 0, 1, 2, 3};
  REQUIRE(first.Send(sweep.native_handle(), {0xffffffff, 26900}, query) == query.size());
  const auto sweep_port = first.GuestPort(sweep.native_handle());
  REQUIRE(sweep_port >= 49152);
  receive(second, host_query, {first.ipv4(), sweep_port}, query);
  REQUIRE(second.Send(host_query.native_handle(), {0xffffffff, sweep_port}, query));
  receive(first, sweep, {second.ipv4(), 26900}, query);

  const relay::Bytes vdp{0, 3, 0xab, 0xcd, 0xef};
  REQUIRE(first.Send(first_vdp.native_handle(), {second.ipv4(), 1000}, vdp));
  receive(second, second_vdp, {first.ipv4(), 1000}, vdp);
  REQUIRE(second.Send(second_vdp.native_handle(), {first.ipv4(), 1000}, vdp));
  receive(first, first_vdp, {second.ipv4(), 1000}, vdp);
  CHECK(first.Send(sweep.native_handle(), {htonl(INADDR_LOOPBACK), 26900}, query).error() ==
        kNetworkUnreachable);
  CHECK(first.Send(sweep.native_handle(), {second.ipv4(), 1000}, relay::Bytes(1201)).error() ==
        kMessageSize);

  // An unrelated local sender cannot feed guest data, even with a valid envelope shape.
  system::N_XSOCKADDR_IN loopback{};
  uint32_t address_size = sizeof(loopback);
  REQUIRE(host_query.GetSockName(&loopback, &address_size) == X_STATUS_SUCCESS);
  CHECK(static_cast<uint32_t>(loopback.sin_addr) == INADDR_LOOPBACK);
  auto forgery = Wrap(0, {first.ipv4(), 1000}, query);
  // A blocking receive consumes the forgery and waits for the later valid payload.
#if defined(_WIN32)
  DWORD receive_timeout = 2000;
#else
  timeval receive_timeout{2, 0};
#endif
  REQUIRE(setsockopt(host_query.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&receive_timeout),
                     sizeof(receive_timeout)) == 0);
  REQUIRE((*server)->Send({{htonl(INADDR_LOOPBACK), loopback.sin_port}, forgery}));
  auto blocking = std::async(std::launch::async, [&] {
    std::array<uint8_t, 64> bytes{};
    relay::Endpoint from{};
    return second.Receive(host_query.native_handle(), bytes, 0, from);
  });
  CHECK(blocking.wait_for(100ms) == std::future_status::timeout);
  REQUIRE(first.Send(sweep.native_handle(), {second.ipv4(), 26900}, query));
  CHECK(blocking.get() == query.size());
  REQUIRE((*server)->Send({{htonl(INADDR_LOOPBACK), loopback.sin_port}, forgery}));
  REQUIRE(Ready(host_query));
  Nonblocking(host_query);
  std::array<uint8_t, 64> buffer{};
  relay::Endpoint source{htonl(0xc612ffff), 4321};
  const auto untouched = source;
  CHECK(second.Receive(host_query.native_handle(), buffer, MSG_PEEK, source).error() ==
        kWouldBlock);
  CHECK(source == untouched);
  CHECK(second.stats().invalid_envelope == 2);
  system::N_XSOCKADDR_IN from;
  std::memset(&from, 0x5a, sizeof(from));
  const auto original_from = from;
  uint32_t from_size = 123;
  CHECK(second.ReceiveFrom(host_query.native_handle(), buffer, 0, from, from_size).error() ==
        kWouldBlock);
  CHECK(std::memcmp(&from, &original_from, sizeof(from)) == 0);
  CHECK(from_size == 123);
  REQUIRE(first.Send(sweep.native_handle(), {second.ipv4(), 26900}, query));
  REQUIRE(Ready(host_query));
  REQUIRE(second.Receive(host_query.native_handle(), buffer, MSG_PEEK, source) == query.size());
  REQUIRE(Ready(host_query));
  std::array<uint8_t, 2> short_buffer{};
  auto truncated = second.Receive(host_query.native_handle(), short_buffer, 0, source);
#if defined(_WIN32)
  CHECK(truncated.error() == kMessageSize);
#else
  CHECK(truncated == short_buffer.size());
#endif
  CHECK(short_buffer[0] == query[0]);
  CHECK(short_buffer[1] == query[1]);
  CHECK(source == relay::Endpoint{first.ipv4(), sweep_port});
  CHECK(second.Receive(host_query.native_handle(), buffer, 0, source).error() == kWouldBlock);
  REQUIRE(first.Send(sweep.native_handle(), {second.ipv4(), 65500}, query));
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while ((second.stats().unknown_port == 0 || first.stats().heartbeats == 0) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(2ms);
  CHECK(second.stats().unknown_port == 1);
  CHECK(first.stats().heartbeats >= 1);
  second.Release(collision.native_handle());
  CHECK(second.GuestPort(collision.native_handle()) == 0);
  // Stop servicing the server to prove association loss without a fallback send.
  service = false;
  const auto loss_deadline = std::chrono::steady_clock::now() + 11s;
  while (first.stats().state == Association::kLive &&
         std::chrono::steady_clock::now() < loss_deadline)
    std::this_thread::sleep_for(5ms);
  CHECK(first.stats().state == Association::kLost);
  CHECK(system_link::RelayStatus(first.stats()) == "Relay: lost connection");
  CHECK(first.Send(sweep.native_handle(), {second.ipv4(), 1000}, query).error() == kNetworkDown);
  system::N_XSOCKADDR_IN original_binding{};
  address_size = sizeof(original_binding);
  REQUIRE(sweep.GetSockName(&original_binding, &address_size) == X_STATUS_SUCCESS);
  service = true;
  // Reserve the first address in a new room so rejoining also changes identity.
  REQUIRE(wrong.Start((*server)->endpoint(), "reassociated", "secret", relay::Digest{1}));
  REQUIRE_FALSE(first.Start((*server)->endpoint(), "reassociated", "wrong", relay::Digest{1}));
  CHECK(first.stats().state == Association::kFailed);
  CHECK(first.GuestPort(sweep.native_handle()) == sweep_port);
  REQUIRE(first.Start((*server)->endpoint(), "reassociated", "secret", relay::Digest{1}));
  CHECK(first.stats().state == Association::kLive);
  CHECK(first.ipv4() != original_ip);
  CHECK(first.GuestPort(sweep.native_handle()) == sweep_port);
  CHECK(first.GuestPort(first_vdp.native_handle()) == 1000);
  system::N_XSOCKADDR_IN rebound{};
  address_size = sizeof(rebound);
  REQUIRE(sweep.GetSockName(&rebound, &address_size) == X_STATUS_SUCCESS);
  CHECK(rebound.sin_addr == original_binding.sin_addr);
  CHECK(rebound.sin_port == original_binding.sin_port);
  REQUIRE(first.Send(sweep.native_handle(), {first.ipv4(), 1000}, query));
  receive(first, first_vdp, {first.ipv4(), sweep_port}, query);
  REQUIRE(first.Send(first_vdp.native_handle(), {first.ipv4(), sweep_port}, vdp));
  receive(first, sweep, {first.ipv4(), 1000}, vdp);
  first.Stop();
  second.Stop();
  CHECK(first.ipv4() == 0);
  CHECK(first.GuestPort(sweep.native_handle()) == 0);
  CHECK(first.stats().state == Association::kStopped);
  // A send on a socket still open after the final cleanup is dropped, not failed.
  const auto after_cleanup = first.Send(first_vdp.native_handle(), {0xFFFFFFFFu, 26900}, vdp);
  REQUIRE(after_cleanup);
  CHECK(*after_cleanup == static_cast<int>(vdp.size()));
  CHECK(first.stats().dropped_after_cleanup == 1);
  pump.request_stop();
  pump.join();
  CHECK(counters.receive_errors == 0);
  CHECK(counters.send_errors == 0);
}

TEST_CASE("relay client handshake times out without a server", "[kernel][relay_client][network]") {
  auto sink = relay::UdpSocket::Open("127.0.0.1", 0);
  if (!sink)
    SKIP("Loopback UDP socket unavailable");
  Client client;
  const auto start = std::chrono::steady_clock::now();
  CHECK(system_link::RelayStatus(client.stats()) == "Relay: waiting for System Link");
  client.Stop();
  CHECK(system_link::RelayStatus(client.stats()) == "Relay: waiting for System Link");
  auto joined = client.Start((*sink)->endpoint(), "test", "secret", {});
  CHECK_FALSE(joined);
  CHECK(std::chrono::steady_clock::now() - start < 6s);
  CHECK(client.stats().state == Association::kFailed);
  CHECK(system_link::RelayStatus(client.stats()) ==
        "Relay failed: relay not reachable (handshake timeout)");
  client.Stop();
  CHECK(system_link::RelayStatus(client.stats()) ==
        "Relay failed: relay not reachable (handshake timeout)");
  unsigned hellos = 0;
  for (;;) {
    auto packet = (*sink)->Receive(0);
    REQUIRE(packet);
    if (!*packet)
      break;
    auto message = relay::Decode((**packet).bytes);
    REQUIRE(message);
    CHECK(message->type == relay::Type::kHello);
    ++hellos;
  }
  CHECK(hellos >= 2);
}

TEST_CASE("invite secret file authenticates a decoded client against the relay",
          "[kernel][relay_client][invite][network]") {
  struct File {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("relay-invite-secret-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / "fixture";
    File() { REQUIRE(std::filesystem::create_directory(directory)); }
    ~File() {
      std::error_code error;
      std::filesystem::remove(path, error);
      std::filesystem::remove(directory, error);
    }
  } file;
  {
    std::ofstream stream(file.path, std::ios::binary);
    stream << " \t\r\n0123456789ABCDEF\v\f ";
  }
  auto server = relay::UdpSocket::Open("127.0.0.1", 0);
  REQUIRE(server);
  auto code =
      relay::MakeInvite("127.0.0.1:" + std::to_string((*server)->endpoint().port), file.path);
  REQUIRE(code);
  auto invite = relay::DecodeInvite(*code);
  REQUIRE(invite);
  auto file_secret = ReadSecret(file.path.string());
  REQUIRE(file_secret);
  CHECK(relay::JoinAuth(invite->SecretView(), {}, {}, invite->room, {}) ==
        relay::JoinAuth(file_secret->view(), {}, {}, invite->room, {}));
  relay::Core core({1, 1}, std::string(file_secret->view()), relay::Digest{42});
  relay::TransportCounters counters;
  std::jthread pump([&](std::stop_token stop) {
    const auto start = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
      auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start);
      relay::Pump(**server, core, now.count(), counters, 10);
    }
  });
  Client first, other;
  REQUIRE(first.Start((*server)->endpoint(), invite->room, Secret(invite->SecretView()), {}));
  CHECK(system_link::RelayStatus(first.stats()) == "Relay: connected as 198.18.0.1");
  auto failed = other.Start((*server)->endpoint(), invite->room, Secret(invite->SecretView()), {});
  CHECK_FALSE(failed);
  CHECK(system_link::RelayStatus(other.stats()) == "Relay failed: room full");
  failed = other.Start((*server)->endpoint(), "different", Secret(invite->SecretView()), {});
  CHECK_FALSE(failed);
  CHECK(system_link::RelayStatus(other.stats()) == "Relay failed: too many rooms");
  failed = other.Start((*server)->endpoint(), invite->room, Secret(invite->SecretView()),
                       relay::Digest{1});
  CHECK_FALSE(failed);
  CHECK(system_link::RelayStatus(other.stats()) == "Relay failed: different game build");
}

TEST_CASE("relay status records all wire REJECT reasons", "[kernel][relay_client][network]") {
  for (const auto& [reason, text] : {std::pair{relay::Reason::kVersion, "protocol version"},
                                     {relay::Reason::kMalformed, "malformed handshake"},
                                     {relay::Reason::kAuth, "wrong secret"},
                                     {relay::Reason::kRoomFull, "room full"},
                                     {relay::Reason::kTooManyRooms, "too many rooms"},
                                     {relay::Reason::kFingerprintMismatch, "different game build"},
                                     {relay::Reason::kRateLimited, "rate limited"}}) {
    auto server = relay::UdpSocket::Open("127.0.0.1", 0);
    REQUIRE(server);
    std::jthread pump([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        auto packet = (*server)->Receive(10);
        if (packet && *packet) {
          relay::Message reject;
          reject.type = relay::Type::kReject;
          reject.reason = reason;
          auto wire = relay::Encode(reject);
          if (wire)
            (*server)->Send({(**packet).endpoint, *wire});
        }
      }
    });
    Client client;
    CHECK_FALSE(client.Start((*server)->endpoint(), "test", "secret", {}));
    CHECK(system_link::RelayStatus(client.stats()) == std::string("Relay failed: ") + text);
  }
}

TEST_CASE("relay clients publish, search, look up, tokenize and remove sessions",
          "[kernel][relay_client][directory][network]") {
  auto server = relay::UdpSocket::Open("127.0.0.1", 0);
  if (!server)
    SKIP("Loopback UDP socket unavailable");
  const auto endpoint = (*server)->endpoint();
  relay::Core core({}, "secret", relay::Digest{42});
  relay::TransportCounters counters;
  std::atomic<bool> service = true;
  std::jthread pump([&](std::stop_token stop) {
    const auto start = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
      if (!service) {
        std::this_thread::sleep_for(1ms);
        continue;
      }
      auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start);
      relay::Pump(**server, core, now.count(), counters, 10);
    }
  });
  auto records = [](Client& client, RequestHandle handle) {
    REQUIRE(handle);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      auto poll = client.PollRecords(handle);
      if (poll.state != RequestState::kPending)
        return poll;
      std::this_thread::sleep_for(1ms);
    }
    FAIL("Directory request did not complete");
    return RecordsPoll{};
  };
  auto token = [](Client& client, RequestHandle handle) {
    REQUIRE(handle);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      auto poll = client.PollToken(handle);
      if (poll.state != RequestState::kPending)
        return poll;
      std::this_thread::sleep_for(1ms);
    }
    FAIL("Token request did not complete");
    return TokenPoll{};
  };
  Client host, other, guest;
  REQUIRE(host.Start(endpoint, "live", "secret", {}));
  REQUIRE(other.Start(endpoint, "live", "secret", {}));
  relay::SessionRecord record;
  record.session_id = 0x1122334455667788;
  record.key.fill(0x6b);
  record.flags = 0x2f;
  record.max_public = 4;
  record.open_public = 3;
  record.properties = {0x09, 0x81, 0x00, 0x40};
  record.contexts = {0x0b, 0x80, 0x00, 0x00};
  record.qos = {1, 2, 3};
  CHECK_FALSE(host.PublishSession(relay::SessionRecord{}));
  REQUIRE(host.PublishSession(record));

  RecordsPoll found;
  const auto publish_deadline = std::chrono::steady_clock::now() + 5s;
  do {
    found = records(other, other.BeginSearch(200, 0x08));
    REQUIRE(found.state == RequestState::kDone);
    if (found.records.empty())
      std::this_thread::sleep_for(300ms);
  } while (found.records.empty() && std::chrono::steady_clock::now() < publish_deadline);
  REQUIRE(found.records.size() == 1);
  const auto& listed = found.records[0];
  CHECK(listed.session_id == record.session_id);
  CHECK(listed.host_ipv4 == host.ipv4());
  CHECK(listed.host_peer_id != 0);
  CHECK(listed.key == record.key);
  CHECK(listed.properties == record.properties);
  CHECK(listed.contexts == record.contexts);
  CHECK(listed.qos == record.qos);
  CHECK(records(host, host.BeginSearch(200, 0)).records.empty());
  const auto lookup = records(other, other.BeginLookup(record.session_id));
  REQUIRE(lookup.records.size() == 1);
  CHECK(lookup.records[0] == listed);

  const auto denied = token(other, other.BeginTokenRequest(record.session_id));
  CHECK(denied.state == RequestState::kFailed);
  CHECK(denied.failure == RequestFailure::kRejected);
  auto granted = token(host, host.BeginTokenRequest(record.session_id));
  REQUIRE(granted.state == RequestState::kDone);
  REQUIRE(granted.token);
  CHECK(granted.token->expiry_seconds == 15 * 60);
  CHECK(granted.token->secret.view().size() == relay::Digest{}.size());
  REQUIRE(guest.Start(endpoint, std::move(*granted.token), {}));
  CHECK(guest.ipv4() != host.ipv4());
  CHECK(guest.ipv4() != other.ipv4());
  CHECK(records(guest, guest.BeginLookup(record.session_id)).records.size() == 1);
  guest.Stop();

  REQUIRE(host.RemoveSession(record.session_id));
  const auto remove_deadline = std::chrono::steady_clock::now() + 5s;
  do {
    found = records(other, other.BeginLookup(record.session_id));
    if (!found.records.empty())
      std::this_thread::sleep_for(300ms);
  } while (!found.records.empty() && std::chrono::steady_clock::now() < remove_deadline);
  CHECK(found.state == RequestState::kDone);
  CHECK(found.records.empty());
  CHECK(host.stats().directory_failed == 0);

  // Requests time out without a reply and fail on association loss.
  service = false;
  std::this_thread::sleep_for(100ms);  // Let an in-flight 10 ms pump receive finish.
  const auto start = std::chrono::steady_clock::now();
  const auto timed_out = records(host, host.BeginSearch(1, 0));
  CHECK(timed_out.failure == RequestFailure::kTimeout);
  CHECK(std::chrono::steady_clock::now() - start >= 2900ms);
  CHECK(host.stats().requests_failed == 1);
  const auto pending = other.BeginLookup(record.session_id);
  CHECK(other.PollRecords(pending).state == RequestState::kPending);
  CHECK(other.PollToken(pending).failure == RequestFailure::kInvalid);
  other.Stop();
  const auto lost = other.PollRecords(pending);
  CHECK(lost.state == RequestState::kFailed);
  CHECK(lost.failure == RequestFailure::kAssociation);
  CHECK(other.PollRecords(pending).failure == RequestFailure::kInvalid);
  CHECK_FALSE(other.PublishSession(record));
  CHECK_FALSE(other.RemoveSession(record.session_id));
  CHECK(other.PollRecords(other.BeginSearch(1, 0)).failure == RequestFailure::kAssociation);
  CHECK(other.PollToken(other.BeginTokenRequest(0)).failure == RequestFailure::kAssociation);
  host.Stop();
  pump.request_stop();
  pump.join();
  CHECK(counters.receive_errors == 0);
  CHECK(counters.send_errors == 0);
}

// A loopback relay whose replies pass through a test filter before sending.
struct FilteredRelay {
  using Filter = std::function<void(std::vector<relay::Datagram>&)>;
  std::unique_ptr<relay::UdpSocket> socket;
  relay::Core core{{}, "secret", relay::Digest{42}};
  std::mutex mutex;
  Filter filter;
  std::atomic<bool> service = true;
  std::jthread pump;

  FilteredRelay() {
    auto opened = relay::UdpSocket::Open("127.0.0.1", 0);
    REQUIRE(opened);
    socket = std::move(*opened);
    pump = std::jthread([this](std::stop_token stop) {
      const auto start = std::chrono::steady_clock::now();
      while (!stop.stop_requested()) {
        if (!service) {
          std::this_thread::sleep_for(1ms);
          continue;
        }
        auto packet = socket->Receive(10);
        if (!packet || !*packet)
          continue;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
        std::vector<relay::Datagram> output;
        {
          std::lock_guard lock(mutex);
          output = core.Input((**packet).endpoint, (**packet).bytes, now);
          if (filter)
            filter(output);
        }
        for (const auto& datagram : output)
          (void)socket->Send(datagram);
      }
    });
  }
  ~FilteredRelay() {
    pump.request_stop();
    pump.join();
  }
  relay::Endpoint endpoint() const { return socket->endpoint(); }
  void SetFilter(Filter next) {
    std::lock_guard lock(mutex);
    filter = std::move(next);
  }
  relay::Counters counters() {
    std::lock_guard lock(mutex);
    return core.counters();
  }
  // Stops serving and waits out an in-flight receive.
  void Pause() {
    service = false;
    std::this_thread::sleep_for(100ms);
  }
};

relay::Type TypeOf(const relay::Datagram& datagram) {
  return static_cast<relay::Type>(datagram.bytes[5]);
}

RecordsPoll WaitRecords(Client& client, RequestHandle handle) {
  REQUIRE(handle);
  const auto deadline = std::chrono::steady_clock::now() + 6s;
  while (std::chrono::steady_clock::now() < deadline) {
    auto poll = client.PollRecords(handle);
    if (poll.state != RequestState::kPending)
      return poll;
    std::this_thread::sleep_for(1ms);
  }
  FAIL("Directory request did not complete");
  return {};
}

TokenPoll WaitToken(Client& client, RequestHandle handle) {
  REQUIRE(handle);
  const auto deadline = std::chrono::steady_clock::now() + 6s;
  while (std::chrono::steady_clock::now() < deadline) {
    auto poll = client.PollToken(handle);
    if (poll.state != RequestState::kPending)
      return poll;
    std::this_thread::sleep_for(1ms);
  }
  FAIL("Token request did not complete");
  return {};
}

relay::SessionRecord LargeRecord(uint64_t id) {
  relay::SessionRecord record;
  record.session_id = id;
  record.max_public = 4;
  record.open_public = 2;
  record.properties.assign(relay::kMaxRecordProperties, static_cast<uint8_t>(id));
  record.contexts.assign(relay::kMaxRecordContexts, 2);
  record.qos.assign(relay::kMaxRecordQos, 3);
  return record;
}

// Waits until other's lookup sees the session, proving the PUBLISH was applied.
void WaitPublished(Client& other, uint64_t session_id) {
  const auto deadline = std::chrono::steady_clock::now() + 6s;
  for (;;) {
    const auto found = WaitRecords(other, other.BeginLookup(session_id));
    REQUIRE(found.state == RequestState::kDone);
    if (!found.records.empty())
      return;
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(300ms);
  }
}

TEST_CASE("relay client retries lost directory replies with stable request ids",
          "[kernel][relay_client][directory][network]") {
  FilteredRelay relay;
  Client host, other;
  REQUIRE(host.Start(relay.endpoint(), "loss", "secret", {}));
  REQUIRE(other.Start(relay.endpoint(), "loss", "secret", {}));
  struct Losses {
    unsigned status = 1, grant = 1, results = 0;
    relay::TokenId dropped_token{};
  } losses;
  relay.SetFilter([&](std::vector<relay::Datagram>& output) {
    std::erase_if(output, [&](const relay::Datagram& datagram) {
      const auto type = TypeOf(datagram);
      if (type == relay::Type::kStatus && losses.status) {
        --losses.status;
        return true;
      }
      if (type == relay::Type::kTokenGrant && losses.grant) {
        --losses.grant;
        const auto grant = relay::Decode(datagram.bytes);
        losses.dropped_token = grant->join_token;
        return true;
      }
      if (type == relay::Type::kResults && losses.results) {
        --losses.results;
        return true;
      }
      return false;
    });
  });
  auto record = LargeRecord(42);
  REQUIRE(host.PublishSession(record));
  WaitPublished(other, record.session_id);
  // The lost STATUS is recovered by retrying the same update sequence.
  CHECK(relay.counters().sessions == 1);

  auto granted = WaitToken(host, host.BeginTokenRequest(record.session_id));
  REQUIRE(granted.state == RequestState::kDone);
  REQUIRE(granted.token);
  CHECK(granted.token->id == losses.dropped_token);
  CHECK(relay.counters().tokens == 1);

  {
    std::lock_guard lock(relay.mutex);
    losses.results = 1;
  }
  const auto lookup = WaitRecords(other, other.BeginLookup(record.session_id));
  CHECK(lookup.state == RequestState::kDone);
  CHECK(lookup.records.size() == 1);
  // A later update still applies after the retried one.
  REQUIRE(host.RemoveSession(record.session_id));
  const auto removed = std::chrono::steady_clock::now() + 3s;
  while (relay.counters().sessions && std::chrono::steady_clock::now() < removed)
    std::this_thread::sleep_for(1ms);
  CHECK(relay.counters().sessions == 0);
  CHECK(relay.counters().tokens == 0);
  CHECK(host.stats().directory_failed == 0);
  CHECK(host.stats().requests_failed == 0);
  CHECK(other.stats().requests_failed == 0);
  relay.SetFilter({});
}

TEST_CASE("relay client reassembles reordered and duplicated RESULTS pages once",
          "[kernel][relay_client][directory][network]") {
  FilteredRelay relay;
  Client host, other;
  REQUIRE(host.Start(relay.endpoint(), "pages", "secret", {}));
  REQUIRE(other.Start(relay.endpoint(), "pages", "secret", {}));
  REQUIRE(host.PublishSession(LargeRecord(1)));
  REQUIRE(host.PublishSession(LargeRecord(2)));
  WaitPublished(other, 2);
  auto ids = [](const RecordsPoll& poll) {
    std::vector<uint64_t> result;
    for (const auto& record : poll.records)
      result.push_back(record.session_id);
    return result;
  };

  // In order, each largest record fills its own page.
  size_t pages = 0;
  relay.SetFilter([&](std::vector<relay::Datagram>& output) {
    if (!output.empty() && TypeOf(output[0]) == relay::Type::kResults)
      pages = output.size();
  });
  auto found = WaitRecords(other, other.BeginSearch(200, 0));
  REQUIRE(found.state == RequestState::kDone);
  CHECK(pages == 2);
  CHECK(ids(found) == std::vector<uint64_t>{2, 1});
  CHECK(found.records[0].properties == LargeRecord(2).properties);

  relay.SetFilter([](std::vector<relay::Datagram>& output) {
    if (output.size() < 2 || TypeOf(output[0]) != relay::Type::kResults)
      return;
    std::ranges::reverse(output);
    const auto copy = output;
    output.insert(output.end(), copy.begin(), copy.end());
  });
  found = WaitRecords(other, other.BeginSearch(200, 0));
  REQUIRE(found.state == RequestState::kDone);
  CHECK(ids(found) == std::vector<uint64_t>{2, 1});
  CHECK(other.stats().requests_failed == 0);
  relay.SetFilter({});
}

TEST_CASE("relay client pages follow the newest directory snapshot",
          "[kernel][relay_client][directory][network]") {
  FilteredRelay relay;
  Client host, other;
  REQUIRE(host.Start(relay.endpoint(), "snapshot", "secret", {}));
  REQUIRE(other.Start(relay.endpoint(), "snapshot", "secret", {}));
  REQUIRE(host.PublishSession(LargeRecord(1)));
  REQUIRE(host.PublishSession(LargeRecord(2)));
  WaitPublished(other, 2);
  // Each search attempt relabels its two pages with a snapshot, or drops one (-1).
  std::vector<std::array<int64_t, 2>> attempts;
  size_t seen = 0;
  SECTION("a newer snapshot discards collected pages") {
    attempts = {{{100, -1}}, {{-1, 101}}, {{101, 101}}};
  }
  SECTION("an older snapshot is ignored") {
    attempts = {{{101, -1}}, {{100, 100}}, {{101, 101}}};
  }
  relay.SetFilter([&](std::vector<relay::Datagram>& output) {
    if (output.size() != 2 || TypeOf(output[0]) != relay::Type::kResults ||
        seen == attempts.size())
      return;
    const auto& plan = attempts[seen++];
    std::vector<relay::Datagram> kept;
    for (size_t page = 0; page < output.size(); ++page) {
      if (plan[page] < 0)
        continue;
      auto datagram = output[page];
      for (size_t i = 0; i < 4; ++i)
        datagram.bytes[12 + i] = static_cast<uint8_t>(uint32_t(plan[page]) >> (8 * i));
      kept.push_back(std::move(datagram));
    }
    output = std::move(kept);
  });
  const auto found = WaitRecords(other, other.BeginSearch(200, 0));
  REQUIRE(found.state == RequestState::kDone);
  REQUIRE(found.records.size() == 2);
  CHECK(found.records[0].session_id == 2);
  CHECK(found.records[1].session_id == 1);
  {
    // Mixing snapshots would have completed on the second attempt.
    std::lock_guard lock(relay.mutex);
    CHECK(seen == 3);
  }
  relay.SetFilter({});
}

TEST_CASE("relay client bounds requests and updates and fails them on loss",
          "[kernel][relay_client][directory][network]") {
  FilteredRelay relay;
  Client host, other;
  REQUIRE(host.Start(relay.endpoint(), "bounds", "secret", {}));
  REQUIRE(other.Start(relay.endpoint(), "bounds", "secret", {}));
  // Make the last relay datagram recent so the loss deadline is predictable.
  CHECK(WaitRecords(host, host.BeginLookup(1)).state == RequestState::kDone);
  relay.Pause();
  const auto paused = std::chrono::steady_clock::now();

  std::vector<RequestHandle> handles;
  for (size_t i = 0; i < kMaxRequests; ++i) {
    handles.push_back(other.BeginSearch(1, 0));
    REQUIRE(handles.back());
  }
  CHECK(other.BeginLookup(1) == 0);
  for (size_t i = 0; i < kMaxUpdates; ++i)
    REQUIRE(other.PublishSession(LargeRecord(i + 1)));
  CHECK_FALSE(other.PublishSession(LargeRecord(100)));
  CHECK_FALSE(other.RemoveSession(1));
  other.Stop();
  for (const auto handle : handles) {
    const auto poll = other.PollRecords(handle);
    CHECK(poll.state == RequestState::kFailed);
    CHECK(poll.failure == RequestFailure::kAssociation);
  }
  CHECK(other.stats().state == Association::kStopped);

  // A request still pending when the association is lost fails with it.
  std::this_thread::sleep_until(paused + 8500ms);
  REQUIRE(host.stats().state == Association::kLive);
  const auto pending = host.BeginSearch(1, 0);
  REQUIRE(pending);
  const auto lost = WaitRecords(host, pending);
  CHECK(host.stats().state == Association::kLost);
  CHECK(lost.state == RequestState::kFailed);
  CHECK(lost.failure == RequestFailure::kAssociation);
  CHECK_FALSE(host.PublishSession(LargeRecord(1)));
}
}  // namespace
}  // namespace rex::kernel::xam::relay_link
