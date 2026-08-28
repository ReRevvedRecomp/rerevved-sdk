#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xsocket.h>

#include "kernel/xam/lan_system_link.h"

#if REX_PLATFORM_WIN32
#include <winsock2.h>
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

TEST_CASE("XSocket datagrams preserve native address and port order", "[kernel][lan]") {
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

}  // namespace
}  // namespace rex::kernel::xam::lan
