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

namespace rex::kernel::xam::lan {
namespace {

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

bool LanSystemLinkState::Configure(std::string_view local_address, uint64_t local_xuid) {
  uint32_t parsed_address = 0;
  if (!local_xuid || !ParseIpv4(local_address, &parsed_address)) {
    Reset();
    return false;
  }

  std::lock_guard lock(mutex_);
  ResetLocked();
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

void LanSystemLinkState::ResetLocked() {
  enabled_ = false;
  local_xuid_ = 0;
  local_xnaddr_ = {};
  address_mappings_.clear();
  qos_ = {};
  stats_ = {};
}

}  // namespace rex::kernel::xam::lan
