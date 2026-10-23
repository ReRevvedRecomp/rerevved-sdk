#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rex::system {
class XSocket;
}  // namespace rex::system

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::kernel::xam::lan {

constexpr uint32_t kQosListenEnable = 0x00000001;
constexpr uint32_t kQosListenDisable = 0x00000002;
constexpr uint32_t kQosListenSetData = 0x00000004;
constexpr uint32_t kQosListenSetBitsPerSecond = 0x00000008;
constexpr uint32_t kQosListenRelease = 0x00000010;

// XSESSION_CREATE_HOST: the create fills the caller's XSESSION_INFO.
constexpr uint32_t kSessionCreateHost = 0x00000001;
constexpr size_t kSessionInfoSize = 60;
constexpr size_t kSessionNonceSize = 8;
constexpr uint32_t kSessionDetailsSize = 124;

struct SessionSnapshot {
  bool created = false;
  uint32_t flags = 0;
  uint32_t max_public_slots = 0;
  uint32_t max_private_slots = 0;
  std::array<uint8_t, kSessionInfoSize> info{};
  std::array<uint8_t, kSessionNonceSize> nonce{};
  uint32_t joined_public = 0;
  uint32_t joined_private = 0;
  // Members a Live directory record reported before this client joined. They
  // occupy slots alongside the joined counts until membership updates name them.
  uint32_t filled_public = 0;
  uint32_t filled_private = 0;
  uint32_t user_index = 0;
  uint32_t state = 0;
};

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

enum class SocketKind {
  kUnknown,
  kDatagram,
  kVdp,
  kStream,
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

  ~LanSystemLinkState();

  // Reconfiguring an enabled state keeps the socket registry and claims, so a
  // repeated XNetStartup does not release ports held by open sockets.
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

  void CreateSession(const SessionSnapshot& session);
  void JoinSession(uint32_t public_members, uint32_t private_members);
  // Removes members, clamping each count at zero.
  void LeaveSession(uint32_t public_members, uint32_t private_members);
  // Removes record-reported members of each slot kind; `either` members of
  // unknown kind take public counts first. Returns the {public, private}
  // members no record count covered.
  std::pair<uint32_t, uint32_t> ConsumeFilled(uint32_t public_members, uint32_t private_members,
                                              uint32_t either = 0);
  void ModifySession(uint32_t flags, uint32_t max_public_slots, uint32_t max_private_slots);
  void DeleteSession();
  void SetSessionState(uint32_t state);
  SessionSnapshot session_snapshot() const;

  // Guest socket kinds are recorded only while enabled. Forgetting a socket
  // releases its port claim.
  void RegisterSocket(uint32_t handle, SocketKind kind);
  SocketKind socket_kind(uint32_t handle) const;
  void ForgetSocket(uint32_t handle);

  // A registered socket known to be bound skips the pre-send bind check.
  void MarkBound(uint32_t handle);
  bool NeedsBindBeforeSend(uint32_t handle) const;

  // Claim the native wildcard bind for `port`/`handle`; instances cannot share
  // a broadcast port. Return 0/native error; set `claimed` only for new claims,
  // retain existing claims, and return 0 when unsupported.
  int ClaimDatagramPort(uint32_t handle, uint16_t port, bool* claimed);
  void ReleasePortClaim(uint32_t handle);
  bool HasPortClaim(uint32_t handle) const;

 private:
  struct SocketEntry {
    SocketKind kind = SocketKind::kUnknown;
    int64_t port_claim = -1;
    bool bound = false;
  };

  void ResetIdentityLocked();
  void ResetLocked();

  mutable std::mutex mutex_;
  bool enabled_ = false;
  uint64_t local_xuid_ = 0;
  XnAddrSnapshot local_xnaddr_{};
  std::vector<std::pair<uint32_t, XnAddrSnapshot>> address_mappings_;
  QosListenSnapshot qos_{};
  NetworkStatsSnapshot stats_{};
  SessionSnapshot session_{};
  std::unordered_map<uint32_t, SocketEntry> sockets_;
};

// Binds a socket the guest never bound to `local_ipv4` (network order) and an
// ephemeral port, so its sends leave the selected interface and it receives
// broadcasts there. Returns true only when this call performed the bind.
bool BindUnboundToLocalAddress(rex::system::XSocket& socket, uint32_t local_ipv4);

// The process-wide LAN state used by the XNet exports.
LanSystemLinkState& SystemLinkState();

// Enabled LAN host creates with local address, kSessionCreateHost, and
// session_info_ptr write fresh XSESSION_INFO and return true; other creates
// touch no memory. Nonzero nonce_ptr gets a random nonzero 8-byte nonce.
bool FillHostSessionInfo(const LanSystemLinkState& state, rex::memory::Memory& memory,
                         uint32_t flags, uint32_t session_info_ptr, uint32_t nonce_ptr);

// Creates and retains the host output or client input, resetting prior membership.
void CreateSession(LanSystemLinkState& state, rex::memory::Memory& memory, uint32_t flags,
                   uint32_t public_slots, uint32_t private_slots, uint32_t user_index,
                   uint32_t session_info_ptr, uint32_t nonce_ptr);
void JoinSession(LanSystemLinkState& state, rex::memory::Memory& memory, uint32_t user_count,
                 uint32_t private_slots_ptr);

// Returns Win32 status; disabled LAN or a missing record leaves guest memory untouched.
uint32_t FillSessionDetails(const LanSystemLinkState& state, rex::memory::Memory& memory,
                            uint32_t buffer_size_ptr, uint32_t details_ptr);

}  // namespace rex::kernel::xam::lan
