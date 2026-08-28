#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rex::kernel::xam::lan {

constexpr uint32_t kQosListenEnable = 0x00000001;
constexpr uint32_t kQosListenDisable = 0x00000002;
constexpr uint32_t kQosListenSetData = 0x00000004;
constexpr uint32_t kQosListenSetBitsPerSecond = 0x00000008;
constexpr uint32_t kQosListenRelease = 0x00000010;

struct XnAddrSnapshot {
  uint32_t ipv4 = 0;
  uint32_t online_ipv4 = 0;
  uint16_t online_port = 0;
  std::array<uint8_t, 6> ethernet{};
  std::array<uint8_t, 20> online{};

  bool operator==(const XnAddrSnapshot&) const = default;
};

struct QosListenSnapshot {
  bool enabled = false;
  uint32_t bits_per_second = 0;
  uint32_t last_flags = 0;
  std::vector<uint8_t> payload;
};

struct NetworkStatsSnapshot {
  uint64_t sent_packets = 0;
  uint64_t sent_bytes = 0;
  uint64_t received_packets = 0;
  uint64_t received_bytes = 0;

  bool operator==(const NetworkStatsSnapshot&) const = default;
};

enum class QosUpdateResult {
  kSuccess,
  kNotConfigured,
  kInvalidFlags,
  kPayloadTooLarge,
};

class LanSystemLinkState {
 public:
  static constexpr size_t kMaxAddressMappings = 64;
  static constexpr size_t kMaxQosPayloadSize = 4096;

  bool Configure(std::string_view local_address, uint64_t local_xuid);
  void Reset();

  bool enabled() const;
  uint32_t local_ipv4() const;
  uint64_t local_xuid() const;
  XnAddrSnapshot local_xnaddr() const;
  uint32_t SelectBindIpv4(uint32_t requested_ipv4) const;

  bool XnAddrToInAddr(const XnAddrSnapshot& xnaddr, uint32_t* in_addr);
  bool InAddrToXnAddr(uint32_t in_addr, XnAddrSnapshot* xnaddr) const;

  QosUpdateResult UpdateQos(std::span<const uint8_t> payload, uint32_t bits_per_second,
                            uint32_t flags);
  QosListenSnapshot qos_snapshot() const;

  void RecordSend(size_t bytes);
  void RecordReceive(size_t bytes);
  NetworkStatsSnapshot stats_snapshot() const;

 private:
  void ResetLocked();

  mutable std::mutex mutex_;
  bool enabled_ = false;
  uint64_t local_xuid_ = 0;
  XnAddrSnapshot local_xnaddr_{};
  std::vector<std::pair<uint32_t, XnAddrSnapshot>> address_mappings_;
  QosListenSnapshot qos_{};
  NetworkStatsSnapshot stats_{};
};

}  // namespace rex::kernel::xam::lan
