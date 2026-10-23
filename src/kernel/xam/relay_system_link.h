#pragma once

#include "../../core/relay_transport.h"
#include "../../core/relay_invite.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rex::system {
struct N_XSOCKADDR_IN;
}

namespace rex::kernel::xam::relay_link {

// Socket errors use the guest Winsock values on every host.
constexpr int kWouldBlock = 10035;
constexpr int kAddressInUse = 10048;
constexpr int kNetworkDown = 10050;
constexpr int kNetworkUnreachable = 10051;
constexpr int kMessageSize = 10040;
constexpr size_t kEnvelopeSize = 14;
constexpr size_t kMaxQueued = 256;

bool IsVirtualAddress(uint32_t ipv4);
bool CanRoute(uint32_t ipv4);
relay::Bytes Wrap(uint64_t token, relay::Endpoint source, std::span<const uint8_t> payload);
std::optional<relay::Datagram> Unwrap(uint64_t token, std::span<const uint8_t> bytes);
Result<relay::Endpoint> Resolve(std::string_view address);
Result<relay::InviteAddress> ParseAddress(std::string_view address);
// One movable allocation, wiped on destruction and once WELCOME derives K_assoc.
class Secret {
 public:
  Secret();
  Secret(const char* text);
  explicit Secret(std::string_view bytes);
  ~Secret();
  Secret(Secret&&) = default;
  Secret(const Secret&) = delete;
  std::string_view view() const;
  void Clear();

 private:
  friend Result<Secret> ReadSecret(std::string_view path);
  static constexpr size_t kCapacity = 65537;
  std::unique_ptr<char[]> bytes_;
  size_t offset_ = 0;
  size_t size_ = 0;
};
Result<Secret> ReadSecret(std::string_view path);
Result<relay::Digest> HostFingerprint();

// A relay-issued join token. Its secret uses the same wiped storage as Secret.
struct JoinToken {
  relay::TokenId id{};
  Secret secret;
  uint32_t expiry_seconds = 0;
};

// Directory requests complete on the client thread; callers poll without blocking.
using RequestHandle = uint32_t;
enum class RequestState { kPending, kDone, kFailed };
enum class RequestFailure { kNone, kTimeout, kAssociation, kRejected, kInvalid };
struct RecordsPoll {
  RequestState state = RequestState::kFailed;
  RequestFailure failure = RequestFailure::kInvalid;
  std::vector<relay::SessionRecord> records;
};
struct TokenPoll {
  RequestState state = RequestState::kFailed;
  RequestFailure failure = RequestFailure::kInvalid;
  std::optional<JoinToken> token;
};
constexpr size_t kMaxRequests = 64;
constexpr size_t kMaxUpdates = 64;
constexpr unsigned kMaxUpdateAttempts = 5;
constexpr uint32_t kDirectoryRetryMs = 1000;
constexpr uint32_t kRequestTimeoutMs = 3000;

// Round-trip probes travel as DATA between guest port 0 on both sides. Guest
// sockets never own port 0, so the receiving client answers or consumes every
// probe itself and none reaches a guest socket.
constexpr uint16_t kProbePort = 0;
constexpr size_t kProbePayloadSize = 16;
constexpr size_t kMaxProbeSets = 64;
constexpr unsigned kMaxProbesPerSet = 8;
constexpr uint32_t kProbeTimeoutMs = 1000;
// A set that cannot send every probe within this window sends no more.
constexpr uint32_t kProbeSendWindowMs = 2000;
constexpr double kProbesPerSecond = 40;
constexpr double kProbeRepliesPerSecond = 80;
// SDK side-channel payloads also travel as DATA between guest ports 0 and start
// with this magic; the client hands them to the side-channel handler instead
// of the probe logic. See "Host session data" in docs/MAINTAINING.md.
constexpr std::string_view kSideChannelMagic = "RXSD";
using SideChannelHandler = std::function<void(uint32_t source_ipv4, std::span<const uint8_t>)>;
struct ProbePoll {
  RequestState state = RequestState::kFailed;
  RequestFailure failure = RequestFailure::kInvalid;
  unsigned sent = 0;
  std::vector<uint32_t> rtt_ms;  // one entry per answered probe, in reply order
};
// Continuous refill at `rate` per second with a burst of `rate`.
class TokenBucket {
 public:
  using Clock = std::chrono::steady_clock;
  explicit TokenBucket(double rate) : rate_(rate), tokens_(rate) {}
  bool Take(Clock::time_point now);

 private:
  double rate_;
  double tokens_;
  Clock::time_point last_{};
  bool started_ = false;
};

// Direct UDP paths between room members; see "Direct paths" in docs/SYSTEM_LINK_RELAY.md.
constexpr uint32_t kDirectProbeIntervalMs = 200;
constexpr uint32_t kDirectProbeWindowMs = 5000;
// An up path sends KEEPALIVE at this interval and goes down when no KEEPALIVE_ACK
// or PROBE_ACK has arrived for kDirectAckTimeoutMs; received data does not count.
constexpr uint32_t kDirectKeepaliveMs = 500;
constexpr uint32_t kDirectAckTimeoutMs = 2000;
constexpr uint32_t kConnectRetryMs = 1000;
constexpr uint32_t kConnectTimeoutMs = 3000;
// CANDIDATES go at WELCOME and twice more, kCandidateIntervalMs apart.
constexpr unsigned kCandidateSends = 3;
constexpr uint32_t kCandidateIntervalMs = 2000;
struct DirectOptions {
  bool enabled = false;
  // Fault injection: after this many seconds on a direct path, drop every
  // direct datagram received for the rest of the association. Zero is off.
  uint32_t drop_after_seconds = 0;
  // Delay before a new attempt after the first, second and later failures.
  std::array<uint32_t, 3> backoff_ms{30000, 60000, 120000};
};
// Local IPv4 interface endpoints with the given port. A loopback relay uses only
// 127.0.0.1; other relays exclude loopback, link-local and virtual addresses.
std::vector<relay::Candidate> LocalCandidates(relay::Endpoint server, uint16_t port);

class PortTable {
 public:
  static constexpr size_t kMaxSockets = 1024;
  std::expected<uint16_t, int> Bind(uint64_t socket, uint16_t port, uint16_t loopback_port);
  void Release(uint64_t socket);
  uint16_t GuestPort(uint64_t socket) const;
  uint16_t LoopbackPort(uint16_t port) const;
  void Clear();

 private:
  struct Entry {
    uint16_t guest;
    uint16_t loopback;
  };
  std::map<uint64_t, Entry> sockets_;
  uint16_t next_ = 49152;
};

enum class Association { kStopped, kJoining, kLive, kLost, kFailed };
struct Stats {
  Association state = Association::kStopped;
  bool started = false;
  uint32_t ipv4 = 0;
  std::string last_failure;
  uint64_t delivered = 0;
  uint64_t unknown_port = 0;
  uint64_t invalid_wire = 0;
  uint64_t invalid_envelope = 0;
  uint64_t rejected_destination = 0;
  uint64_t queue_full = 0;
  uint64_t oversize = 0;
  uint64_t socket_errors = 0;
  uint64_t connection_resets = 0;
  uint64_t heartbeats = 0;
  uint64_t discarded_pending = 0;
  uint64_t dropped_after_cleanup = 0;
  uint64_t directory_failed = 0;  // PUBLISH or REMOVE rejected or never acknowledged
  uint64_t requests_failed = 0;   // SEARCH, LOOKUP or TOKEN_REQUEST failed or timed out
  uint64_t probes_sent = 0;
  uint64_t probe_replies_received = 0;
  uint64_t probe_replies_sent = 0;
  uint64_t probe_replies_dropped = 0;  // reply budget exhausted, replies disabled or queue full
  uint64_t invalid_probes = 0;         // port-0 payloads that are not a probe or a known reply
  uint64_t side_channel_in = 0;        // side-channel payloads passed to the handler
  uint64_t side_channel_out = 0;       // side-channel payloads queued for the relay

  uint32_t direct_peers = 0;            // peers whose guest unicast currently goes direct
  uint32_t direct_attempted_peers = 0;  // peers with at least one direct attempt
  uint64_t direct_attempts = 0;         // attempts probed after a valid CONNECT_INFO
  uint64_t direct_established = 0;
  uint64_t direct_fallbacks = 0;         // paths that went down while the association stayed live
  uint64_t direct_in = 0;                // authenticated direct datagrams received
  uint64_t direct_out = 0;               // direct datagrams sent
  uint64_t direct_connect_timeouts = 0;  // CONNECT_REQUEST without CONNECT_INFO
  uint64_t direct_probe_failures = 0;    // attempts without an answered probe
  uint64_t direct_bad_tag = 0;           // CONNECT_INFO or CONNECT_CLOSE with a bad tag
  uint64_t direct_malformed = 0;
  uint64_t direct_unknown_peer = 0;  // sender without a current attempt
  uint64_t direct_stale_attempt = 0;
  uint64_t direct_bad_mac = 0;
  uint64_t direct_replay = 0;
  uint64_t direct_fault_drops = 0;  // dropped by DirectOptions::drop_after_seconds
  uint64_t direct_evicted = 0;      // peer entries evicted to make room for another peer
};

// Owns the relay and injector sockets on one thread. Guest handles remain owned
// by XSocket. Start/Stop serialize lifecycle; I/O never waits on the relay socket.
class Client {
 public:
  Client();
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  VoidResult Start(relay::Endpoint server, std::string room, Secret secret,
                   relay::Digest fingerprint);
  // Joins the token's room with a relay-issued token instead of the room secret.
  VoidResult Start(relay::Endpoint server, JoinToken token, relay::Digest fingerprint);
  Stats Stop();
  // Records a startup that never reached the handshake, so guest I/O reports
  // WSAENETDOWN instead of the silent drops that follow the final cleanup.
  void MarkFailed(std::string reason);
  uint32_t ipv4() const;
  // The resolved relay endpoint of the latest Start, or zero before one.
  relay::Endpoint server() const;
  Stats stats() const;
  // Guest handle classification survives association changes and XNetCleanup.
  void RegisterSocket(uint32_t handle, bool datagram);
  bool IsDatagramSocket(uint32_t handle) const;
  void ForgetSocket(uint32_t handle);
  std::expected<void, int> Bind(uint64_t socket, uint16_t port);
  void Release(uint64_t socket);
  uint16_t GuestPort(uint64_t socket) const;
  std::expected<int, int> Send(uint64_t socket, relay::Endpoint destination,
                               std::span<const uint8_t> payload);
  std::expected<int, int> Receive(uint64_t socket, std::span<uint8_t> buffer, int flags,
                                  relay::Endpoint& source);
  std::expected<int, int> ReceiveFrom(uint64_t socket, std::span<uint8_t> buffer, int flags,
                                      system::N_XSOCKADDR_IN& source, uint32_t& source_size);
  // Directory updates are queued and retried in order until acknowledged; false
  // means the association is not live, the queue is full or the record is invalid.
  bool PublishSession(const relay::SessionRecord& record);
  bool RemoveSession(uint64_t session_id);
  // Handle 0 means the request table is full. Done or failed polls consume the handle.
  RequestHandle BeginSearch(uint16_t max_results, uint32_t required_flags);
  RequestHandle BeginLookup(uint64_t session_id);
  RequestHandle BeginTokenRequest(uint64_t session_id);
  RecordsPoll PollRecords(RequestHandle handle);
  TokenPoll PollToken(RequestHandle handle);
  // Measures round trips to a virtual address with 1-kMaxProbesPerSet probes.
  // Handle 0 means the probe table is full. A set is done when every sent probe
  // was answered or timed out; done with no round trips means unreachable.
  RequestHandle BeginProbe(uint32_t ipv4, unsigned count);
  ProbePoll PollProbe(RequestHandle handle);
  // Probes are answered only while enabled; the default is off.
  void SetProbeReplies(bool enabled);
  // Applies to associations started afterwards.
  void SetDirect(DirectOptions options);
  // The handler runs on the client thread, outside the client lock, for every
  // DELIVER from guest port 0 to guest port 0 whose payload starts with
  // kSideChannelMagic. It may call SendSideChannel.
  void SetSideChannelHandler(SideChannelHandler handler);
  // While live, queue side-channel payloads only to virtual unicast guest port
  // 0 via the relay; reject nonmagic, oversized, full-queue, or non-virtual
  // destinations.
  bool SendSideChannel(uint32_t ipv4, std::span<const uint8_t> payload);

 private:
  VoidResult Associate(relay::Endpoint server, std::string room,
                       std::optional<relay::TokenId> token, Secret secret,
                       relay::Digest fingerprint);
  RequestHandle BeginRequest(relay::Message message);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

Client& SystemLinkClient();

}  // namespace rex::kernel::xam::relay_link
