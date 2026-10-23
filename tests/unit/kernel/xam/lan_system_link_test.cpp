#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xmemory.h>
#include <rex/system/xsocket.h>

#include "kernel/xam/lan_system_link.h"
#include "test_memory.h"

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::kernel::xam::lan {
namespace {

TEST_CASE("LAN remains disabled by default", "[kernel][lan]") {
  CHECK_FALSE(REXCVAR_QUERY(bool, system_link_lan_enabled));

  rex::system::xam::UserProfile profile;
  CHECK(profile.xuid() == 0xB13EBABEBABEBABE);

  LanSystemLinkState state;
  CHECK_FALSE(state.enabled());
  CHECK(state.SelectBindIpv4(0) == 0);
  CHECK(state.UpdateQos({}, 0, kQosListenEnable) == QosUpdateResult::kNotConfigured);
}

TEST_CASE("LAN configuration is explicit and rejects unusable identities", "[kernel][lan]") {
  LanSystemLinkState state;

  CHECK_FALSE(state.Configure("", 1));
  CHECK_FALSE(state.Configure("not-an-address", 1));
  CHECK_FALSE(state.Configure("0.0.0.0", 1));
  CHECK_FALSE(state.Configure("10.0.0.1", 0));
  CHECK_FALSE(state.enabled());
}

TEST_CASE("LAN instances expose distinct configured identities", "[kernel][lan]") {
  LanSystemLinkState host;
  LanSystemLinkState client;

  REQUIRE(host.Configure("10.0.0.136", 0xB13E000000000001));
  REQUIRE(client.Configure("10.0.0.28", 0xB13E000000000002));

  CHECK(host.local_xuid() != client.local_xuid());
  CHECK(host.local_xnaddr().ipv4 != client.local_xnaddr().ipv4);
  CHECK(host.local_xnaddr().ethernet != client.local_xnaddr().ethernet);
}

TEST_CASE("LAN wildcard binds select the configured interface", "[kernel][lan]") {
  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.136", 0xB13E000000000001));

  CHECK(state.SelectBindIpv4(0) == state.local_ipv4());
  CHECK(state.SelectBindIpv4(0x10203040) == 0x10203040);
}

TEST_CASE("LAN configured XUID reaches the user profile", "[kernel][lan]") {
  rex::cvar::testing::ScopedLifecycleOverride lifecycle_override;
  REQUIRE(rex::cvar::SetFlagByName("system_link_lan_enabled", "true"));
  REQUIRE(rex::cvar::SetFlagByName("system_link_xuid", "12771645593269305345"));

  rex::system::xam::UserProfile profile;
  CHECK(profile.xuid() == 0xB13E000000000001);

  REQUIRE(rex::cvar::SetFlagByName("system_link_lan_enabled", "false"));
  REQUIRE(rex::cvar::SetFlagByName("system_link_xuid", "0"));
}

TEST_CASE("LAN configured name reaches the user profile", "[kernel][lan]") {
  rex::cvar::testing::ScopedLifecycleOverride lifecycle_override;
  REQUIRE(rex::cvar::SetFlagByName("system_link_lan_enabled", "true"));
  REQUIRE(rex::cvar::SetFlagByName("system_link_profile_name", "LAN Host"));

  rex::system::xam::UserProfile profile;
  CHECK(profile.name() == "LAN Host");

  CHECK_FALSE(rex::cvar::SetFlagByName("system_link_profile_name", "1234567890123456"));
  rex::system::xam::UserProfile truncated_profile;
  CHECK(truncated_profile.name() == "LAN Host");

  REQUIRE(rex::cvar::SetFlagByName("system_link_lan_enabled", "false"));
  REQUIRE(rex::cvar::SetFlagByName("system_link_profile_name", ""));
}

TEST_CASE("LAN XNADDR conversion round-trips only registered addresses", "[kernel][lan]") {
  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.28", 0xB13E000000000002));

  XnAddrSnapshot peer{};
  peer.ipv4 = state.local_xnaddr().ipv4;
  peer.online_ipv4 = 0x10203040;
  peer.online_port = 1000;
  peer.ethernet = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
  peer.online.fill(0x5A);

  uint32_t converted = 0;
  REQUIRE(state.XnAddrToInAddr(peer, &converted));
  CHECK(converted == peer.ipv4);

  XnAddrSnapshot restored{};
  REQUIRE(state.InAddrToXnAddr(converted, &restored));
  CHECK(restored == peer);
  CHECK_FALSE(state.InAddrToXnAddr(converted + 1, &restored));
}

TEST_CASE("LAN QoS listener retains an opaque 72-byte lifecycle", "[kernel][lan]") {
  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.136", 0xB13E000000000001));
  std::array<uint8_t, 72> payload{};
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i);
  }

  REQUIRE(state.UpdateQos({}, 0, kQosListenEnable) == QosUpdateResult::kSuccess);
  REQUIRE(state.UpdateQos(payload, 0, kQosListenSetData) == QosUpdateResult::kSuccess);
  auto qos = state.qos_snapshot();
  CHECK(qos.enabled);
  CHECK(qos.payload == std::vector<uint8_t>(payload.begin(), payload.end()));

  REQUIRE(state.UpdateQos({}, 0, kQosListenDisable) == QosUpdateResult::kSuccess);
  qos = state.qos_snapshot();
  CHECK_FALSE(qos.enabled);
  CHECK(qos.payload == std::vector<uint8_t>(payload.begin(), payload.end()));

  REQUIRE(state.UpdateQos({}, 0, kQosListenRelease) == QosUpdateResult::kSuccess);
  qos = state.qos_snapshot();
  CHECK_FALSE(qos.enabled);
  CHECK(qos.payload.empty());

  std::array<uint8_t, LanSystemLinkState::kMaxQosPayloadSize + 1> oversized{};
  CHECK(state.UpdateQos(oversized, 0, kQosListenSetData) == QosUpdateResult::kPayloadTooLarge);
  CHECK(state.UpdateQos({}, 0, 0x80000000) == QosUpdateResult::kInvalidFlags);
}

TEST_CASE("LAN state cleanup clears mappings QoS and counters", "[kernel][lan]") {
  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.136", 0xB13E000000000001));
  auto address = state.local_xnaddr();
  uint32_t converted = 0;
  REQUIRE(state.XnAddrToInAddr(address, &converted));
  REQUIRE(state.UpdateQos({}, 1024, kQosListenEnable | kQosListenSetBitsPerSecond) ==
          QosUpdateResult::kSuccess);
  state.RecordSend(120);
  state.RecordReceive(80);

  auto stats = state.stats_snapshot();
  CHECK(stats.sent_packets == 1);
  CHECK(stats.sent_bytes == 120);
  CHECK(stats.received_packets == 1);
  CHECK(stats.received_bytes == 80);

  state.Reset();
  CHECK_FALSE(state.enabled());
  CHECK(state.qos_snapshot().payload.empty());
  CHECK(state.stats_snapshot() == NetworkStatsSnapshot{});
  XnAddrSnapshot restored{};
  CHECK_FALSE(state.InAddrToXnAddr(converted, &restored));
}

TEST_CASE("Native UDP port collision fails without address reuse", "[kernel][lan]") {
#if REX_PLATFORM_WIN32
  WSADATA data{};
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif

  {
    rex::system::XSocket first(nullptr);
    rex::system::XSocket second(nullptr);
    REQUIRE(first.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                             rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
    REQUIRE(second.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                              rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);

    rex::system::N_XSOCKADDR_IN first_address{};
    first_address.sin_family = AF_INET;
    first_address.sin_addr = INADDR_LOOPBACK;
    first_address.sin_port = 0;
    REQUIRE(first.Bind(&first_address, sizeof(first_address)) == X_STATUS_SUCCESS);

    sockaddr_in endpoint{};

#if REX_PLATFORM_WIN32
    int endpoint_size = sizeof(endpoint);
#else
    socklen_t endpoint_size = sizeof(endpoint);
#endif
    REQUIRE(getsockname(first.native_handle(), reinterpret_cast<sockaddr*>(&endpoint),
                        &endpoint_size) == 0);

    rex::system::N_XSOCKADDR_IN second_address{};
    second_address.sin_family = AF_INET;
    second_address.sin_addr = ntohl(endpoint.sin_addr.s_addr);
    second_address.sin_port = ntohs(endpoint.sin_port);
    CHECK(second.Bind(&second_address, sizeof(second_address)) == X_STATUS_UNSUCCESSFUL);
  }

#if REX_PLATFORM_WIN32
  WSACleanup();
#endif
}

TEST_CASE("XSocket reports its selected native endpoint", "[kernel][lan]") {
#if REX_PLATFORM_WIN32
  WSADATA data{};
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif

  {
    rex::system::XSocket socket(nullptr);
    REQUIRE(socket.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                              rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);

    rex::system::N_XSOCKADDR_IN bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr = INADDR_LOOPBACK;
    bind_address.sin_port = 0;
    REQUIRE(socket.Bind(&bind_address, sizeof(bind_address)) == X_STATUS_SUCCESS);

    rex::system::N_XSOCKADDR_IN endpoint{};
    uint32_t endpoint_size = sizeof(endpoint);
    REQUIRE(socket.GetSockName(&endpoint, &endpoint_size) == X_STATUS_SUCCESS);
    CHECK(endpoint_size == sizeof(endpoint));
    CHECK(static_cast<uint16_t>(endpoint.sin_family) == AF_INET);
    CHECK(static_cast<uint32_t>(endpoint.sin_addr) == INADDR_LOOPBACK);
    CHECK(static_cast<uint16_t>(endpoint.sin_port) != 0);
  }

#if REX_PLATFORM_WIN32
  WSACleanup();
#endif
}

TEST_CASE("XSocket datagrams preserve native address and port order", "[kernel][lan][.network]") {
#if REX_PLATFORM_WIN32
  WSADATA data{};
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif

  {
    rex::system::XSocket receiver(nullptr);
    rex::system::XSocket sender(nullptr);
    REQUIRE(receiver.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                                rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
    REQUIRE(sender.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                              rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);

    rex::system::N_XSOCKADDR_IN bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr = INADDR_LOOPBACK;
    bind_address.sin_port = 0;
    REQUIRE(receiver.Bind(&bind_address, sizeof(bind_address)) == X_STATUS_SUCCESS);

    sockaddr_in receiver_address{};
#if REX_PLATFORM_WIN32
    int receiver_address_size = sizeof(receiver_address);
#else
    socklen_t receiver_address_size = sizeof(receiver_address);
#endif
    REQUIRE(getsockname(receiver.native_handle(), reinterpret_cast<sockaddr*>(&receiver_address),
                        &receiver_address_size) == 0);

    rex::system::N_XSOCKADDR_IN destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr = ntohl(receiver_address.sin_addr.s_addr);
    destination.sin_port = ntohs(receiver_address.sin_port);
    uint8_t sent = 0x5A;
    REQUIRE(sender.SendTo(&sent, sizeof(sent), 0, &destination, sizeof(destination)) ==
            sizeof(sent));

    sockaddr_in sender_address{};
#if REX_PLATFORM_WIN32
    int sender_address_size = sizeof(sender_address);
#else
    socklen_t sender_address_size = sizeof(sender_address);
#endif
    REQUIRE(getsockname(sender.native_handle(), reinterpret_cast<sockaddr*>(&sender_address),
                        &sender_address_size) == 0);

    uint8_t received = 0;
    rex::system::N_XSOCKADDR_IN source{};
    uint32_t source_size = sizeof(source);
    REQUIRE(receiver.RecvFrom(&received, sizeof(received), 0, &source, &source_size) ==
            sizeof(received));
    CHECK(received == sent);
    CHECK(static_cast<uint32_t>(source.sin_addr) == INADDR_LOOPBACK);
    CHECK(static_cast<uint16_t>(source.sin_port) == ntohs(sender_address.sin_port));
  }

#if REX_PLATFORM_WIN32
  WSACleanup();
#endif
}

class ScopedSocketStartup {
 public:
  ScopedSocketStartup() {
#if REX_PLATFORM_WIN32
    WSADATA data{};
    started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    started_ = true;
#endif
  }
  ~ScopedSocketStartup() {
#if REX_PLATFORM_WIN32
    if (started_) {
      WSACleanup();
    }
#endif
  }
  bool started() const { return started_; }

 private:
  bool started_ = false;
};

rex::system::N_XSOCKADDR_IN MakeAddress(uint32_t host_order_address, uint16_t port) {
  rex::system::N_XSOCKADDR_IN address{};
  address.sin_family = AF_INET;
  address.sin_addr = host_order_address;
  address.sin_port = port;
  return address;
}

rex::system::N_XSOCKADDR_IN LocalEndpoint(rex::system::XSocket& socket) {
  rex::system::N_XSOCKADDR_IN endpoint{};
  uint32_t endpoint_size = sizeof(endpoint);
  REQUIRE(socket.GetSockName(&endpoint, &endpoint_size) == X_STATUS_SUCCESS);
  return endpoint;
}

#if REX_PLATFORM_WIN32
bool WaitReadable(rex::system::XSocket& socket) {
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(static_cast<SOCKET>(socket.native_handle()), &readable);
  timeval timeout{2, 0};
  return select(0, &readable, nullptr, nullptr, &timeout) == 1;
}

uint16_t FindFreeUdpPort() {
  SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  REQUIRE(probe != INVALID_SOCKET);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  REQUIRE(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  int address_size = sizeof(address);
  REQUIRE(getsockname(probe, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
  closesocket(probe);
  return ntohs(address.sin_port);
}

// Selects the default-route IPv4 address without sending; zero when unavailable.
uint32_t FindRoutedIpv4() {
  SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (probe == INVALID_SOCKET) {
    return 0;
  }
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(9);
  inet_pton(AF_INET, "192.0.2.1", &destination.sin_addr);
  sockaddr_in local{};
  int local_size = sizeof(local);
  const bool routed =
      connect(probe, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0 &&
      getsockname(probe, reinterpret_cast<sockaddr*>(&local), &local_size) == 0;
  closesocket(probe);
  const uint32_t address = routed ? local.sin_addr.s_addr : 0;
  return ntohl(address) >> 24 == 127 ? 0 : address;
}
#endif

TEST_CASE("LAN socket kinds are recorded only while enabled", "[kernel][lan]") {
  LanSystemLinkState state;
  state.RegisterSocket(7, SocketKind::kDatagram);
  CHECK(state.socket_kind(7) == SocketKind::kUnknown);

  REQUIRE(state.Configure("10.0.0.136", 0xB13E000000000001));
  state.RegisterSocket(7, SocketKind::kDatagram);
  state.RegisterSocket(8, SocketKind::kVdp);
  CHECK(state.socket_kind(7) == SocketKind::kDatagram);
  CHECK(state.socket_kind(8) == SocketKind::kVdp);

  CHECK(state.NeedsBindBeforeSend(7));
  CHECK(state.NeedsBindBeforeSend(99));
  state.MarkBound(7);
  CHECK_FALSE(state.NeedsBindBeforeSend(7));
  state.RegisterSocket(9, SocketKind::kStream);
  CHECK_FALSE(state.NeedsBindBeforeSend(9));

  state.ForgetSocket(7);
  CHECK(state.socket_kind(7) == SocketKind::kUnknown);
  state.Reset();
  CHECK(state.socket_kind(8) == SocketKind::kUnknown);
  CHECK_FALSE(state.NeedsBindBeforeSend(8));
}

TEST_CASE("LAN retained UDP sockets keep baseline cleanup and startup behavior",
          "[kernel][lan][.network]") {
  ScopedSocketStartup startup;
  REQUIRE(startup.started());
  LanSystemLinkState state;
  rex::system::XSocket socket(nullptr);
  REQUIRE(socket.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                            rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
  state.RegisterSocket(7, SocketKind::kDatagram);
  REQUIRE(state.Configure("127.0.0.1", 1));
  CHECK(state.socket_kind(7) == SocketKind::kUnknown);
  state.RegisterSocket(7, SocketKind::kDatagram);
  uint16_t original_port = 0;
  SECTION("unbound socket") {}
  SECTION("bound socket") {
    REQUIRE(BindUnboundToLocalAddress(socket, state.local_ipv4()));
    original_port = LocalEndpoint(socket).sin_port;
    state.MarkBound(7);
    CHECK_FALSE(state.NeedsBindBeforeSend(7));
  }
#if REX_PLATFORM_WIN32
  const auto claimed_port = FindFreeUdpPort();
  bool claimed = false;
  REQUIRE(state.ClaimDatagramPort(7, claimed_port, &claimed) == 0);
  REQUIRE(claimed);
#endif
  // XNetCleanup calls Reset; XNetStartup calls Configure. Neither closes guest sockets.
  state.Reset();
  CHECK(state.socket_kind(7) == SocketKind::kUnknown);
  CHECK_FALSE(state.HasPortClaim(7));
  CHECK_FALSE(state.NeedsBindBeforeSend(7));
  REQUIRE(state.Configure("127.0.0.1", 1));
  CHECK(state.socket_kind(7) == SocketKind::kUnknown);
  CHECK(state.NeedsBindBeforeSend(7));
#if REX_PLATFORM_WIN32
  REQUIRE(state.ClaimDatagramPort(7, claimed_port, &claimed) == 0);
  CHECK_FALSE(claimed);
  state.RegisterSocket(8, SocketKind::kDatagram);
  REQUIRE(state.ClaimDatagramPort(8, claimed_port, &claimed) == 0);
  REQUIRE(claimed);
  REQUIRE(state.Configure("127.0.0.1", 1));
  CHECK(state.socket_kind(8) == SocketKind::kDatagram);
  CHECK(state.HasPortClaim(8));
#endif
  CHECK(BindUnboundToLocalAddress(socket, state.local_ipv4()) == (original_port == 0));
  state.MarkBound(7);
  CHECK(state.NeedsBindBeforeSend(7));
  auto endpoint = LocalEndpoint(socket);
  CHECK(static_cast<uint32_t>(endpoint.sin_addr) == INADDR_LOOPBACK);
  if (original_port)
    CHECK(static_cast<uint16_t>(endpoint.sin_port) == original_port);
  uint8_t sent = 0x5a;
  REQUIRE(socket.SendTo(&sent, 1, 0, &endpoint, sizeof(endpoint)) == 1);
#if REX_PLATFORM_WIN32
  REQUIRE(WaitReadable(socket));
#endif
  uint8_t received = 0;
  rex::system::N_XSOCKADDR_IN source{};
  uint32_t source_size = sizeof(source);
  REQUIRE(socket.RecvFrom(&received, 1, 0, &source, &source_size) == 1);
  CHECK(received == sent);
}

TEST_CASE("LAN implicit bind selects the configured address once", "[kernel][lan]") {
  ScopedSocketStartup startup;
  REQUIRE(startup.started());
  {
    rex::system::XSocket unbound(nullptr);
    REQUIRE(unbound.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                               rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
    const uint32_t loopback = htonl(INADDR_LOOPBACK);
    REQUIRE(BindUnboundToLocalAddress(unbound, loopback));
    auto endpoint = LocalEndpoint(unbound);
    CHECK(static_cast<uint32_t>(endpoint.sin_addr) == INADDR_LOOPBACK);
    CHECK(static_cast<uint16_t>(endpoint.sin_port) != 0);
    CHECK_FALSE(BindUnboundToLocalAddress(unbound, loopback));

    rex::system::XSocket bound(nullptr);
    REQUIRE(bound.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                             rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
    auto explicit_address = MakeAddress(INADDR_LOOPBACK, 0);
    REQUIRE(bound.Bind(&explicit_address, sizeof(explicit_address)) == X_STATUS_SUCCESS);
    const auto explicit_endpoint = LocalEndpoint(bound);
    CHECK_FALSE(BindUnboundToLocalAddress(bound, loopback));
    CHECK(static_cast<uint16_t>(LocalEndpoint(bound).sin_port) ==
          static_cast<uint16_t>(explicit_endpoint.sin_port));
  }
}

TEST_CASE("VDP datagrams keep the guest length prefix", "[kernel][lan][.network]") {
  ScopedSocketStartup startup;
  REQUIRE(startup.started());
  {
    rex::system::XSocket receiver(nullptr);
    rex::system::XSocket sender(nullptr);
    REQUIRE(receiver.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                                rex::system::XSocket::X_IPPROTO_VDP) == X_STATUS_SUCCESS);
    REQUIRE(sender.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                              rex::system::XSocket::X_IPPROTO_VDP) == X_STATUS_SUCCESS);
    auto bind_address = MakeAddress(INADDR_LOOPBACK, 0);
    REQUIRE(receiver.Bind(&bind_address, sizeof(bind_address)) == X_STATUS_SUCCESS);
    auto destination = LocalEndpoint(receiver);

    std::array<uint8_t, 5> sent{0x00, 0x03, 0xFE, 0xFE, 0x68};
    REQUIRE(sender.SendTo(sent.data(), sent.size(), 0, &destination, sizeof(destination)) ==
            static_cast<int>(sent.size()));
    std::array<uint8_t, 16> received{};
    rex::system::N_XSOCKADDR_IN source{};
    uint32_t source_size = sizeof(source);
    REQUIRE(receiver.RecvFrom(received.data(), received.size(), 0, &source, &source_size) ==
            static_cast<int>(sent.size()));
    CHECK(std::equal(sent.begin(), sent.end(), received.begin()));
  }
}

#if REX_PLATFORM_WIN32
TEST_CASE("LAN datagram port claims admit one instance per port", "[kernel][lan][.network]") {
  ScopedSocketStartup startup;
  REQUIRE(startup.started());
  const uint16_t port = FindFreeUdpPort();

  LanSystemLinkState first;
  LanSystemLinkState second;
  REQUIRE(first.Configure("127.0.0.1", 0xB13E000000000001));
  REQUIRE(second.Configure("127.0.0.1", 0xB13E000000000002));
  CHECK(first.ClaimDatagramPort(1, port, nullptr) == 0);
  CHECK_FALSE(first.HasPortClaim(1));

  first.RegisterSocket(1, SocketKind::kDatagram);
  second.RegisterSocket(1, SocketKind::kDatagram);
  bool claimed = false;
  REQUIRE(first.ClaimDatagramPort(1, port, &claimed) == 0);
  CHECK(claimed);
  CHECK(first.HasPortClaim(1));
  CHECK(first.ClaimDatagramPort(1, FindFreeUdpPort(), &claimed) == 0);
  CHECK_FALSE(claimed);
  REQUIRE(first.Configure("127.0.0.1", 0xB13E000000000003));
  CHECK(first.HasPortClaim(1));
  CHECK(first.socket_kind(1) == SocketKind::kDatagram);
  CHECK(second.ClaimDatagramPort(1, port, nullptr) == WSAEADDRINUSE);
  CHECK_FALSE(second.HasPortClaim(1));

  {
    rex::system::XSocket selected(nullptr);
    REQUIRE(selected.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                                rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
    auto selected_address = MakeAddress(INADDR_LOOPBACK, port);
    CHECK(selected.Bind(&selected_address, sizeof(selected_address)) == X_STATUS_SUCCESS);
  }

  first.ForgetSocket(1);
  REQUIRE(second.ClaimDatagramPort(1, port, nullptr) == 0);
  second.ReleasePortClaim(1);
  CHECK_FALSE(second.HasPortClaim(1));
  REQUIRE(second.ClaimDatagramPort(1, port, nullptr) == 0);
  second.Reset();
  first.RegisterSocket(2, SocketKind::kDatagram);
  CHECK(first.ClaimDatagramPort(2, port, nullptr) == 0);
}

TEST_CASE("LAN guest broadcast option enables and disables limited sends",
          "[kernel][lan][.network]") {
  ScopedSocketStartup startup;
  REQUIRE(startup.started());
  const uint32_t selected = FindRoutedIpv4();
  if (!selected) {
    SKIP("no routed non-loopback IPv4 address");
  }

  constexpr uint32_t kGuestSolSocket = 0xFFFF;
  constexpr uint32_t kGuestSoBroadcast = 0x0020;
  rex::system::XSocket socket(nullptr);
  REQUIRE(socket.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                            rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
  auto bind_address = MakeAddress(ntohl(selected), 0);
  REQUIRE(socket.Bind(&bind_address, sizeof(bind_address)) == X_STATUS_SUCCESS);
  auto destination = MakeAddress(INADDR_BROADCAST, LocalEndpoint(socket).sin_port);
  uint8_t payload = 0x02;

  rex::be<uint32_t> enable = 0;
  REQUIRE(socket.SetOption(kGuestSolSocket, kGuestSoBroadcast, &enable, sizeof(enable)) ==
          X_STATUS_SUCCESS);
  int sent = socket.SendTo(&payload, 1, 0, &destination, sizeof(destination));
  int error = WSAGetLastError();
  CHECK(sent < 0);
  CHECK(error == WSAEACCES);

  enable = 1;
  REQUIRE(socket.SetOption(kGuestSolSocket, kGuestSoBroadcast, &enable, sizeof(enable)) ==
          X_STATUS_SUCCESS);
  REQUIRE(socket.SendTo(&payload, 1, 0, &destination, sizeof(destination)) == 1);

  enable = 0;
  REQUIRE(socket.SetOption(kGuestSolSocket, kGuestSoBroadcast, &enable, sizeof(enable)) ==
          X_STATUS_SUCCESS);
  sent = socket.SendTo(&payload, 1, 0, &destination, sizeof(destination));
  error = WSAGetLastError();
  CHECK(sent < 0);
  CHECK(error == WSAEACCES);
}

TEST_CASE("LAN limited broadcasts reach selected-address sockets", "[kernel][lan][.network]") {
  ScopedSocketStartup startup;
  REQUIRE(startup.started());
  const uint32_t selected = FindRoutedIpv4();
  if (!selected) {
    SKIP("no routed non-loopback IPv4 address");
  }

  {
    constexpr uint32_t kGuestSolSocket = 0xFFFF;
    constexpr uint32_t kGuestSoBroadcast = 0x0020;
    rex::be<uint32_t> enable = 1;
    rex::system::XSocket host(nullptr);
    rex::system::XSocket client(nullptr);
    REQUIRE(host.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                            rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
    REQUIRE(client.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                              rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
    auto host_address = MakeAddress(ntohl(selected), 0);
    REQUIRE(host.Bind(&host_address, sizeof(host_address)) == X_STATUS_SUCCESS);
    REQUIRE(host.SetOption(kGuestSolSocket, kGuestSoBroadcast, &enable, sizeof(enable)) ==
            X_STATUS_SUCCESS);
    REQUIRE(client.SetOption(kGuestSolSocket, kGuestSoBroadcast, &enable, sizeof(enable)) ==
            X_STATUS_SUCCESS);
    REQUIRE(BindUnboundToLocalAddress(client, selected));
    const auto host_endpoint = LocalEndpoint(host);
    const auto client_endpoint = LocalEndpoint(client);
    CHECK(static_cast<uint32_t>(client_endpoint.sin_addr) == ntohl(selected));

    auto to_host = MakeAddress(INADDR_BROADCAST, host_endpoint.sin_port);
    uint8_t request = 0x02;
    REQUIRE(client.SendTo(&request, 1, 0, &to_host, sizeof(to_host)) == 1);
    REQUIRE(WaitReadable(host));
    uint8_t received = 0;
    rex::system::N_XSOCKADDR_IN source{};
    uint32_t source_size = sizeof(source);
    REQUIRE(host.RecvFrom(&received, 1, 0, &source, &source_size) == 1);
    CHECK(received == request);
    CHECK(static_cast<uint32_t>(source.sin_addr) == ntohl(selected));
    CHECK(static_cast<uint16_t>(source.sin_port) ==
          static_cast<uint16_t>(client_endpoint.sin_port));

    auto to_client = MakeAddress(INADDR_BROADCAST, client_endpoint.sin_port);
    uint8_t reply = 0x05;
    REQUIRE(host.SendTo(&reply, 1, 0, &to_client, sizeof(to_client)) == 1);
    REQUIRE(WaitReadable(client));
    source_size = sizeof(source);
    REQUIRE(client.RecvFrom(&received, 1, 0, &source, &source_size) == 1);
    CHECK(received == reply);
    CHECK(static_cast<uint16_t>(source.sin_port) == static_cast<uint16_t>(host_endpoint.sin_port));
  }
}
#endif

uint32_t AllocGuestScratch(rex::memory::Memory& memory) {
  auto* heap = memory.LookupHeap(0x10000000);
  REQUIRE(heap != nullptr);
  uint32_t address = 0;
  REQUIRE(heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address));
  return address;
}

TEST_CASE("LAN host session create fills session info and nonce", "[kernel][lan]") {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t scratch = AllocGuestScratch(memory);
  auto* info = memory.TranslateVirtual<uint8_t*>(scratch);
  auto* nonce = memory.TranslateVirtual<uint8_t*>(scratch + 256);
  std::fill_n(info, kSessionInfoSize, uint8_t{0xA5});
  std::fill_n(nonce, kSessionNonceSize, uint8_t{0});

  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.136", 0xB13E000000000001));

  // The host flag may be combined with other create flags (observed 0x21).
  REQUIRE(FillHostSessionInfo(state, memory, 0x21, scratch, scratch + 256));

  CHECK((info[0] & 0xE0) == 0);
  CHECK_FALSE(std::all_of(info, info + 8, [](uint8_t b) { return b == 0; }));

  // The host address must equal what XNetGetTitleXnAddr stores for this state:
  // ina and inaOnline raw, wPortOnline big-endian, then abEnet and abOnline.
  const XnAddrSnapshot local = state.local_xnaddr();
  std::array<uint8_t, 36> expected{};
  std::memcpy(expected.data() + 0, &local.ipv4, 4);
  std::memcpy(expected.data() + 4, &local.online_ipv4, 4);
  expected[8] = static_cast<uint8_t>(local.online_port >> 8);
  expected[9] = static_cast<uint8_t>(local.online_port);
  std::memcpy(expected.data() + 10, local.ethernet.data(), 6);
  std::memcpy(expected.data() + 16, local.online.data(), 20);
  CHECK(std::equal(expected.begin(), expected.end(), info + 8));

  CHECK_FALSE(std::all_of(info + 44, info + 60, [](uint8_t b) { return b == 0; }));
  CHECK_FALSE(std::all_of(nonce, nonce + kSessionNonceSize, [](uint8_t b) { return b == 0; }));

  // A second create produces a different session.
  std::array<uint8_t, kSessionInfoSize> first{};
  std::copy_n(info, first.size(), first.begin());
  REQUIRE(FillHostSessionInfo(state, memory, kSessionCreateHost, scratch, 0));
  CHECK_FALSE(std::equal(first.begin(), first.end(), info));

  // Repeat so a missing system-link mask fails deterministically.
  bool all_system_link = true;
  for (int i = 0; i < 64; ++i) {
    REQUIRE(FillHostSessionInfo(state, memory, kSessionCreateHost, scratch, 0));
    all_system_link = all_system_link && (info[0] & 0xE0) == 0;
  }
  CHECK(all_system_link);
}

TEST_CASE("LAN non-host session create leaves session info untouched", "[kernel][lan]") {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t scratch = AllocGuestScratch(memory);
  auto* info = memory.TranslateVirtual<uint8_t*>(scratch);
  auto* nonce = memory.TranslateVirtual<uint8_t*>(scratch + 256);
  std::fill_n(info, kSessionInfoSize, uint8_t{0x5C});
  std::fill_n(nonce, kSessionNonceSize, uint8_t{0x3D});

  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.28", 0xB13E000000000002));

  CHECK_FALSE(FillHostSessionInfo(state, memory, 0x20, scratch, scratch + 256));
  CHECK(std::all_of(info, info + kSessionInfoSize, [](uint8_t b) { return b == 0x5C; }));
  CHECK(std::all_of(nonce, nonce + kSessionNonceSize, [](uint8_t b) { return b == 0x3D; }));

  // A host create without a session info pointer writes nothing either.
  CHECK_FALSE(FillHostSessionInfo(state, memory, kSessionCreateHost, 0, scratch + 256));
  CHECK(std::all_of(nonce, nonce + kSessionNonceSize, [](uint8_t b) { return b == 0x3D; }));
}

TEST_CASE("Disabled LAN session create writes nothing", "[kernel][lan]") {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t scratch = AllocGuestScratch(memory);
  auto* info = memory.TranslateVirtual<uint8_t*>(scratch);
  auto* nonce = memory.TranslateVirtual<uint8_t*>(scratch + 256);
  std::fill_n(info, kSessionInfoSize, uint8_t{0x77});
  std::fill_n(nonce, kSessionNonceSize, uint8_t{0x11});

  LanSystemLinkState state;
  CHECK_FALSE(FillHostSessionInfo(state, memory, kSessionCreateHost, scratch, scratch + 256));
  CHECK(std::all_of(info, info + kSessionInfoSize, [](uint8_t b) { return b == 0x77; }));
  CHECK(std::all_of(nonce, nonce + kSessionNonceSize, [](uint8_t b) { return b == 0x11; }));
}

TEST_CASE("LAN session details retain host creation and local membership", "[kernel][lan]") {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t scratch = AllocGuestScratch(memory);
  auto* bytes = memory.TranslateVirtual<uint8_t*>(scratch);
  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.136", 0xB13E000000000001));

  CreateSession(state, memory, 0x21, 2, 0, 1, scratch, scratch + 64);
  REQUIRE(state.session_snapshot().created);
  CHECK((bytes[0] & 0xE0) == 0);
  CHECK_FALSE(std::all_of(bytes + 64, bytes + 72, [](uint8_t b) { return b == 0; }));
  std::array<uint8_t, kSessionInfoSize> host_info{};
  std::copy_n(bytes, host_info.size(), host_info.begin());
  std::array<uint8_t, kSessionNonceSize> host_nonce{};
  std::copy_n(bytes + 64, host_nonce.size(), host_nonce.begin());
  JoinSession(state, memory, 1, 0);

  // The record owns these bytes even after the create buffers are reused.
  std::fill_n(bytes, 72, uint8_t{0});
  memory::store_and_swap<uint32_t>(bytes + 128, 128);
  auto* details = bytes + 256;
  std::fill_n(details, 128, uint8_t{0xA5});
  REQUIRE(FillSessionDetails(state, memory, scratch + 128, scratch + 256) == 0);
  CHECK(memory::load_and_swap<uint32_t>(details + 0) == 1);
  CHECK(memory::load_and_swap<uint32_t>(details + 4) == 0);
  CHECK(memory::load_and_swap<uint32_t>(details + 8) == 0);
  CHECK(memory::load_and_swap<uint32_t>(details + 12) == 0x21);
  CHECK(memory::load_and_swap<uint32_t>(details + 16) == 2);
  CHECK(memory::load_and_swap<uint32_t>(details + 20) == 0);
  CHECK(memory::load_and_swap<uint32_t>(details + 24) == 1);
  CHECK(memory::load_and_swap<uint32_t>(details + 28) == 0);
  CHECK(memory::load_and_swap<uint32_t>(details + 32) == 1);
  CHECK(memory::load_and_swap<uint32_t>(details + 36) == 0);
  CHECK(memory::load_and_swap<uint32_t>(details + 40) == 0);
  CHECK(std::equal(host_nonce.begin(), host_nonce.end(), details + 44));
  CHECK(std::equal(host_info.begin(), host_info.end(), details + 52));
  CHECK(std::all_of(details + 112, details + 124, [](uint8_t b) { return b == 0; }));
  CHECK(std::all_of(details + 124, details + 128, [](uint8_t b) { return b == 0xA5; }));
}

TEST_CASE("LAN session details retain client input and private membership", "[kernel][lan]") {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t scratch = AllocGuestScratch(memory);
  auto* bytes = memory.TranslateVirtual<uint8_t*>(scratch);
  std::fill_n(bytes, kSessionInfoSize, uint8_t{0x5C});
  std::fill_n(bytes + 64, kSessionNonceSize, uint8_t{0x3D});
  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.28", 0xB13E000000000002));
  CreateSession(state, memory, 0x20, 1, 1, 0, scratch, scratch + 64);
  CHECK(std::all_of(bytes, bytes + kSessionInfoSize, [](uint8_t b) { return b == 0x5C; }));
  CHECK(std::all_of(bytes + 64, bytes + 72, [](uint8_t b) { return b == 0x3D; }));
  memory::store_and_swap<uint32_t>(bytes + 80, 0);
  memory::store_and_swap<uint32_t>(bytes + 84, 1);
  memory::store_and_swap<uint32_t>(bytes + 88, 2);
  JoinSession(state, memory, 3, scratch + 80);
  memory::store_and_swap<uint32_t>(bytes + 128, kSessionDetailsSize);
  auto* details = bytes + 256;
  for (uint32_t session_state : {2u, 3u}) {
    state.SetSessionState(session_state);
    REQUIRE(FillSessionDetails(state, memory, scratch + 128, scratch + 256) == 0);
    CHECK(memory::load_and_swap<uint32_t>(details + 24) == 0);
    CHECK(memory::load_and_swap<uint32_t>(details + 28) == 0);
    CHECK(memory::load_and_swap<uint32_t>(details + 32) == 3);
    CHECK(memory::load_and_swap<uint32_t>(details + 40) == session_state);
    CHECK(std::all_of(details + 44, details + 52, [](uint8_t b) { return b == 0x3D; }));
    CHECK(std::all_of(details + 52, details + 112, [](uint8_t b) { return b == 0x5C; }));
  }
  CreateSession(state, memory, 0x20, 2, 0, 0, scratch, scratch + 64);
  CHECK(state.session_snapshot().joined_public == 0);
  CHECK(state.session_snapshot().joined_private == 0);
  CHECK(state.session_snapshot().state == 0);
}

TEST_CASE("LAN session details report the required buffer size", "[kernel][lan]") {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t scratch = AllocGuestScratch(memory);
  auto* bytes = memory.TranslateVirtual<uint8_t*>(scratch);
  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.136", 0xB13E000000000001));
  CreateSession(state, memory, 0x21, 2, 0, 0, scratch, 0);
  memory::store_and_swap<uint32_t>(bytes + 128, 123);
  std::fill_n(bytes + 256, 128, uint8_t{0xA5});
  CHECK(FillSessionDetails(state, memory, scratch + 128, scratch + 256) == 122);
  CHECK(memory::load_and_swap<uint32_t>(bytes + 128) == 124);
  CHECK(std::all_of(bytes + 256, bytes + 384, [](uint8_t b) { return b == 0xA5; }));
  memory::store_and_swap<uint32_t>(bytes + 128, 0);
  CHECK(FillSessionDetails(state, memory, scratch + 128, 0) == 122);
  CHECK(memory::load_and_swap<uint32_t>(bytes + 128) == 124);
  CHECK(FillSessionDetails(state, memory, 0, scratch + 256) == 87);
  CHECK(FillSessionDetails(state, memory, scratch + 128, 0) == 87);
  CHECK(FillSessionDetails(state, memory, scratch + 128, 0xFFFFFFF0) == 87);
}

TEST_CASE("LAN session delete and cleanup clear retained details", "[kernel][lan]") {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t scratch = AllocGuestScratch(memory);
  auto* bytes = memory.TranslateVirtual<uint8_t*>(scratch);
  LanSystemLinkState state;
  REQUIRE(state.Configure("10.0.0.136", 0xB13E000000000001));
  CreateSession(state, memory, 0x21, 2, 0, 0, scratch, 0);
  JoinSession(state, memory, 1, 0);
  SECTION("delete") {
    state.DeleteSession();
  }
  SECTION("XNetCleanup reset") {
    state.Reset();
    REQUIRE(state.Configure("10.0.0.136", 0xB13E000000000001));
  }
  CHECK_FALSE(state.session_snapshot().created);
  CHECK(state.session_snapshot().joined_public == 0);
  std::fill_n(bytes + 128, 256, uint8_t{0xA5});
  CHECK(FillSessionDetails(state, memory, scratch + 128, scratch + 256) == 0);
  CHECK(std::all_of(bytes + 128, bytes + 384, [](uint8_t b) { return b == 0xA5; }));
}

TEST_CASE("Disabled LAN session details write nothing", "[kernel][lan]") {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t scratch = AllocGuestScratch(memory);
  auto* bytes = memory.TranslateVirtual<uint8_t*>(scratch);
  std::fill_n(bytes, 384, uint8_t{0xA5});
  LanSystemLinkState state;
  CreateSession(state, memory, 0x21, 2, 0, 0, scratch, scratch + 64);
  JoinSession(state, memory, 1, 0);
  CHECK_FALSE(state.session_snapshot().created);
  CHECK(FillSessionDetails(state, memory, scratch + 128, scratch + 256) == 0);
  CHECK(std::all_of(bytes, bytes + 384, [](uint8_t b) { return b == 0xA5; }));
}

}  // namespace
}  // namespace rex::kernel::xam::lan
