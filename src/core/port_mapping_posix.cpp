#include "port_mapping_internal.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace rex::port_mapping::internal {
namespace {
// Linux lists routes in /proc/net/route; a default route has destination 0,
// mask 0 and the gateway flag, and the lowest metric wins. Other systems have
// no such file, so no router is found.
std::optional<Route> FindRoute() {
  std::ifstream routes("/proc/net/route");
  std::string line;
  std::getline(routes, line);
  std::optional<Route> best;
  uint32_t best_metric = 0;
  while (std::getline(routes, line)) {
    std::istringstream fields(line);
    std::string interface;
    uint32_t destination = 0, gateway = 0, flags = 0, references = 0, use = 0, metric = 0, mask = 1;
    // The addresses are hexadecimal in the byte order of the machine, which
    // is the network order of the bytes in memory.
    if (!(fields >> interface >> std::hex >> destination >> gateway >> flags >> std::dec >>
          references >> use >> metric >> std::hex >> mask))
      continue;
    // Mask 0 leaves out the halves of the address space a VPN routes.
    if (destination != 0 || mask != 0 || gateway == 0 || !(flags & 0x2))
      continue;
    if (!best || metric < best_metric) {
      best = Route{gateway, 0};
      best_metric = metric;
    }
  }
  return best;
}
}  // namespace

Environment SystemEnvironment() {
  Environment environment;
  environment.find_route = FindRoute;
  auto direct = std::make_shared<DirectUpnp>();
  environment.upnp_open = [direct](const Route& route, const Options& options,
                                   const std::atomic<bool>& stop) {
    return direct->Open(route, options, stop);
  };
  environment.upnp_close = [direct](uint16_t outside_port) { direct->Close(outside_port); };
  return environment;
}

}  // namespace rex::port_mapping::internal
