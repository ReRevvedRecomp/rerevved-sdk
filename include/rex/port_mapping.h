/**
 * @file        rex/port_mapping.h
 * @brief       Opens a UDP port on the home router for the online server
 *
 * Safe from any host thread.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <string>

#include <rex/result.h>

namespace rex::port_mapping {

struct Options {
  uint16_t port = 30100;
  // The label on the router. An earlier mapping with it is reused.
  std::string name = "Online game";
};

enum class State {
  kClosed,
  kOpening,
  kOpen,
  kFailed,
};

enum class Failure {
  kNone,
  kNoNetwork,
  kNoDiscovery,  // the Windows service "SSDP Discovery" is disabled
  kNoRouter,
  kPortsTaken,     // other programs hold this port and the nine after it
  kSharedAddress,  // the router has no public address of its own
  kRefused,
};

struct Status {
  State state = State::kClosed;
  Failure failure = Failure::kNone;
  std::string address;
  uint16_t port = 0;  // may differ from the port asked for
  // Windows: a network set to Public hides the router from UPnP.
  bool public_network = false;
};

// Returns at once; poll Current for the result.
VoidResult Open(const Options& options);
// Waits for the router, which can take seconds. Call it before the process
// exits, or the mapping stays on the router.
void Close();
Status Current();

}  // namespace rex::port_mapping
