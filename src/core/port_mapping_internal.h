#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
  uint32_t lifetime = 0;  // seconds until the router drops it; 0 for no limit
};

enum class NatPmpOutcome {
  kGranted,
  kSilent,
  kRefused,
};
struct NatPmpResult {
  NatPmpOutcome outcome = NatPmpOutcome::kSilent;
  Grant grant;
  // The mapping request was sent, so without a grant the router may still
  // hold a mapping whose reply was lost.
  bool port_asked = false;
};

// RFC 6886. `outside_port` and `lifetime` are only requests.
NatPmpResult NatPmpOpen(relay::Endpoint gateway, uint16_t port, uint16_t outside_port,
                        uint32_t lifetime, const std::atomic<bool>& stop);
void NatPmpClose(relay::Endpoint gateway, uint16_t port);

struct UpnpResult {
  Failure failure = Failure::kNoRouter;
  Grant grant;
};

// Asks the router itself, so it works where the system's UPnP is off or
// blocked. Open and Close belong to one thread.
class DirectUpnp {
 public:
  struct Control;
  // Tests point `ssdp_port` at a fake router.
  explicit DirectUpnp(uint16_t ssdp_port = 1900);
  ~DirectUpnp();
  UpnpResult Open(const Route& route, const Options& options, const std::atomic<bool>& stop);
  void Close(uint16_t outside_port);

 private:
  uint16_t ssdp_port_;
  std::unique_ptr<Control> control_;
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
