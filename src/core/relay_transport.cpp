#include "relay_transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <chrono>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#else
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::relay {
namespace {
#if defined(_WIN32)
using NativeSocket = SOCKET;
using AddressSize = int;
int LastError() {
  return WSAGetLastError();
}
#else
using NativeSocket = int;
using AddressSize = socklen_t;
int LastError() {
  return errno;
}
#endif
Error SocketError(const char* operation) {
  return Error{ErrorCategory::IO, operation, LastError()};
}
sockaddr_in Address(Endpoint endpoint) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = endpoint.ipv4;
  address.sin_port = htons(endpoint.port);
  return address;
}
}  // namespace

Result<std::unique_ptr<UdpSocket>> UdpSocket::Open(std::string_view bind, uint16_t port) {
  auto result = std::unique_ptr<UdpSocket>(new UdpSocket);
#if defined(_WIN32)
  WSADATA data{};
  const int error = WSAStartup(MAKEWORD(2, 2), &data);
  if (error)
    return std::unexpected(Error{ErrorCategory::IO, "WSAStartup failed", error});
  result->started_ = true;
#endif
  result->handle_ = static_cast<int64_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (result->handle_ == -1)
    return std::unexpected(SocketError("UDP socket failed"));
#if defined(_WIN32)
  BOOL connection_reset = FALSE;
  DWORD returned = 0;
  if (WSAIoctl(static_cast<SOCKET>(result->handle_), SIO_UDP_CONNRESET, &connection_reset,
               sizeof(connection_reset), nullptr, 0, &returned, nullptr, nullptr) != 0)
    return std::unexpected(SocketError("UDP connection-reset suppression failed"));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  const std::string text(bind);
  if (inet_pton(AF_INET, text.c_str(), &address.sin_addr) != 1)
    return std::unexpected(Error{ErrorCategory::Config, "--bind requires an IPv4 address"});
  if (::bind(static_cast<NativeSocket>(result->handle_), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0)
    return std::unexpected(SocketError("UDP bind failed"));
  AddressSize size = sizeof(address);
  if (getsockname(static_cast<NativeSocket>(result->handle_), reinterpret_cast<sockaddr*>(&address),
                  &size) != 0)
    return std::unexpected(SocketError("UDP getsockname failed"));
  result->endpoint_ = {address.sin_addr.s_addr, ntohs(address.sin_port)};
  return result;
}

UdpSocket::~UdpSocket() {
  if (handle_ != -1) {
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(handle_));
#else
    close(static_cast<int>(handle_));
#endif
  }
#if defined(_WIN32)
  if (started_)
    WSACleanup();
#endif
}

Result<std::optional<Datagram>> UdpSocket::Receive(uint32_t timeout_ms) {
  const auto handle = static_cast<NativeSocket>(handle_);
#if defined(_WIN32)
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(handle, &read_set);
  timeval timeout{static_cast<long>(timeout_ms / 1000),
                  static_cast<long>((timeout_ms % 1000) * 1000)};
  const int ready = select(static_cast<int>(handle + 1), &read_set, nullptr, nullptr, &timeout);
#else
  pollfd descriptor{handle, POLLIN, 0};
  const int ready = poll(&descriptor, 1, static_cast<int>(std::min(timeout_ms, uint32_t{INT_MAX})));
  if (ready < 0 && errno == EINTR)
    return std::optional<Datagram>{};
#endif
  if (ready < 0)
    return std::unexpected(SocketError("UDP select failed"));
  if (!ready)
    return std::optional<Datagram>{};
  // Read the entire IPv4 UDP datagram so an oversized packet cannot become a valid prefix.
  std::array<char, 65536> buffer{};
  sockaddr_in source{};
  AddressSize source_size = sizeof(source);
  const auto count = recvfrom(handle, buffer.data(), static_cast<int>(buffer.size()), 0,
                              reinterpret_cast<sockaddr*>(&source), &source_size);
  if (count < 0) {
#if defined(_WIN32)
    if (WSAGetLastError() == WSAECONNRESET) {
      ++connection_resets_;
      return std::optional<Datagram>{};
    }
#else
    if (errno == EINTR)
      return std::optional<Datagram>{};
#endif
    return std::unexpected(SocketError("UDP receive failed"));
  }
  Datagram datagram{{source.sin_addr.s_addr, ntohs(source.sin_port)}, {}};
  datagram.bytes.assign(buffer.data(), buffer.data() + count);
  return std::optional<Datagram>{std::move(datagram)};
}

VoidResult UdpSocket::Send(const Datagram& datagram) {
  const auto address = Address(datagram.endpoint);
  const auto count = sendto(static_cast<NativeSocket>(handle_),
                            reinterpret_cast<const char*>(datagram.bytes.data()),
                            static_cast<int>(datagram.bytes.size()), 0,
                            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  if (count < 0 || static_cast<size_t>(count) != datagram.bytes.size())
    return std::unexpected(SocketError("UDP send failed"));
  return Ok();
}

bool Pump(UdpSocket& socket, Core& core, uint64_t now, TransportCounters& counters,
          uint32_t timeout_ms) {
  const auto start = std::chrono::steady_clock::now();
  auto received = socket.Receive(timeout_ms);
  now += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
  core.Tick(now);
  for (const auto& notice : core.TakeNotices())
    if (!socket.Send(notice))
      ++counters.send_errors;
  if (!received) {
    ++counters.receive_errors;
    return false;
  }
  if (!*received)
    return false;
  for (auto& datagram : core.Input((**received).endpoint, (**received).bytes, now)) {
    if (!socket.Send(datagram))
      ++counters.send_errors;
    if (datagram.bytes.size() > 5 && datagram.bytes[5] == static_cast<uint8_t>(Type::kTokenGrant))
      SecureWipe(datagram.bytes);
  }
  return true;
}

}  // namespace rex::relay
