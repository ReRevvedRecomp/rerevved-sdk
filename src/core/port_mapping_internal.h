#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <rex/port_mapping.h>

#include "relay_core.h"

// Addresses are IPv4 in network byte order, as in relay::Endpoint.
namespace rex::port_mapping::internal {

// True for an address the Internet cannot reach: private, shared by the
// provider (100.64.0.0/10), loopback, link-local or "this network".
bool IsUnreachable(uint32_t address);

struct Route {
  uint32_t gateway = 0;
  uint32_t local = 0;  // this computer on the router's network; 0 when unknown
};

struct Grant {
  uint32_t address = 0;
  uint16_t port = 0;
};

// RFC 6886. `outside_port` and `lifetime` are only requests.
std::optional<Grant> NatPmpOpen(relay::Endpoint gateway, uint16_t port, uint16_t outside_port,
                                uint32_t lifetime, const std::atomic<bool>& stop);
void NatPmpClose(relay::Endpoint gateway, uint16_t port);

struct UpnpResult {
  Failure failure = Failure::kNoRouter;
  Grant grant;
};

// What the mapping thread asks of the system. Tests replace it.
struct Environment {
  std::function<std::optional<Route>()> find_route;
  // Null where UPnP is not available.
  std::function<UpnpResult(const Route&, const Options&, const std::atomic<bool>&)> upnp_open;
  std::function<void(uint16_t outside_port)> upnp_close;
  std::function<bool()> discovery_disabled;
  std::function<bool()> public_network;
  uint16_t nat_pmp_port = 5351;
  // Seconds; renewed at half.
  uint32_t nat_pmp_lifetime = 3600;
};

Environment SystemEnvironment();

VoidResult OpenWith(Environment environment, const Options& options);

}  // namespace rex::port_mapping::internal
