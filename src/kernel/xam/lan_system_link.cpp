#include "lan_system_link.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <rex/system/xsocket.h>

namespace rex::kernel::xam::lan {
namespace {

void ClosePortClaim(int64_t claim) {
#if REX_PLATFORM_WIN32
  if (claim >= 0) {
    closesocket(static_cast<SOCKET>(claim));
  }
#else
  (void)claim;
#endif
}

std::array<uint8_t, 6> BuildEthernetIdentity(uint64_t xuid) {
  uint64_t mixed = xuid;
  mixed ^= mixed >> 33;
  mixed *= 0xff51afd7ed558ccdULL;
  mixed ^= mixed >> 33;
  mixed *= 0xc4ceb9fe1a85ec53ULL;
  mixed ^= mixed >> 33;

  std::array<uint8_t, 6> identity{};
  for (size_t i = 0; i < identity.size(); ++i) {
    identity[i] = static_cast<uint8_t>(mixed >> (i * 8));
  }
  identity[0] = static_cast<uint8_t>((identity[0] & 0xFC) | 0x02);
  return identity;
}

bool ParseIpv4(std::string_view text, uint32_t* raw_address) {
  if (text.empty() || !raw_address) {
    return false;
  }
  std::string terminated(text);
  in_addr address{};
  if (inet_pton(AF_INET, terminated.c_str(), &address) != 1 || address.s_addr == 0) {
    return false;
  }
  *raw_address = address.s_addr;
  return true;
}

}  // namespace

LanSystemLinkState::~LanSystemLinkState() {
  std::lock_guard lock(mutex_);
  ResetLocked();
}

bool LanSystemLinkState::Configure(std::string_view local_address, uint64_t local_xuid) {
  uint32_t parsed_address = 0;
  if (!local_xuid || !ParseIpv4(local_address, &parsed_address)) {
    Reset();
    return false;
  }

  std::lock_guard lock(mutex_);
  if (enabled_) {
    ResetIdentityLocked();
  } else {
    ResetLocked();
  }
  enabled_ = true;
  local_xuid_ = local_xuid;
  local_xnaddr_.ipv4 = parsed_address;
  local_xnaddr_.ethernet = BuildEthernetIdentity(local_xuid);
  return true;
}

void LanSystemLinkState::Reset() {
  std::lock_guard lock(mutex_);
  ResetLocked();
}

bool LanSystemLinkState::enabled() const {
  std::lock_guard lock(mutex_);
  return enabled_;
}

void LanSystemLinkState::CreateSession(const SessionSnapshot& session) {
  std::lock_guard lock(mutex_);
  if (enabled_) {
    session_ = session;
    session_.created = true;
    session_.joined_public = 0;
    session_.joined_private = 0;
    session_.state = 0;
  }
}

void LanSystemLinkState::JoinSession(uint32_t public_members, uint32_t private_members) {
  std::lock_guard lock(mutex_);
  if (enabled_ && session_.created) {
    session_.joined_public += public_members;
    session_.joined_private += private_members;
  }
}

void LanSystemLinkState::LeaveSession(uint32_t public_members, uint32_t private_members) {
  std::lock_guard lock(mutex_);
  if (enabled_ && session_.created) {
    session_.joined_public -= std::min(session_.joined_public, public_members);
    session_.joined_private -= std::min(session_.joined_private, private_members);
  }
}

std::pair<uint32_t, uint32_t> LanSystemLinkState::ConsumeFilled(uint32_t public_members,
                                                                uint32_t private_members,
                                                                uint32_t either) {
  std::lock_guard lock(mutex_);
  if (!enabled_ || !session_.created)
    return {public_members + either, private_members};
  auto take = [](uint32_t& filled, uint32_t& members) {
    const uint32_t taken = std::min(filled, members);
    filled -= taken;
    members -= taken;
  };
  take(session_.filled_public, public_members);
  take(session_.filled_private, private_members);
  take(session_.filled_public, either);
  take(session_.filled_private, either);
  return {public_members + either, private_members};
}

void LanSystemLinkState::ModifySession(uint32_t flags, uint32_t max_public_slots,
                                       uint32_t max_private_slots) {
  std::lock_guard lock(mutex_);
  if (enabled_ && session_.created) {
    session_.flags = flags;
    session_.max_public_slots = max_public_slots;
    session_.max_private_slots = max_private_slots;
  }
}

void LanSystemLinkState::DeleteSession() {
  std::lock_guard lock(mutex_);
  session_ = {};
}

void LanSystemLinkState::SetSessionState(uint32_t state) {
  std::lock_guard lock(mutex_);
  if (enabled_ && session_.created) {
    session_.state = state;
  }
}

SessionSnapshot LanSystemLinkState::session_snapshot() const {
  std::lock_guard lock(mutex_);
  return session_;
}

uint32_t LanSystemLinkState::local_ipv4() const {
  std::lock_guard lock(mutex_);
  return local_xnaddr_.ipv4;
}

uint64_t LanSystemLinkState::local_xuid() const {
  std::lock_guard lock(mutex_);
  return local_xuid_;
}

XnAddrSnapshot LanSystemLinkState::local_xnaddr() const {
  std::lock_guard lock(mutex_);
  return local_xnaddr_;
}

uint32_t LanSystemLinkState::SelectBindIpv4(uint32_t requested_ipv4) const {
  std::lock_guard lock(mutex_);
  return enabled_ && !requested_ipv4 ? local_xnaddr_.ipv4 : requested_ipv4;
}

bool LanSystemLinkState::XnAddrToInAddr(const XnAddrSnapshot& xnaddr, uint32_t* in_addr) {
  if (!in_addr || !xnaddr.ipv4) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return false;
  }

  auto existing = std::find_if(address_mappings_.begin(), address_mappings_.end(),
                               [&](const auto& entry) { return entry.first == xnaddr.ipv4; });
  if (existing != address_mappings_.end()) {
    existing->second = xnaddr;
  } else {
    if (address_mappings_.size() == kMaxAddressMappings) {
      address_mappings_.erase(address_mappings_.begin());
    }
    address_mappings_.emplace_back(xnaddr.ipv4, xnaddr);
  }
  *in_addr = xnaddr.ipv4;
  return true;
}

bool LanSystemLinkState::InAddrToXnAddr(uint32_t in_addr, XnAddrSnapshot* xnaddr) const {
  if (!xnaddr || !in_addr) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return false;
  }
  auto existing = std::find_if(address_mappings_.begin(), address_mappings_.end(),
                               [&](const auto& entry) { return entry.first == in_addr; });
  if (existing == address_mappings_.end()) {
    return false;
  }
  *xnaddr = existing->second;
  return true;
}

QosUpdateResult LanSystemLinkState::UpdateQos(std::span<const uint8_t> payload,
                                              uint32_t bits_per_second, uint32_t flags) {
  constexpr uint32_t kKnownFlags = kQosListenEnable | kQosListenDisable | kQosListenSetData |
                                   kQosListenSetBitsPerSecond | kQosListenRelease;
  if (!flags || (flags & ~kKnownFlags)) {
    return QosUpdateResult::kInvalidFlags;
  }
  if ((flags & kQosListenSetData) && payload.size() > kMaxQosPayloadSize) {
    return QosUpdateResult::kPayloadTooLarge;
  }

  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return QosUpdateResult::kNotConfigured;
  }
  if (flags & kQosListenRelease) {
    qos_ = {};
    return QosUpdateResult::kSuccess;
  }
  if (flags & kQosListenEnable) {
    qos_.enabled = true;
  }
  if (flags & kQosListenDisable) {
    qos_.enabled = false;
  }
  if (flags & kQosListenSetData) {
    qos_.payload.assign(payload.begin(), payload.end());
  }
  if (flags & kQosListenSetBitsPerSecond) {
    qos_.bits_per_second = bits_per_second;
  }
  qos_.last_flags = flags;
  return QosUpdateResult::kSuccess;
}

QosListenSnapshot LanSystemLinkState::qos_snapshot() const {
  std::lock_guard lock(mutex_);
  return qos_;
}

void LanSystemLinkState::RecordSend(size_t bytes) {
  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return;
  }
  ++stats_.sent_packets;
  stats_.sent_bytes += bytes;
}

void LanSystemLinkState::RecordReceive(size_t bytes) {
  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return;
  }
  ++stats_.received_packets;
  stats_.received_bytes += bytes;
}

NetworkStatsSnapshot LanSystemLinkState::stats_snapshot() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

void LanSystemLinkState::RegisterSocket(uint32_t handle, SocketKind kind) {
  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return;
  }
  auto& entry = sockets_[handle];
  ClosePortClaim(entry.port_claim);
  entry = {kind, -1};
}

SocketKind LanSystemLinkState::socket_kind(uint32_t handle) const {
  std::lock_guard lock(mutex_);
  auto entry = sockets_.find(handle);
  return entry == sockets_.end() ? SocketKind::kUnknown : entry->second.kind;
}

void LanSystemLinkState::ForgetSocket(uint32_t handle) {
  std::lock_guard lock(mutex_);
  auto entry = sockets_.find(handle);
  if (entry == sockets_.end()) {
    return;
  }
  ClosePortClaim(entry->second.port_claim);
  sockets_.erase(entry);
}

void LanSystemLinkState::MarkBound(uint32_t handle) {
  std::lock_guard lock(mutex_);
  auto entry = sockets_.find(handle);
  if (entry != sockets_.end()) {
    entry->second.bound = true;
  }
}

bool LanSystemLinkState::NeedsBindBeforeSend(uint32_t handle) const {
  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return false;
  }
  auto entry = sockets_.find(handle);
  return entry == sockets_.end() ||
         (entry->second.kind != SocketKind::kStream && !entry->second.bound);
}

int LanSystemLinkState::ClaimDatagramPort(uint32_t handle, uint16_t port, bool* claimed) {
  if (claimed) {
    *claimed = false;
  }
#if REX_PLATFORM_WIN32
  std::lock_guard lock(mutex_);
  auto entry = sockets_.find(handle);
  if (!enabled_ || !port || entry == sockets_.end() || entry->second.port_claim >= 0) {
    return 0;
  }

  SOCKET claim = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (claim == INVALID_SOCKET) {
    return WSAGetLastError();
  }
  // The claim is never read; an empty receive buffer drops its datagram copies.
  int receive_buffer = 0;
  setsockopt(claim, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_buffer),
             sizeof(receive_buffer));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(claim, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    const int error = WSAGetLastError();
    closesocket(claim);
    return error;
  }
  entry->second.port_claim = static_cast<int64_t>(claim);
  if (claimed) {
    *claimed = true;
  }
  return 0;
#else
  (void)handle;
  (void)port;
  return 0;
#endif
}

void LanSystemLinkState::ReleasePortClaim(uint32_t handle) {
  std::lock_guard lock(mutex_);
  auto entry = sockets_.find(handle);
  if (entry == sockets_.end()) {
    return;
  }
  ClosePortClaim(entry->second.port_claim);
  entry->second.port_claim = -1;
}

bool LanSystemLinkState::HasPortClaim(uint32_t handle) const {
  std::lock_guard lock(mutex_);
  auto entry = sockets_.find(handle);
  return entry != sockets_.end() && entry->second.port_claim >= 0;
}

void LanSystemLinkState::ResetIdentityLocked() {
  local_xuid_ = 0;
  local_xnaddr_ = {};
  address_mappings_.clear();
  qos_ = {};
  stats_ = {};
}

void LanSystemLinkState::ResetLocked() {
  session_ = {};
  enabled_ = false;
  ResetIdentityLocked();
  for (const auto& [handle, entry] : sockets_) {
    ClosePortClaim(entry.port_claim);
  }
  sockets_.clear();
}

bool BindUnboundToLocalAddress(rex::system::XSocket& socket, uint32_t local_ipv4) {
  if (!local_ipv4) {
    return false;
  }
  // Windows reports an unbound socket as an error; other platforms report port 0.
  rex::system::N_XSOCKADDR_IN current{};
  uint32_t current_size = sizeof(current);
  if (socket.GetSockName(&current, &current_size) == X_STATUS_SUCCESS &&
      static_cast<uint16_t>(current.sin_port) != 0) {
    return false;
  }

  rex::system::N_XSOCKADDR_IN address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr = ntohl(local_ipv4);
  std::memset(address.x_sin_zero, 0, sizeof(address.x_sin_zero));
  return socket.Bind(&address, sizeof(address)) == X_STATUS_SUCCESS;
}

}  // namespace rex::kernel::xam::lan
