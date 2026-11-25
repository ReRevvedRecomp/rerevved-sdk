#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/port_mapping.h>

#include "core/port_mapping_internal.h"
#include "core/relay_transport.h"

using namespace std::chrono_literals;
namespace port_mapping = rex::port_mapping;
namespace internal = rex::port_mapping::internal;
namespace relay = rex::relay;

namespace {
uint32_t Address(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const std::array<uint8_t, 4> octets{a, b, c, d};
  uint32_t address = 0;
  std::memcpy(&address, octets.data(), 4);
  return address;
}

struct FakeGateway {
  struct Behavior {
    std::array<uint8_t, 4> address{203, 0, 113, 7};
    uint16_t result = 0;        // NAT-PMP result code; 0 is success
    uint16_t granted_port = 0;  // 0 grants the port asked for
    bool silent = false;
  };
  explicit FakeGateway(Behavior behavior) : behavior_(behavior) {
    auto opened = relay::UdpSocket::Open("127.0.0.1", 0);
    if (!opened)
      return;
    socket_ = std::move(*opened);
    worker_ = std::thread([this] { Serve(); });
  }
  FakeGateway() : FakeGateway(Behavior{}) {}
  ~FakeGateway() {
    stop_.store(true);
    if (worker_.joinable())
      worker_.join();
  }
  bool ready() const { return socket_ != nullptr; }
  relay::Endpoint endpoint() const { return socket_->endpoint(); }
  internal::Route route() const { return {endpoint().ipv4, endpoint().ipv4}; }
  // Requests to map (lifetime above 0) or remove (lifetime 0) a port.
  size_t Count(bool removals) {
    std::lock_guard lock(mutex_);
    size_t count = 0;
    for (const auto& request : requests_) {
      if (request.size() != 12 || request[1] != 1)
        continue;
      const bool removal = !request[8] && !request[9] && !request[10] && !request[11];
      count += removal == removals;
    }
    return count;
  }

 private:
  void Serve() {
    while (!stop_.load()) {
      auto received = socket_->Receive(20);
      if (!received || !*received)
        continue;
      const auto& request = (*received)->bytes;
      {
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
      }
      if (behavior_.silent || request.size() < 2)
        continue;
      relay::Bytes reply{0,
                         static_cast<uint8_t>(request[1] + 128),
                         static_cast<uint8_t>(behavior_.result >> 8),
                         static_cast<uint8_t>(behavior_.result),
                         0,
                         0,
                         0,
                         1};
      if (request[1] == 0) {
        reply.insert(reply.end(), behavior_.address.begin(), behavior_.address.end());
      } else if (request.size() == 12) {
        const bool removal = !request[8] && !request[9] && !request[10] && !request[11];
        const uint16_t asked = static_cast<uint16_t>((request[6] << 8) | request[7]);
        const uint16_t outside =
            removal ? 0 : (behavior_.granted_port ? behavior_.granted_port : asked);
        reply.insert(reply.end(), {request[4], request[5], static_cast<uint8_t>(outside >> 8),
                                   static_cast<uint8_t>(outside), request[8], request[9],
                                   request[10], request[11]});
      }
      (void)socket_->Send({(*received)->endpoint, reply});
    }
  }

  Behavior behavior_;
  std::unique_ptr<relay::UdpSocket> socket_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::vector<relay::Bytes> requests_;
  std::thread worker_;
};

// The system as the mapping thread sees it: one router, NAT-PMP only.
internal::Environment EnvironmentFor(const FakeGateway& gateway) {
  internal::Environment environment;
  environment.find_route = [route = gateway.route()] { return std::optional(route); };
  environment.nat_pmp_port = gateway.endpoint().port;
  return environment;
}

port_mapping::Status WaitWhileOpening() {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (port_mapping::Current().state == port_mapping::State::kOpening &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  return port_mapping::Current();
}

port_mapping::Status FailWith(internal::Environment environment) {
  REQUIRE(internal::OpenWith(std::move(environment), {}));
  const auto status = WaitWhileOpening();
  port_mapping::Close();
  CHECK(status.state == port_mapping::State::kFailed);
  CHECK(status.address.empty());
  CHECK(status.port == 0);
  return status;
}
}  // namespace

TEST_CASE("port mapping tells public addresses from unreachable ones", "[port_mapping]") {
  for (const uint32_t address :
       {Address(10, 1, 2, 3), Address(172, 16, 0, 1), Address(172, 31, 255, 254),
        Address(192, 168, 1, 1), Address(100, 64, 0, 1), Address(100, 127, 255, 254),
        Address(127, 0, 0, 1), Address(169, 254, 3, 4), Address(0, 0, 0, 0)})
    CHECK(internal::IsUnreachable(address));
  for (const uint32_t address :
       {Address(203, 0, 113, 7), Address(172, 15, 0, 1), Address(172, 32, 0, 1),
        Address(100, 63, 0, 1), Address(100, 128, 0, 1), Address(192, 169, 0, 1),
        Address(8, 8, 8, 8)})
    CHECK_FALSE(internal::IsUnreachable(address));
}

TEST_CASE("NAT-PMP opens the port the router grants", "[port_mapping][network]") {
  std::atomic<bool> stop{false};
  {
    FakeGateway gateway({.granted_port = 41234});
    if (!gateway.ready())
      SKIP("Loopback UDP socket unavailable");
    const auto grant = internal::NatPmpOpen(gateway.endpoint(), 30100, 30100, 3600, stop);
    REQUIRE(grant);
    CHECK(grant->address == Address(203, 0, 113, 7));
    CHECK(grant->port == 41234);
    CHECK(gateway.Count(false) == 1);
    internal::NatPmpClose(gateway.endpoint(), 30100);
    CHECK(gateway.Count(true) == 1);
  }
  {
    // Result code 2: not authorized.
    FakeGateway refusing({.result = 2});
    REQUIRE(refusing.ready());
    CHECK_FALSE(internal::NatPmpOpen(refusing.endpoint(), 30100, 30100, 3600, stop));
  }
  {
    // A router without NAT-PMP does not answer.
    FakeGateway silent({.silent = true});
    REQUIRE(silent.ready());
    CHECK_FALSE(internal::NatPmpOpen(silent.endpoint(), 30100, 30100, 3600, stop));
  }
}

TEST_CASE("a router port opens, renews and is removed on close", "[port_mapping][network]") {
  CHECK(port_mapping::Current().state == port_mapping::State::kClosed);
  port_mapping::Close();  // Closing with nothing open does nothing.

  FakeGateway gateway({.granted_port = 41234});
  if (!gateway.ready())
    SKIP("Loopback UDP socket unavailable");
  auto environment = EnvironmentFor(gateway);
  environment.nat_pmp_lifetime = 2;  // renewed after one second
  CHECK_FALSE(internal::OpenWith(environment, {.port = 0}));
  REQUIRE(internal::OpenWith(environment, {.port = 30100}));
  // One mapping per process.
  CHECK_FALSE(internal::OpenWith(environment, {.port = 30101}));

  const auto status = WaitWhileOpening();
  CHECK(status.state == port_mapping::State::kOpen);
  CHECK(status.failure == port_mapping::Failure::kNone);
  CHECK(status.address == "203.0.113.7");
  CHECK(status.port == 41234);

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (gateway.Count(false) < 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  CHECK(gateway.Count(false) >= 2);
  CHECK(gateway.Count(true) == 0);

  port_mapping::Close();
  CHECK(gateway.Count(true) == 1);
  CHECK(port_mapping::Current().state == port_mapping::State::kClosed);
}

TEST_CASE("a router port reports why it could not be opened", "[port_mapping][network]") {
  using port_mapping::Failure;
  internal::Environment none;
  none.find_route = [] { return std::optional<internal::Route>(); };
  CHECK(FailWith(none).failure == Failure::kNoNetwork);

  // A refusal fails at once; a silent router would cost two seconds a case.
  FakeGateway refusing({.result = 2});
  if (!refusing.ready())
    SKIP("Loopback UDP socket unavailable");
  auto environment = EnvironmentFor(refusing);
  auto status = FailWith(environment);
  CHECK(status.failure == Failure::kNoRouter);
  CHECK_FALSE(status.public_network);

  environment.public_network = [] { return true; };
  status = FailWith(environment);
  CHECK(status.failure == Failure::kNoRouter);
  CHECK(status.public_network);

  environment.discovery_disabled = [] { return true; };
  status = FailWith(environment);
  CHECK(status.failure == Failure::kNoDiscovery);
  CHECK_FALSE(status.public_network);

  // UPnP names the cause when it found the router.
  environment = EnvironmentFor(refusing);
  environment.upnp_open = [](const internal::Route&, const port_mapping::Options&,
                             const std::atomic<bool>&) {
    return internal::UpnpResult{Failure::kPortsTaken, {}};
  };
  environment.upnp_close = [](uint16_t) {};
  CHECK(FailWith(environment).failure == Failure::kPortsTaken);

  // The router's own outside address is shared by the provider.
  FakeGateway shared({.address = {100, 64, 1, 2}});
  REQUIRE(shared.ready());
  CHECK(FailWith(EnvironmentFor(shared)).failure == Failure::kSharedAddress);
  CHECK(shared.Count(true) == 1);
}

TEST_CASE("a UPnP mapping wins over NAT-PMP and is removed on close", "[port_mapping][network]") {
  FakeGateway gateway;
  if (!gateway.ready())
    SKIP("Loopback UDP socket unavailable");
  std::atomic<int> closed_port{0};
  auto environment = EnvironmentFor(gateway);
  environment.upnp_open = [](const internal::Route&, const port_mapping::Options& options,
                             const std::atomic<bool>&) {
    return internal::UpnpResult{
        port_mapping::Failure::kNone,
        {Address(198, 51, 100, 9), static_cast<uint16_t>(options.port + 1)}};
  };
  environment.upnp_close = [&closed_port](uint16_t port) { closed_port.store(port); };
  REQUIRE(internal::OpenWith(environment, {.port = 30100}));
  const auto status = WaitWhileOpening();
  CHECK(status.state == port_mapping::State::kOpen);
  CHECK(status.address == "198.51.100.9");
  CHECK(status.port == 30101);
  CHECK(closed_port.load() == 0);
  port_mapping::Close();
  CHECK(closed_port.load() == 30101);
  CHECK(gateway.Count(false) == 0);

  // A UPnP mapping behind a shared address is removed and reported.
  environment.upnp_open = [](const internal::Route&, const port_mapping::Options& options,
                             const std::atomic<bool>&) {
    return internal::UpnpResult{port_mapping::Failure::kNone, {Address(10, 0, 0, 2), options.port}};
  };
  closed_port.store(0);
  CHECK(FailWith(environment).failure == port_mapping::Failure::kSharedAddress);
  CHECK(closed_port.load() == 30100);
  CHECK(gateway.Count(false) == 0);
}

TEST_CASE("the system lookups behind a router port answer without touching the router",
          "[port_mapping]") {
  const auto environment = internal::SystemEnvironment();
  REQUIRE(environment.find_route);
  const auto route = environment.find_route();
  const bool found = route.has_value();
  const bool has_local = route && route->local != 0;
  const bool gateway_private = route && internal::IsUnreachable(route->gateway);
  const bool discovery_disabled =
      environment.discovery_disabled && environment.discovery_disabled();
  const bool public_network = environment.public_network && environment.public_network();
  CAPTURE(found, has_local, gateway_private, discovery_disabled, public_network);
  SUCCEED();
}
