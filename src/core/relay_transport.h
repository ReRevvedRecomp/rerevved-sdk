#pragma once

#include "relay_core.h"

#include <memory>
#include <optional>
#include <rex/result.h>

namespace rex::relay {

// Single-threaded UDP owner. A receive returns no datagram on timeout.
class UdpSocket {
 public:
  static Result<std::unique_ptr<UdpSocket>> Open(std::string_view bind, uint16_t port);
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  Endpoint endpoint() const { return endpoint_; }
  uint64_t connection_resets() const { return connection_resets_; }
  Result<std::optional<Datagram>> Receive(uint32_t timeout_ms);
  VoidResult Send(const Datagram& datagram);

 private:
  UdpSocket() = default;
  int64_t handle_ = -1;
  bool started_ = false;
  Endpoint endpoint_;
  uint64_t connection_resets_ = 0;
};

struct TransportCounters {
  uint64_t receive_errors = 0;
  uint64_t send_errors = 0;
};

// The CLI and loopback test share this exact byte-moving network loop iteration.
bool Pump(UdpSocket& socket, Core& core, uint64_t now, TransportCounters& counters,
          uint32_t timeout_ms = 0);

}  // namespace rex::relay
