#include "relay_system_link.h"
#include "../../core/relay_invite.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <rex/crypto/sha256.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/version.h>
#include <rex/system/xsocket.h>

namespace rex::kernel::xam::relay_link {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
#if defined(_WIN32)
using Native = SOCKET;
using AddressSize = int;
#else
using Native = int;
using AddressSize = socklen_t;
#endif
int SocketError() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  switch (errno) {
    case EAGAIN:
      return kWouldBlock;
    case EADDRINUSE:
      return kAddressInUse;
    case EMSGSIZE:
      return kMessageSize;
    case EINVAL:
      return 10022;
    case ENOTSOCK:
      return 10038;
    case EINTR:
      return 10004;
    default:
      return kNetworkDown;
  }
#endif
}
struct Winsock {
  bool ready = true;
  Winsock() {
#if defined(_WIN32)
    WSADATA data{};
    ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
  }
  ~Winsock() {
#if defined(_WIN32)
    if (ready)
      WSACleanup();
#endif
  }
};
sockaddr_in Address(relay::Endpoint endpoint) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = endpoint.ipv4;
  address.sin_port = htons(endpoint.port);
  return address;
}
struct Socket {
  Native handle = static_cast<Native>(-1);
  relay::Endpoint endpoint;
  ~Socket() {
    if (handle == static_cast<Native>(-1))
      return;
#if defined(_WIN32)
    closesocket(handle);
#else
    close(handle);
#endif
  }
  bool Open(uint32_t address) {
    handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == static_cast<Native>(-1))
      return false;
    auto bind_address = Address({address, 0});
    if (::bind(handle, reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address)))
      return false;
    AddressSize size = sizeof(bind_address);
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&bind_address), &size))
      return false;
    endpoint = {bind_address.sin_addr.s_addr, ntohs(bind_address.sin_port)};
#if defined(_WIN32)
    BOOL resets = FALSE;
    DWORD returned = 0;
    if (WSAIoctl(handle, SIO_UDP_CONNRESET, &resets, sizeof(resets), nullptr, 0, &returned, nullptr,
                 nullptr))
      return false;
    u_long nonblocking = 1;
    return ioctlsocket(handle, FIONBIO, &nonblocking) == 0;
#else
    return fcntl(handle, F_SETFL, O_NONBLOCK) == 0;
#endif
  }
  bool Send(relay::Endpoint destination, std::span<const uint8_t> bytes) {
    auto address = Address(destination);
    return sendto(handle, reinterpret_cast<const char*>(bytes.data()),
                  static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == static_cast<int>(bytes.size());
  }
};
std::optional<relay::Digest> FromHex(std::string_view text) {
  if (text.size() != 64)
    return {};
  relay::Digest result{};
  for (size_t i = 0; i < result.size(); ++i) {
    unsigned value = 0;
    auto parsed = std::from_chars(text.data() + i * 2, text.data() + i * 2 + 2, value, 16);
    if (parsed.ec != std::errc{})
      return {};
    result[i] = static_cast<uint8_t>(value);
  }
  return result;
}
const char* Rejection(relay::Reason reason) {
  switch (reason) {
    case relay::Reason::kVersion:
      return "protocol version";
    case relay::Reason::kMalformed:
      return "malformed handshake";
    case relay::Reason::kAuth:
      return "wrong secret";
    case relay::Reason::kRoomFull:
      return "room full";
    case relay::Reason::kTooManyRooms:
      return "too many rooms";
    case relay::Reason::kFingerprintMismatch:
      return "different game build";
    case relay::Reason::kRateLimited:
      return "rate limited";
  }
  return "unknown rejection";
}
// Probe payload: ASCII RXQP, kind, sequence, two zero bytes, probe id:u32 LE, four zero bytes.
constexpr uint8_t kProbeRequest = 1;
constexpr uint8_t kProbeReply = 2;
relay::Bytes ProbePayload(uint8_t kind, uint8_t sequence, uint32_t id) {
  relay::Bytes payload(kProbePayloadSize);
  std::memcpy(payload.data(), "RXQP", 4);
  payload[4] = kind;
  payload[5] = sequence;
  for (size_t i = 0; i < 4; ++i)
    payload[8 + i] = static_cast<uint8_t>(id >> (i * 8));
  return payload;
}
struct ProbeFields {
  uint8_t kind = 0;
  uint8_t sequence = 0;
  uint32_t id = 0;
};
bool IsSideChannel(std::span<const uint8_t> payload) {
  return payload.size() >= kSideChannelMagic.size() &&
         std::memcmp(payload.data(), kSideChannelMagic.data(), kSideChannelMagic.size()) == 0;
}
std::optional<ProbeFields> ParseProbe(std::span<const uint8_t> payload) {
  if (payload.size() != kProbePayloadSize || std::memcmp(payload.data(), "RXQP", 4) ||
      (payload[4] != kProbeRequest && payload[4] != kProbeReply) || payload[6] || payload[7] ||
      payload[12] || payload[13] || payload[14] || payload[15])
    return {};
  ProbeFields fields{payload[4], payload[5], 0};
  for (size_t i = 0; i < 4; ++i)
    fields.id |= uint32_t(payload[8 + i]) << (i * 8);
  return fields;
}
}  // namespace

bool TokenBucket::Take(Clock::time_point now) {
  if (!started_) {
    started_ = true;
    last_ = now;
  } else if (now > last_) {
    tokens_ = std::min(rate_, tokens_ + rate_ * std::chrono::duration<double>(now - last_).count());
    last_ = now;
  }
  if (tokens_ < 1)
    return false;
  tokens_ -= 1;
  return true;
}

std::vector<relay::Candidate> LocalCandidates(relay::Endpoint server, uint16_t port) {
  std::vector<relay::Candidate> result;
  auto add = [&](uint32_t ipv4) {
    const auto host = ntohl(ipv4);
    const bool loopback = (host >> 24) == 127;
    const bool link_local = (host >> 16) == 0xa9fe;
    if (!host || loopback || link_local || IsVirtualAddress(ipv4) || host >= 0xe0000000u ||
        result.size() >= relay::kMaxCandidates)
      return;
    const relay::Candidate candidate{ipv4, port};
    if (std::find(result.begin(), result.end(), candidate) == result.end())
      result.push_back(candidate);
  };
  // A loopback relay binds the relay socket to loopback, so only that address is reachable.
  if ((ntohl(server.ipv4) >> 24) == 127) {
    result.push_back({htonl(INADDR_LOOPBACK), port});
    return result;
  }
#if defined(_WIN32)
  // Host-name resolution supplies local interface candidates.
  std::array<char, 256> name{};
  if (gethostname(name.data(), static_cast<int>(name.size() - 1)) == 0) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* resolved = nullptr;
    if (getaddrinfo(name.data(), nullptr, &hints, &resolved) == 0) {
      for (auto* entry = resolved; entry; entry = entry->ai_next)
        add(reinterpret_cast<sockaddr_in*>(entry->ai_addr)->sin_addr.s_addr);
      freeaddrinfo(resolved);
    }
  }
#else
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) == 0) {
    for (auto* entry = interfaces; entry; entry = entry->ifa_next)
      if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_INET)
        add(reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr);
    freeifaddrs(interfaces);
  }
#endif
  return result;
}

bool IsVirtualAddress(uint32_t ipv4) {
  return (ntohl(ipv4) & 0xfffe0000u) == 0xc6120000u;
}
bool CanRoute(uint32_t ipv4) {
  return ipv4 == 0xffffffffu || IsVirtualAddress(ipv4);
}

relay::Bytes Wrap(uint64_t token, relay::Endpoint source, std::span<const uint8_t> payload) {
  if (payload.size() > relay::kMaxPayload)
    return {};
  relay::Bytes result(kEnvelopeSize + payload.size());
  for (size_t i = 0; i < 8; ++i)
    result[i] = static_cast<uint8_t>(token >> (i * 8));
  std::memcpy(result.data() + 8, &source.ipv4, 4);
  result[12] = static_cast<uint8_t>(source.port);
  result[13] = static_cast<uint8_t>(source.port >> 8);
  std::copy(payload.begin(), payload.end(), result.begin() + kEnvelopeSize);
  return result;
}
std::optional<relay::Datagram> Unwrap(uint64_t token, std::span<const uint8_t> bytes) {
  if (bytes.size() < kEnvelopeSize || bytes.size() > kEnvelopeSize + relay::kMaxPayload)
    return {};
  uint64_t actual = 0;
  for (size_t i = 0; i < 8; ++i)
    actual |= uint64_t(bytes[i]) << (i * 8);
  if (actual != token)
    return {};
  relay::Datagram result;
  std::memcpy(&result.endpoint.ipv4, bytes.data() + 8, 4);
  result.endpoint.port = uint16_t(bytes[12]) | (uint16_t(bytes[13]) << 8);
  result.bytes.assign(bytes.begin() + kEnvelopeSize, bytes.end());
  return result;
}
Result<relay::InviteAddress> ParseAddress(std::string_view address) {
  const auto colon = address.rfind(':');
  if (colon == std::string_view::npos || !colon || colon + 1 == address.size())
    return std::unexpected(Error{ErrorCategory::Config, "Relay address requires host:port"});
  const auto number = address.substr(colon + 1);
  unsigned port = 0;
  const auto parsed = std::from_chars(number.data(), number.data() + number.size(), port);
  if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || !port ||
      port > 65535)
    return std::unexpected(Error{ErrorCategory::Config, "Relay port must be 1-65535"});
  return relay::InviteAddress{std::string(address.substr(0, colon)), static_cast<uint16_t>(port)};
}
Result<relay::Endpoint> Resolve(std::string_view address) {
  auto parsed = ParseAddress(address);
  if (!parsed)
    return std::unexpected(parsed.error());
  Winsock winsock;
  if (!winsock.ready)
    return std::unexpected(Error{ErrorCategory::IO, "Relay WSAStartup failed"});
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* resolved = nullptr;
  if (getaddrinfo(parsed->host.c_str(), nullptr, &hints, &resolved))
    return std::unexpected(Error{ErrorCategory::IO, "Cannot resolve relay IPv4 host"});
  relay::Endpoint result{reinterpret_cast<sockaddr_in*>(resolved->ai_addr)->sin_addr.s_addr,
                         parsed->port};
  freeaddrinfo(resolved);
  return result;
}
Secret::Secret() : bytes_(std::make_unique<char[]>(kCapacity)) {}
Secret::Secret(const char* text) : Secret(std::string_view(text)) {}
Secret::Secret(std::string_view bytes) : Secret() {
  if (bytes.size() < kCapacity) {
    size_ = bytes.size();
    std::copy(bytes.begin(), bytes.end(), bytes_.get());
  }
}
Secret::~Secret() {
  Clear();
}
std::string_view Secret::view() const {
  return bytes_ ? std::string_view(bytes_.get() + offset_, size_) : std::string_view{};
}
void Secret::Clear() {
  if (bytes_) {
    volatile char* bytes = bytes_.get();
    for (size_t i = 0; i < kCapacity; ++i)
      bytes[i] = 0;
  }
  offset_ = size_ = 0;
}
Result<Secret> ReadSecret(std::string_view path) {
  Secret secret;
  auto size = relay::ReadSecretFile(rex::to_path(path), {secret.bytes_.get(), Secret::kCapacity});
  if (!size)
    return std::unexpected(size.error());
  secret.size_ = *size;
  auto first = secret.view().find_first_not_of(" \t\r\n\f\v");
  if (first == std::string::npos)
    return std::unexpected(Error{ErrorCategory::Config, "Relay secret is empty"});
  secret.size_ = secret.view().find_last_not_of(" \t\r\n\f\v") - first + 1;
  secret.offset_ = first;
  return secret;
}
Result<relay::Digest> HostFingerprint() {
  static std::mutex mutex;
  static std::optional<relay::Digest> cached;
  std::lock_guard lock(mutex);
  if (cached)
    return *cached;
  auto fingerprint = []() -> Result<relay::Digest> {
#if defined(_WIN32)
    // _get_wpgmptr is unavailable in hosts initialized through a narrow main.
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size())
      return std::unexpected(Error{ErrorCategory::IO, "Cannot locate relay host executable"});
    const std::filesystem::path executable_path(std::wstring_view(path.data(), length));
#else
    const auto executable_path = filesystem::GetExecutablePath();
#endif
    auto executable = FromHex(crypto::sha256_file(executable_path));
    if (!executable)
      return std::unexpected(Error{ErrorCategory::IO, "Cannot hash relay host executable"});
    std::string input = REXGLUE_VERSION_STRING;
    input.append(reinterpret_cast<const char*>(executable->data()), executable->size());
    auto hash = FromHex(crypto::sha256(input));
    if (!hash)
      return std::unexpected(Error{ErrorCategory::Crypto, "Cannot compute relay fingerprint"});
    return *hash;
  }();
  if (fingerprint)
    cached = *fingerprint;
  return fingerprint;
}

std::expected<uint16_t, int> PortTable::Bind(uint64_t socket, uint16_t port, uint16_t loopback) {
  if (sockets_.contains(socket))
    return std::unexpected(10022);
  if (sockets_.size() >= kMaxSockets)
    return std::unexpected(10055);
  if (port && LoopbackPort(port))
    return std::unexpected(kAddressInUse);
  if (!port) {
    do {
      port = next_;
      next_ = next_ == 65535 ? 49152 : static_cast<uint16_t>(next_ + 1);
    } while (LoopbackPort(port));
  }
  sockets_.emplace(socket, Entry{port, loopback});
  return port;
}
void PortTable::Release(uint64_t socket) {
  sockets_.erase(socket);
}
uint16_t PortTable::GuestPort(uint64_t socket) const {
  auto found = sockets_.find(socket);
  return found == sockets_.end() ? 0 : found->second.guest;
}
uint16_t PortTable::LoopbackPort(uint16_t port) const {
  for (const auto& [socket, entry] : sockets_)
    if (entry.guest == port)
      return entry.loopback;
  return 0;
}
void PortTable::Clear() {
  sockets_.clear();
  next_ = 49152;
}

struct Client::Impl {
  std::mutex lifecycle;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::thread worker;
  bool stop = false;
  Stats stats;
  Error failure;
  uint32_t ipv4 = 0;
  uint64_t envelope_token = 0;
  relay::Endpoint server;  // survives resets; replaced by the next Start
  relay::Endpoint injector;
  Socket* wake = nullptr;  // Borrowed only under mutex while the worker owns it.
  PortTable ports;
  std::set<uint32_t> datagram_handles;
  std::deque<relay::Message> queue;
  Clock::time_point last_destination_log{};
  struct Update {
    relay::Message message;
    unsigned attempts = 0;
    Clock::time_point next{};
  };
  // message.request_id is fixed for the request's lifetime, so a retried
  // TOKEN_REQUEST receives the relay's cached grant. RESULTS pages carry the
  // directory snapshot; pages from an older snapshot are discarded.
  struct Request {
    relay::Message message;
    RequestState state = RequestState::kPending;
    RequestFailure failure = RequestFailure::kNone;
    Clock::time_point deadline{};
    Clock::time_point next_send{};
    uint16_t page_count = 0;
    uint32_t snapshot = 0;
    std::map<uint16_t, std::vector<relay::SessionRecord>> pages;
    std::vector<relay::SessionRecord> records;
    std::optional<JoinToken> token;
  };
  std::deque<Update> updates;
  std::map<RequestHandle, Request> requests;
  RequestHandle next_handle = 0;
  uint32_t next_wire_id = 0;
  uint32_t next_update = 0;  // PUBLISH and REMOVE sequence for this association
  struct ProbeSet {
    uint32_t ipv4 = 0;
    uint32_t id = 0;
    unsigned count = 0;
    RequestState state = RequestState::kPending;
    RequestFailure failure = RequestFailure::kNone;
    Clock::time_point begin{};
    std::vector<Clock::time_point> sent_at;
    std::vector<bool> answered;
    std::vector<uint32_t> rtt_ms;
  };
  std::map<RequestHandle, ProbeSet> probes;
  RequestHandle next_probe_handle = 0;
  uint32_t next_probe_id = 0;
  TokenBucket probe_budget{kProbesPerSecond};
  TokenBucket reply_budget{kProbeRepliesPerSecond};
  bool probe_replies = false;
  SideChannelHandler side_channel;
  DirectOptions direct_options;  // copied by each association at its WELCOME

  // Direct paths of one association. Only the worker thread touches this state;
  // counters publish into stats under mutex, which callers must not hold.
  struct DirectLink {
    enum class Phase { kIdle, kRequested, kProbing, kUp };
    struct Candidate {
      relay::Endpoint endpoint;
      bool local = false;
    };
    struct Peer {
      Phase phase = Phase::kIdle;
      bool attempted = false;
      unsigned failures = 0;
      Clock::time_point next_attempt{};
      Clock::time_point request_next{};
      Clock::time_point request_deadline{};
      uint32_t attempt = 0;  // newest attempt id; zero before the first CONNECT_INFO
      relay::TokenSecret send_key;
      relay::TokenSecret receive_key;
      std::vector<Candidate> candidates;
      std::set<uint64_t> probes;  // sequences of probes sent in the current window
      Clock::time_point probe_next{};
      Clock::time_point probe_deadline{};
      relay::Endpoint endpoint{};
      bool local = false;
      Clock::time_point established{};
      Clock::time_point last_ack{};  // last KEEPALIVE_ACK or PROBE_ACK while up
      Clock::time_point last_keepalive{};
      uint64_t sequence = 0;
      relay::ReplayWindow window;
      Clock::time_point used{};  // last Touch or CONNECT_INFO; orders eviction
    };
    // Bounds the per-peer attempt floors; see Accepted.
    static constexpr size_t kMaxFloors = 1024;

    Impl& impl;
    DirectOptions options;
    uint32_t self;
    relay::TokenSecret assoc;
    Socket& transport;
    Socket& injection;
    std::vector<relay::Candidate> local_candidates;
    std::map<uint32_t, Peer> peers{};
    // Retain each peer's highest ID through close/eviction for the association
    // lifetime; CONNECT_INFO IDs <= it replay. Beyond kMaxFloors, fold the
    // lowest into evicted_floor, rejecting IDs <= it for every peer.
    std::map<uint32_t, uint32_t> floors{};
    uint32_t evicted_floor = 0;
    unsigned candidate_sends = 0;
    Clock::time_point next_candidates{};
    bool dropping = false;

    static std::string Address(uint32_t ipv4) {
      const auto* octets = reinterpret_cast<const uint8_t*>(&ipv4);
      return fmt::format("{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
    }
    void Count(uint64_t Stats::* field) {
      std::lock_guard lock(impl.mutex);
      ++(impl.stats.*field);
    }
    void Publish() {
      uint32_t up = 0, attempted = 0;
      for (const auto& [vip, peer] : peers) {
        up += peer.phase == Phase::kUp;
        attempted += peer.attempted;
      }
      std::lock_guard lock(impl.mutex);
      impl.stats.direct_peers = up;
      impl.stats.direct_attempted_peers = attempted;
    }
    void Backoff(Peer& peer, Clock::time_point now) {
      ++peer.failures;
      const auto index = std::min<size_t>(peer.failures, options.backoff_ms.size()) - 1;
      peer.next_attempt = now + std::chrono::milliseconds(options.backoff_ms[index]);
    }
    void Down(uint32_t vip, Peer& peer, Clock::time_point now, std::string_view reason) {
      peer.phase = Phase::kIdle;
      peer.probes.clear();
      Backoff(peer, now);
      Count(&Stats::direct_fallbacks);
      REXKRNL_INFO("System Link direct path to {} down ({}); using the relay", Address(vip),
                   reason);
    }
    // Returns the sequence sent, or zero when the frame could not be sent.
    uint64_t SendFrame(Peer& peer, relay::DirectFrame frame, relay::Endpoint to) {
      frame.attempt_id = peer.attempt;
      frame.sender_ipv4 = self;
      frame.sequence = ++peer.sequence;
      auto bytes = relay::EncodeDirect(frame, peer.send_key.bytes());
      if (!bytes || !transport.Send(to, *bytes)) {
        Count(&Stats::socket_errors);
        return 0;
      }
      Count(&Stats::direct_out);
      return frame.sequence;
    }
    uint64_t SendControl(Peer& peer, relay::DirectType type, relay::Endpoint to,
                         uint64_t echo = 0) {
      relay::DirectFrame frame;
      frame.type = type;
      frame.echo = echo;
      return SendFrame(peer, std::move(frame), to);
    }
    void CheckFault(Clock::time_point now) {
      if (dropping || !options.drop_after_seconds)
        return;
      for (const auto& [vip, peer] : peers) {
        if (peer.phase == Phase::kUp &&
            now - peer.established >= std::chrono::seconds(options.drop_after_seconds)) {
          dropping = true;
          REXKRNL_WARN("System Link direct fault injection: dropping received direct datagrams");
          return;
        }
      }
    }

    // A unicast guest datagram to or from a member starts an attempt; later
    // attempts wait for the backoff after a failure or fallback.
    void Touch(uint32_t vip, Clock::time_point now) {
      if (vip == self || !IsVirtualAddress(vip))
        return;
      auto* slot = Slot(vip, now);
      if (!slot)
        return;
      auto& peer = *slot;
      if (peer.phase != Phase::kIdle || now < peer.next_attempt)
        return;
      peer.phase = Phase::kRequested;
      peer.attempted = true;
      peer.request_next = now;
      peer.request_deadline = now + std::chrono::milliseconds(kConnectTimeoutMs);
      Publish();
    }

    // Finds or makes the entry for vip. A full table evicts its least recently
    // used entry without a live path or probing attempt, idle before requested,
    // so destinations that never answer cannot lock out a real peer.
    Peer* Slot(uint32_t vip, Clock::time_point now) {
      auto found = peers.find(vip);
      if (found == peers.end()) {
        if (peers.size() >= relay::kMaxMembers) {
          auto victim = peers.end();
          for (auto it = peers.begin(); it != peers.end(); ++it) {
            const auto& peer = it->second;
            if (peer.phase != Phase::kIdle && peer.phase != Phase::kRequested)
              continue;
            if (victim == peers.end() ||
                std::pair(peer.phase != Phase::kIdle, peer.used) <
                    std::pair(victim->second.phase != Phase::kIdle, victim->second.used))
              victim = it;
          }
          if (victim == peers.end())
            return nullptr;
          peers.erase(victim);
          Count(&Stats::direct_evicted);
        }
        found = peers.emplace(vip, Peer{}).first;
      }
      found->second.used = now;
      return &found->second;
    }
    // Adds the K_assoc tag to CANDIDATES and CONNECT_REQUEST after the token is set;
    // false drops a message that does not encode.
    bool Sign(relay::Message& message) const {
      if (message.type != relay::Type::kCandidates && message.type != relay::Type::kConnectRequest)
        return true;
      return relay::SignConnect(assoc.bytes(), message);
    }
    // Records a fresh attempt id for vip; false for a closed or superseded one.
    bool Accepted(uint32_t vip, uint32_t attempt_id) {
      if (attempt_id <= evicted_floor)
        return false;
      const auto floor = floors.find(vip);
      if (floor != floors.end() && attempt_id <= floor->second)
        return false;
      Raise(vip, attempt_id);
      return true;
    }
    // Raises vip's floor to at least attempt_id.
    void Raise(uint32_t vip, uint32_t attempt_id) {
      auto [floor, inserted] = floors.try_emplace(vip, attempt_id);
      if (!inserted) {
        floor->second = std::max(floor->second, attempt_id);
        return;
      }
      if (floors.size() > kMaxFloors) {
        auto lowest = floors.end();
        for (auto it = floors.begin(); it != floors.end(); ++it)
          if (it != floor && (lowest == floors.end() || it->second < lowest->second))
            lowest = it;
        evicted_floor = std::max(evicted_floor, lowest->second);
        floors.erase(lowest);
      }
    }
    // Probe acknowledgements count only inside the attempt's probing window.
    static bool InProbeWindow(const Peer& peer, Clock::time_point now) {
      return now < peer.probe_deadline &&
             (peer.phase == Phase::kProbing || (peer.phase == Phase::kUp && !peer.local));
    }

    void Service(Clock::time_point now, std::vector<relay::Message>& out) {
      if (candidate_sends < kCandidateSends && now >= next_candidates) {
        relay::Message candidates;
        candidates.type = relay::Type::kCandidates;
        candidates.candidates = local_candidates;
        out.push_back(std::move(candidates));
        ++candidate_sends;
        next_candidates = now + std::chrono::milliseconds(kCandidateIntervalMs);
      }
      CheckFault(now);
      for (auto& [vip, peer] : peers) {
        if (peer.phase == Phase::kRequested) {
          if (now >= peer.request_deadline) {
            peer.phase = Phase::kIdle;
            Backoff(peer, now);
            Count(&Stats::direct_connect_timeouts);
          } else if (now >= peer.request_next) {
            relay::Message request;
            request.type = relay::Type::kConnectRequest;
            request.ipv4 = vip;
            out.push_back(std::move(request));
            peer.request_next = now + std::chrono::milliseconds(kConnectRetryMs);
          }
          continue;
        }
        if (peer.phase == Phase::kProbing && now >= peer.probe_deadline) {
          peer.phase = Phase::kIdle;
          peer.probes.clear();
          Backoff(peer, now);
          Count(&Stats::direct_probe_failures);
          continue;
        }
        // An established observed path keeps probing local candidates until the window ends.
        const bool probing = InProbeWindow(peer, now);
        if (probing && now >= peer.probe_next) {
          for (size_t i = 0; i < peer.candidates.size(); ++i) {
            if (peer.phase == Phase::kUp && !peer.candidates[i].local)
              continue;
            if (auto sequence =
                    SendControl(peer, relay::DirectType::kProbe, peer.candidates[i].endpoint))
              peer.probes.insert(sequence);
          }
          peer.probe_next = now + std::chrono::milliseconds(kDirectProbeIntervalMs);
        }
        if (peer.phase != Phase::kUp)
          continue;
        if (!probing) {
          peer.probes.clear();
          peer.probe_next = peer.probe_deadline;  // Next() then stops waking for probes
        }

        // Only acknowledgements prove this side's sends arrive; received data does
        // not, so a path that loses only this side's sends still goes down.
        if (now - peer.last_ack >= std::chrono::milliseconds(kDirectAckTimeoutMs)) {
          Down(vip, peer, now, "no acknowledgement for 2 seconds");
          continue;
        }
        if (now >= NextKeepalive(peer)) {
          SendControl(peer, relay::DirectType::kKeepalive, peer.endpoint);
          peer.last_keepalive = now;
        }
      }
      Publish();
    }
    static Clock::time_point NextKeepalive(const Peer& peer) {
      return peer.last_keepalive + std::chrono::milliseconds(kDirectKeepaliveMs);
    }
    Clock::time_point Next(Clock::time_point deadline) const {
      if (candidate_sends < kCandidateSends)
        deadline = std::min(deadline, next_candidates);
      for (const auto& [vip, peer] : peers) {
        switch (peer.phase) {
          case Phase::kIdle:
            break;
          case Phase::kRequested:
            deadline = std::min({deadline, peer.request_next, peer.request_deadline});
            break;
          case Phase::kProbing:
            deadline = std::min({deadline, peer.probe_next, peer.probe_deadline});
            break;
          case Phase::kUp:
            if (!peer.local && peer.probe_next < peer.probe_deadline)
              deadline = std::min(deadline, peer.probe_next);
            deadline = std::min({deadline, NextKeepalive(peer),
                                 peer.last_ack + std::chrono::milliseconds(kDirectAckTimeoutMs)});
            if (options.drop_after_seconds && !dropping)
              deadline = std::min(
                  deadline, peer.established + std::chrono::seconds(options.drop_after_seconds));
            break;
        }
      }
      return deadline;
    }

    void ConnectInfo(const relay::Message& message, Clock::time_point now) {
      if (!IsVirtualAddress(message.ipv4) || message.ipv4 == self) {
        Count(&Stats::direct_malformed);
        return;
      }
      auto pair_key = relay::UnwrapPairKey(assoc.bytes(), message);
      if (!pair_key) {
        Count(&Stats::direct_bad_tag);
        return;
      }
      // Only the newest attempt counts. A repeat of the current attempt changes
      // nothing: probing it again would refresh the peer's view of a path that
      // already failed. A request answered that way times out into backoff.
      const auto current = peers.find(message.ipv4);
      if (current != peers.end() && current->second.attempt == message.attempt_id)
        return;
      if (!Accepted(message.ipv4, message.attempt_id)) {
        Count(&Stats::direct_stale_attempt);
        return;
      }
      auto* slot = Slot(message.ipv4, now);
      if (!slot)
        return;
      auto& peer = *slot;
      if (peer.phase == Phase::kUp)
        Down(message.ipv4, peer, now, "replaced by a new attempt");
      peer.attempt = message.attempt_id;
      auto send_key = relay::DirectKey(pair_key->bytes(), self);
      auto receive_key = relay::DirectKey(pair_key->bytes(), message.ipv4);
      peer.send_key = relay::TokenSecret(send_key);
      peer.receive_key = relay::TokenSecret(receive_key);
      relay::SecureWipe(send_key);
      relay::SecureWipe(receive_key);
      peer.candidates.clear();
      for (const auto& candidate : message.candidates)
        peer.candidates.push_back({{candidate.ipv4, candidate.port}, true});
      const relay::Endpoint observed{message.observed.ipv4, message.observed.port};
      if (std::none_of(peer.candidates.begin(), peer.candidates.end(),
                       [&](const Candidate& candidate) { return candidate.endpoint == observed; }))
        peer.candidates.push_back({observed, false});
      peer.probes.clear();
      peer.sequence = 0;
      peer.window = {};
      peer.phase = Phase::kProbing;
      peer.attempted = true;
      peer.probe_next = now;
      peer.probe_deadline = now + std::chrono::milliseconds(kDirectProbeWindowMs);
      Count(&Stats::direct_attempts);
      Publish();
    }
    // The CLOSE tag is under K_assoc, so only the relay can close an attempt. Its
    // id becomes the peer's floor even without a matching entry: a CONNECT_INFO
    // delayed past its CLOSE must not recreate the departed peer.
    void ConnectClose(const relay::Message& message, Clock::time_point now) {
      if (!relay::ConstantTimeEqual(
              relay::CloseTag(assoc.bytes(), message.attempt_id, message.ipv4), message.key_tag)) {
        Count(&Stats::direct_bad_tag);
        return;
      }
      Raise(message.ipv4, message.attempt_id);
      auto found = peers.find(message.ipv4);
      if (found == peers.end() || found->second.attempt > message.attempt_id) {
        Count(&Stats::direct_stale_attempt);
        return;
      }
      if (found->second.phase == Phase::kUp)
        Down(message.ipv4, found->second, now, "peer left the room");
      peers.erase(found);
      Publish();
    }
    static bool IsLocal(const Peer& peer, relay::Endpoint endpoint) {
      return std::any_of(peer.candidates.begin(), peer.candidates.end(),
                         [&](const Candidate& c) { return c.local && c.endpoint == endpoint; });
    }
    // An answered probe establishes the path toward the acknowledgement's source.
    void Use(uint32_t vip, Peer& peer, relay::Endpoint source, Clock::time_point now) {
      const Candidate candidate{source, IsLocal(peer, source)};
      if (peer.phase == Phase::kUp &&
          (peer.endpoint == candidate.endpoint || peer.local || !candidate.local))
        return;
      if (peer.phase != Phase::kUp) {
        peer.phase = Phase::kUp;
        peer.established = now;
        peer.last_keepalive = now;
        peer.failures = 0;
        Count(&Stats::direct_established);
      }
      peer.endpoint = candidate.endpoint;
      peer.local = candidate.local;
      REXKRNL_INFO("System Link direct path to {} up via its {} endpoint", Address(vip),
                   peer.local ? "local" : "observed");
      Publish();
    }
    // An authenticated datagram from an unlisted endpoint moves an established path there.
    void Rebind(uint32_t vip, Peer& peer, relay::Endpoint source) {
      if (peer.phase != Phase::kUp || peer.endpoint == source)
        return;
      peer.endpoint = source;
      peer.local = IsLocal(peer, source);
      REXKRNL_INFO("System Link direct path to {} moved to its {} endpoint", Address(vip),
                   peer.local ? "local" : "observed");
    }
    void Datagram(std::span<const uint8_t> bytes, relay::Endpoint source, Clock::time_point now) {
      CheckFault(now);
      if (dropping) {
        Count(&Stats::direct_fault_drops);
        return;
      }
      auto frame = relay::ParseDirect(bytes);
      if (!frame) {
        Count(&Stats::direct_malformed);
        return;
      }
      auto found = peers.find(frame->sender_ipv4);
      if (found == peers.end() || !found->second.attempt) {
        Count(&Stats::direct_unknown_peer);
        return;
      }
      const auto vip = found->first;
      auto& peer = found->second;
      if (frame->attempt_id != peer.attempt) {
        Count(&Stats::direct_stale_attempt);
        return;
      }
      if (!relay::VerifyDirect(bytes, peer.receive_key.bytes())) {
        Count(&Stats::direct_bad_mac);
        return;
      }
      if (!peer.window.Fresh(frame->sequence)) {
        Count(&Stats::direct_replay);
        return;
      }
      peer.window.Accept(frame->sequence);
      Count(&Stats::direct_in);
      switch (frame->type) {
        case relay::DirectType::kProbe:
          SendControl(peer, relay::DirectType::kProbeAck, source, frame->sequence);
          return;
        case relay::DirectType::kProbeAck: {
          if (InProbeWindow(peer, now) && peer.probes.contains(frame->echo)) {
            Use(vip, peer, source, now);
            peer.last_ack = now;
          }
          return;
        }
        case relay::DirectType::kKeepalive:
          Rebind(vip, peer, source);
          SendControl(peer, relay::DirectType::kKeepaliveAck, source, frame->sequence);
          return;
        case relay::DirectType::kKeepaliveAck:
          Rebind(vip, peer, source);
          if (peer.phase == Phase::kUp)
            peer.last_ack = now;
          return;
        case relay::DirectType::kData:
          Rebind(vip, peer, source);
          Inject(*frame);
          return;
      }
    }
    // Delivers exactly as a DELIVER: same envelope, virtual source restored.
    void Inject(const relay::DirectFrame& frame) {
      if (frame.destination_port == kProbePort) {
        Count(&Stats::direct_malformed);
        return;
      }
      relay::Bytes envelope;
      uint16_t target = 0;
      {
        std::lock_guard lock(impl.mutex);
        target = impl.ports.LoopbackPort(frame.destination_port);
        if (!target) {
          ++impl.stats.unknown_port;
          return;
        }
        envelope = Wrap(impl.envelope_token, {frame.sender_ipv4, frame.source_port}, frame.payload);
      }
      const bool sent = injection.Send({htonl(INADDR_LOOPBACK), target}, envelope);
      Count(sent ? &Stats::delivered : &Stats::socket_errors);
    }
    // Sends guest unicast on an established path; false leaves it to the relay.
    bool SendData(const relay::Message& message) {
      if (message.ipv4 == 0xffffffffu || message.destination_port == kProbePort)
        return false;
      auto found = peers.find(message.ipv4);
      if (found == peers.end() || found->second.phase != Phase::kUp)
        return false;
      auto& peer = found->second;
      relay::DirectFrame frame;
      frame.type = relay::DirectType::kData;
      frame.source_port = message.source_port;
      frame.destination_port = message.destination_port;
      frame.payload = message.payload;
      return SendFrame(peer, std::move(frame), peer.endpoint) != 0;
    }
    void Close() {
      for (const auto& [vip, peer] : peers)
        if (peer.phase == Phase::kUp)
          REXKRNL_INFO("System Link direct path to {} closed with the relay association",
                       Address(vip));
      peers.clear();
      Publish();
    }
  };

  void WakeLocked() {
    if (wake) {
      const std::array<uint8_t, 1> signal{0};
      wake->Send(wake->endpoint, signal);
    }
  }

  // Zero is not a valid update sequence, so the counter skips it on wrap.
  uint32_t UpdateSequenceLocked() {
    do
      ++next_update;
    while (!next_update);
    return next_update;
  }
  uint32_t WireIdLocked() {
    do
      ++next_wire_id;
    while (!next_wire_id);
    return next_wire_id;
  }
  void FailRequestLocked(Request& request, RequestFailure failure) {
    request.state = RequestState::kFailed;
    request.failure = failure;
    request.pages.clear();
    request.records.clear();
    ++stats.requests_failed;
  }
  // Association loss forgets queued updates; the relay removes the peer's sessions.
  void FailDirectoryLocked() {
    updates.clear();
    for (auto& [handle, request] : requests)
      if (request.state == RequestState::kPending)
        FailRequestLocked(request, RequestFailure::kAssociation);
    for (auto& [handle, set] : probes)
      if (set.state == RequestState::kPending) {
        set.state = RequestState::kFailed;
        set.failure = RequestFailure::kAssociation;
      }
  }
  // A set is done once every probe it will send has been answered or has timed out.
  static void FinishProbeLocked(ProbeSet& set, Clock::time_point now) {
    if (set.state != RequestState::kPending || set.sent_at.size() < set.count)
      return;
    const bool answered =
        std::all_of(set.answered.begin(), set.answered.end(), [](bool done) { return done; });
    if (set.sent_at.empty() || answered ||
        now >= set.sent_at.back() + std::chrono::milliseconds(kProbeTimeoutMs))
      set.state = RequestState::kDone;
  }
  void DueProbesLocked(Clock::time_point now, std::vector<relay::Message>& due) {
    for (auto& [handle, set] : probes) {
      if (set.state != RequestState::kPending)
        continue;
      if (set.sent_at.size() < set.count &&
          now - set.begin >= std::chrono::milliseconds(kProbeSendWindowMs))
        set.count = static_cast<unsigned>(set.sent_at.size());
      while (set.sent_at.size() < set.count && probe_budget.Take(now)) {
        relay::Message probe;
        probe.type = relay::Type::kData;
        probe.ipv4 = set.ipv4;
        probe.source_port = kProbePort;
        probe.destination_port = kProbePort;
        probe.payload =
            ProbePayload(kProbeRequest, static_cast<uint8_t>(set.sent_at.size()), set.id);
        due.push_back(std::move(probe));
        set.sent_at.push_back(now);
        set.answered.push_back(false);
        ++stats.probes_sent;
      }
      FinishProbeLocked(set, now);
    }
  }
  // Answers probe requests and matches replies; nothing here reaches a guest socket.
  void ProbeLocked(const relay::Message& message, Clock::time_point now) {
    const auto probe = ParseProbe(message.payload);
    if (!probe || message.source_port != kProbePort || message.broadcast) {
      ++stats.invalid_probes;
      return;
    }
    if (probe->kind == kProbeRequest) {
      if (!probe_replies || queue.size() >= kMaxQueued || !reply_budget.Take(now)) {
        ++stats.probe_replies_dropped;
        return;
      }
      relay::Message reply;
      reply.type = relay::Type::kData;
      reply.ipv4 = message.ipv4;
      reply.source_port = kProbePort;
      reply.destination_port = kProbePort;
      reply.payload = ProbePayload(kProbeReply, probe->sequence, probe->id);
      queue.push_back(std::move(reply));
      ++stats.probe_replies_sent;
      return;
    }
    for (auto& [handle, set] : probes) {
      if (set.state != RequestState::kPending || set.id != probe->id || set.ipv4 != message.ipv4 ||
          probe->sequence >= set.sent_at.size() || set.answered[probe->sequence])
        continue;
      set.answered[probe->sequence] = true;
      set.rtt_ms.push_back(static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now - set.sent_at[probe->sequence])
              .count()));
      ++stats.probe_replies_received;
      FinishProbeLocked(set, now);
      return;
    }
    ++stats.invalid_probes;
  }
  std::vector<relay::Message> DueLocked(Clock::time_point now) {
    std::vector<relay::Message> due;
    while (!updates.empty() && updates.front().next <= now) {
      auto& update = updates.front();
      if (update.attempts == kMaxUpdateAttempts) {
        ++stats.directory_failed;
        updates.pop_front();
        continue;
      }
      ++update.attempts;
      update.next = now + std::chrono::milliseconds(kDirectoryRetryMs);
      due.push_back(update.message);
      break;
    }
    for (auto& [handle, request] : requests) {
      if (request.state != RequestState::kPending)
        continue;
      if (now >= request.deadline) {
        FailRequestLocked(request, RequestFailure::kTimeout);
        continue;
      }
      if (request.next_send > now)
        continue;
      request.next_send = now + std::chrono::milliseconds(kDirectoryRetryMs);
      due.push_back(request.message);
    }
    DueProbesLocked(now, due);
    return due;
  }
  Clock::time_point NextDirectoryLocked(Clock::time_point deadline) const {
    if (!updates.empty())
      deadline = std::min(deadline, updates.front().next);
    for (const auto& [handle, request] : requests)
      if (request.state == RequestState::kPending)
        deadline = std::min({deadline, request.next_send, request.deadline});
    for (const auto& [handle, set] : probes) {
      if (set.state != RequestState::kPending)
        continue;
      // Probes waiting for budget retry shortly; sent probes wait for their timeout.
      deadline =
          std::min(deadline, set.sent_at.size() < set.count
                                 ? Clock::now() + 25ms
                                 : set.sent_at.back() + std::chrono::milliseconds(kProbeTimeoutMs));
    }
    return deadline;
  }
  Request* PendingLocked(uint32_t wire_id) {
    for (auto& [handle, request] : requests)
      if (request.state == RequestState::kPending && request.message.request_id == wire_id)
        return &request;
    return nullptr;
  }
  // Returns false for messages that are not directory replies.
  bool DirectoryReplyLocked(relay::Message& message) {
    if (message.type == relay::Type::kStatus) {
      if (!updates.empty() && updates.front().attempts &&
          updates.front().message.request_id == message.request_id) {
        if (message.status != relay::Status::kOk)
          ++stats.directory_failed;
        updates.pop_front();
      } else if (auto* request = PendingLocked(message.request_id)) {
        FailRequestLocked(*request, RequestFailure::kRejected);
      }
      return true;
    }
    if (message.type == relay::Type::kResults) {
      auto* request = PendingLocked(message.request_id);
      if (!request || request->message.type == relay::Type::kTokenRequest)
        return true;
      if (!request->pages.empty() && message.snapshot != request->snapshot) {
        if (static_cast<int32_t>(message.snapshot - request->snapshot) < 0)
          return true;
        request->pages.clear();
        request->page_count = 0;
      }
      if (request->page_count && request->page_count != message.page_count)
        return true;
      request->snapshot = message.snapshot;
      request->page_count = message.page_count;
      request->pages.insert_or_assign(message.page, std::move(message.records));
      if (request->pages.size() == request->page_count) {
        for (auto& [page, records] : request->pages)
          for (auto& record : records)
            request->records.push_back(std::move(record));
        request->pages.clear();
        const size_t limit =
            request->message.type == relay::Type::kLookup ? 1 : request->message.max_results;
        if (request->records.size() > limit)
          FailRequestLocked(*request, RequestFailure::kRejected);
        else
          request->state = RequestState::kDone;
      }
      return true;
    }
    if (message.type == relay::Type::kTokenGrant) {
      auto* request = PendingLocked(message.request_id);
      if (request && request->message.type == relay::Type::kTokenRequest) {
        request->token.emplace(JoinToken{
            message.join_token,
            Secret(std::string_view(reinterpret_cast<const char*>(message.token_secret.data()),
                                    message.token_secret.size())),
            message.expiry_seconds});
        request->state = RequestState::kDone;
      }
      return true;
    }
    return false;
  }

  void ResetLocked() {
    FailDirectoryLocked();
    next_update = 0;
    queue.clear();
    ipv4 = 0;
    envelope_token = 0;
    injector = {};
    stats = {};
    failure = {};
    last_destination_log = {};
  }

  void Fail(std::string reason) {
    std::lock_guard lock(mutex);
    stats.last_failure = reason;
    failure = Error{ErrorCategory::IO, std::move(reason)};
    stats.state = Association::kFailed;
    changed.notify_all();
  }
  void Run(relay::Endpoint server, std::string room, std::optional<relay::TokenId> join_token,
           Secret secret, relay::Digest fingerprint) {
    Winsock winsock;
    Socket transport, injection, signal;
    // A loopback relay keeps every native socket on loopback, so no listener is exposed.
    const bool local = (ntohl(server.ipv4) >> 24) == 127;
    if (!winsock.ready || !transport.Open(local ? htonl(INADDR_LOOPBACK) : 0) ||
        !injection.Open(htonl(INADDR_LOOPBACK)) || !signal.Open(htonl(INADDR_LOOPBACK))) {
      Fail("Cannot open relay UDP sockets");
      return;
    }
    auto random = relay::RandomSeed();
    if (!random) {
      Fail(random.error().message);
      return;
    }
    relay::Message hello;
    if (join_token) {
      hello.token_join = true;
      hello.join_token = *join_token;
    } else {
      hello.room = std::move(room);
    }
    hello.fingerprint = fingerprint;
    std::copy_n(random->begin(), hello.nonce.size(), hello.nonce.begin());
    {
      std::lock_guard lock(mutex);
      std::memcpy(&envelope_token, random->data() + 16, sizeof(envelope_token));
      injector = injection.endpoint;
      wake = &signal;
    }
    auto transmit = [&](const relay::Message& message) {
      auto bytes = relay::Encode(message);
      if (bytes && transport.Send(server, *bytes))
        return;
      std::lock_guard lock(mutex);
      ++stats.socket_errors;
    };
    uint64_t session_token = 0;
    auto started = Clock::now();
    auto next_handshake = started;
    auto next_heartbeat = started;
    auto last_received = started;
    bool joined = false;
    bool challenged = false;
    relay::Message join;
    join.type = relay::Type::kJoin;
    unsigned join_attempts = 0;
    relay::Nonce server_nonce{};
    std::optional<DirectLink> direct;
    // Full-size receive prevents oversized UDP from becoming a valid prefix.
    std::array<uint8_t, 65536> bytes{};
    for (;;) {
      {
        std::lock_guard lock(mutex);
        if (stop)
          break;
      }
      auto now = Clock::now();
      if (!joined) {
        if (now - started >= 5s) {
          Fail("relay not reachable (handshake timeout)");
          break;
        }
        if (now >= next_handshake) {
          if (!challenged)
            transmit(hello);
          else if (join_attempts < 3) {
            transmit(join);
            ++join_attempts;
          }
          next_handshake = challenged && join_attempts == 3 ? started + 5s : now + 1500ms;
          if (!challenged)
            next_handshake = now + 500ms;
        }
      } else {
        if (now - last_received >= 10s) {
          std::lock_guard lock(mutex);
          stats.state = Association::kLost;
          stats.discarded_pending += queue.size();
          queue.clear();
          FailDirectoryLocked();
          REXKRNL_ERROR("System Link relay association lost: no relay datagram for 10 seconds");
          break;
        }
        if (now >= next_heartbeat) {
          relay::Message heartbeat;
          heartbeat.type = relay::Type::kHeartbeat;
          heartbeat.token = session_token;
          transmit(heartbeat);
          std::lock_guard lock(mutex);
          ++stats.heartbeats;
          next_heartbeat = now + 2s;
        }
        // Limit each drain so receive and heartbeat deadlines are serviced under load.
        for (size_t i = 0; i < 32; ++i) {
          relay::Message outgoing;
          {
            std::lock_guard lock(mutex);
            if (queue.empty())
              break;
            outgoing = std::move(queue.front());
            queue.pop_front();
          }
          outgoing.token = session_token;
          if (direct && outgoing.type == relay::Type::kData) {
            if (outgoing.ipv4 != 0xffffffffu && outgoing.destination_port != kProbePort)
              direct->Touch(outgoing.ipv4, now);
            if (direct->SendData(outgoing))
              continue;
          }
          transmit(outgoing);
        }
        std::vector<relay::Message> due;
        {
          std::lock_guard lock(mutex);
          due = DueLocked(now);
        }
        if (direct)
          direct->Service(now, due);
        for (auto& outgoing : due) {
          outgoing.token = session_token;
          if (direct && !direct->Sign(outgoing))
            continue;
          transmit(outgoing);
        }
      }
      auto deadline = joined ? std::min(next_heartbeat, last_received + 10s)
                             : std::min(next_handshake, started + 5s);
      if (direct)
        deadline = direct->Next(deadline);
      {
        std::lock_guard lock(mutex);
        if (stop || !queue.empty())
          deadline = Clock::now();
        else if (joined)
          deadline = NextDirectoryLocked(deadline);
      }
      auto wait = std::max(
          0us, std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()));
      timeval timeout{static_cast<decltype(timeval::tv_sec)>(wait.count() / 1000000),
                      static_cast<decltype(timeval::tv_usec)>(wait.count() % 1000000)};
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(transport.handle, &readable);
      FD_SET(signal.handle, &readable);
      const auto ready = select(static_cast<int>(std::max(transport.handle, signal.handle) + 1),
                                &readable, nullptr, nullptr, &timeout);
      if (ready < 0) {
        Fail("Relay socket select failed");
        break;
      }
      if (FD_ISSET(signal.handle, &readable))
        recvfrom(signal.handle, reinterpret_cast<char*>(bytes.data()),
                 static_cast<int>(bytes.size()), 0, nullptr, nullptr);
      if (!ready || !FD_ISSET(transport.handle, &readable))
        continue;
      sockaddr_in source{};
      AddressSize source_size = sizeof(source);
      auto count = recvfrom(transport.handle, reinterpret_cast<char*>(bytes.data()),
                            static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&source),
                            &source_size);
      if (count < 0) {
        const auto error = SocketError();
        std::lock_guard lock(mutex);
#if defined(_WIN32)
        if (error == WSAECONNRESET) {
          ++stats.connection_resets;
          continue;
        }
#endif
        if (error != kWouldBlock)
          ++stats.socket_errors;
        continue;
      }
      const relay::Endpoint from{source.sin_addr.s_addr, ntohs(source.sin_port)};
      if (from != server) {
        if (direct) {
          direct->Datagram({bytes.data(), static_cast<size_t>(count)}, from, Clock::now());
          continue;
        }
        std::lock_guard lock(mutex);
        ++stats.invalid_wire;
        continue;
      }
      last_received = Clock::now();
      const std::span<uint8_t> received{bytes.data(), static_cast<size_t>(count)};
      auto message = relay::Decode(received);
      if (message && message->type == relay::Type::kTokenGrant && !joined) {
        relay::SecureWipe(received);
        continue;
      }
      if (!message) {
        std::lock_guard lock(mutex);
        ++stats.invalid_wire;
        continue;
      }
      if (!joined) {
        if (message->type == relay::Type::kReject) {
          Fail(Rejection(message->reason));
          break;
        }
        if (!challenged && message->type == relay::Type::kChallenge) {
          const auto view = secret.view();
          join.auth =
              hello.token_join
                  ? relay::TokenJoinAuth(
                        {reinterpret_cast<const uint8_t*>(view.data()), view.size()}, hello.nonce,
                        message->nonce, hello.join_token, fingerprint)
                  : relay::JoinAuth(view, hello.nonce, message->nonce, hello.room, fingerprint);
          // The secret stays in its wiped storage until WELCOME derives K_assoc.
          server_nonce = message->nonce;
          transmit(join);
          challenged = true;
          join_attempts = 1;
          next_handshake = Clock::now() + 1500ms;
        } else if (challenged && message->type == relay::Type::kWelcome && message->token &&
                   message->peer_id && IsVirtualAddress(message->ipv4)) {
          session_token = message->token;
          joined = true;
          next_heartbeat = Clock::now() + 2s;
          const auto view = secret.view();
          auto assoc =
              relay::AssociationKey({reinterpret_cast<const uint8_t*>(view.data()), view.size()},
                                    hello.nonce, server_nonce, message->peer_id);
          secret.Clear();
          DirectOptions options;
          {
            std::lock_guard lock(mutex);
            options = direct_options;
          }
          if (options.enabled)
            direct.emplace(DirectLink{*this, options, message->ipv4, relay::TokenSecret(assoc),
                                      transport, injection,
                                      LocalCandidates(server, transport.endpoint.port)});
          relay::SecureWipe(assoc);
          std::lock_guard lock(mutex);
          ipv4 = message->ipv4;
          stats.ipv4 = ipv4;
          stats.state = Association::kLive;
          changed.notify_all();
        }
        continue;
      }
      if (message->type == relay::Type::kAck && message->token == session_token)
        continue;
      if (direct && message->type == relay::Type::kConnectInfo) {
        direct->ConnectInfo(*message, Clock::now());
        continue;
      }
      if (direct && message->type == relay::Type::kConnectClose) {
        direct->ConnectClose(*message, Clock::now());
        continue;
      }
      if (message->type == relay::Type::kDeliver && message->destination_port == kProbePort &&
          message->source_port == kProbePort && !message->broadcast &&
          IsVirtualAddress(message->ipv4) && IsSideChannel(message->payload)) {
        SideChannelHandler handler;
        {
          std::lock_guard lock(mutex);
          handler = side_channel;
          ++stats.side_channel_in;
        }
        // Outside the lock: the handler may queue a reply through SendSideChannel.
        if (handler)
          handler(message->ipv4, message->payload);
        continue;
      }
      uint32_t touched = 0;
      {
        std::lock_guard lock(mutex);
        if (DirectoryReplyLocked(*message)) {
          // The grant's secret now lives only in the request's wiped Secret storage.
          if (message->type == relay::Type::kTokenGrant)
            relay::SecureWipe(received);
          continue;
        }
        if (message->type != relay::Type::kDeliver || !IsVirtualAddress(message->ipv4)) {
          ++stats.invalid_wire;
          continue;
        }
        if (message->destination_port == kProbePort) {
          ProbeLocked(*message, Clock::now());
          continue;
        }
        if (!message->broadcast)
          touched = message->ipv4;
        auto target = ports.LoopbackPort(message->destination_port);
        if (!target) {
          ++stats.unknown_port;
        } else {
          auto envelope =
              Wrap(envelope_token, {message->ipv4, message->source_port}, message->payload);
          if (injection.Send({htonl(INADDR_LOOPBACK), target}, envelope))
            ++stats.delivered;
          else
            ++stats.socket_errors;
        }
      }
      if (direct && touched)
        direct->Touch(touched, Clock::now());
    }
    if (direct)
      direct->Close();
    if (joined) {
      relay::Message bye;
      bye.type = relay::Type::kBye;
      bye.token = session_token;
      transmit(bye);
    }
    relay::SecureWipe(bytes);
    std::lock_guard lock(mutex);
    FailDirectoryLocked();
    wake = nullptr;
  }
};

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() {
  Stop();
}
VoidResult Client::Start(relay::Endpoint server, std::string room, Secret secret,
                         relay::Digest fingerprint) {
  return Associate(server, std::move(room), std::nullopt, std::move(secret), fingerprint);
}
VoidResult Client::Start(relay::Endpoint server, JoinToken token, relay::Digest fingerprint) {
  return Associate(server, {}, token.id, std::move(token.secret), fingerprint);
}
VoidResult Client::Associate(relay::Endpoint server, std::string room,
                             std::optional<relay::TokenId> join_token, Secret secret,
                             relay::Digest fingerprint) {
  std::lock_guard lifecycle(impl_->lifecycle);
  std::unique_lock lock(impl_->mutex);
  if (impl_->stats.state == Association::kLive)
    return Ok();
  if (impl_->worker.joinable()) {
    impl_->stop = true;
    impl_->WakeLocked();
    lock.unlock();
    impl_->worker.join();
    lock.lock();
    impl_->ResetLocked();
  }
  const bool credentials = join_token
                               ? secret.view().size() == relay::Digest{}.size()
                               : !room.empty() && room.size() <= 32 && !secret.view().empty();
  if (!server.ipv4 || !server.port || !credentials) {
    impl_->stats.started = true;
    impl_->stats.state = Association::kFailed;
    impl_->stats.last_failure = "bad relay settings";
    return std::unexpected(
        Error{ErrorCategory::Config, "Relay requires host:port, a 1-32 byte room, and a secret"});
  }
  impl_->stop = false;
  impl_->server = server;
  impl_->stats.started = true;
  impl_->stats.last_failure.clear();
  impl_->stats.state = Association::kJoining;
  impl_->worker = std::thread([this, server, room = std::move(room), join_token,
                               secret = std::move(secret), fingerprint]() mutable {
    impl_->Run(server, std::move(room), join_token, std::move(secret), fingerprint);
  });
  impl_->changed.wait(lock, [&] { return impl_->stats.state != Association::kJoining; });
  if (impl_->stats.state != Association::kLive)
    return std::unexpected(impl_->failure);
  return Ok();
}
Stats Client::Stop() {
  std::lock_guard lifecycle(impl_->lifecycle);
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stop = true;
    impl_->WakeLocked();
  }
  if (impl_->worker.joinable())
    impl_->worker.join();
  std::lock_guard lock(impl_->mutex);
  impl_->stats.discarded_pending += impl_->queue.size();
  const auto final_stats = impl_->stats;
  impl_->ports.Clear();
  impl_->ResetLocked();
  impl_->stats.started = final_stats.started;
  impl_->stats.last_failure = final_stats.last_failure;
  return final_stats;
}
void Client::MarkFailed(std::string reason) {
  std::lock_guard lifecycle(impl_->lifecycle);
  std::lock_guard lock(impl_->mutex);
  impl_->stats.started = true;
  impl_->stats.state = Association::kFailed;
  impl_->stats.last_failure = std::move(reason);
}
uint32_t Client::ipv4() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->ipv4;
}
relay::Endpoint Client::server() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->server;
}
Stats Client::stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}
void Client::RegisterSocket(uint32_t handle, bool datagram) {
  std::lock_guard lock(impl_->mutex);
  if (datagram)
    impl_->datagram_handles.insert(handle);
  else
    impl_->datagram_handles.erase(handle);
}
bool Client::IsDatagramSocket(uint32_t handle) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->datagram_handles.contains(handle);
}
void Client::ForgetSocket(uint32_t handle) {
  std::lock_guard lock(impl_->mutex);
  impl_->datagram_handles.erase(handle);
}
std::expected<void, int> Client::Bind(uint64_t socket, uint16_t port) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->stats.state != Association::kLive)
    return std::unexpected(kNetworkDown);
  // Reserve before native bind so a collision leaves the socket unbound for retry.
  auto reserved = impl_->ports.Bind(socket, port, 1);
  if (!reserved)
    return std::unexpected(reserved.error());
  auto address = Address({htonl(INADDR_LOOPBACK), 0});
  auto native = static_cast<Native>(socket);
  if (::bind(native, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
    impl_->ports.Release(socket);
    return std::unexpected(SocketError());
  }
  AddressSize size = sizeof(address);
  if (getsockname(native, reinterpret_cast<sockaddr*>(&address), &size)) {
    impl_->ports.Release(socket);
    return std::unexpected(SocketError());
  }
  impl_->ports.Release(socket);
  auto assigned = impl_->ports.Bind(socket, *reserved, ntohs(address.sin_port));
  if (!assigned)
    return std::unexpected(assigned.error());
  return {};
}
void Client::Release(uint64_t socket) {
  std::lock_guard lock(impl_->mutex);
  impl_->ports.Release(socket);
}
uint16_t Client::GuestPort(uint64_t socket) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->ports.GuestPort(socket);
}
std::expected<int, int> Client::Send(uint64_t socket, relay::Endpoint destination,
                                     std::span<const uint8_t> payload) {
  {
    // After the final XNetCleanup, sockets the guest still holds behave like a
    // network that drops datagrams; titles send their GT2 close after cleanup.
    std::lock_guard lock(impl_->mutex);
    if (impl_->stats.state == Association::kStopped) {
      ++impl_->stats.dropped_after_cleanup;
      return static_cast<int>(payload.size());
    }
  }
  if (!GuestPort(socket)) {
    auto bound = Bind(socket, 0);
    if (!bound && !GuestPort(socket))
      return std::unexpected(bound.error());
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->stats.state != Association::kLive)
    return std::unexpected(kNetworkDown);
  if (!CanRoute(destination.ipv4)) {
    ++impl_->stats.rejected_destination;
    const auto now = Clock::now();
    if (impl_->stats.rejected_destination == 1 || now - impl_->last_destination_log >= 5s) {
      REXKRNL_DEBUG("System Link relay rejected destinations={}",
                    impl_->stats.rejected_destination);
      impl_->last_destination_log = now;
    }
    return std::unexpected(kNetworkUnreachable);
  }
  if (payload.size() > relay::kMaxPayload) {
    ++impl_->stats.oversize;
    return std::unexpected(kMessageSize);
  }
  if (impl_->queue.size() == kMaxQueued) {
    ++impl_->stats.queue_full;
    return std::unexpected(kWouldBlock);
  }
  relay::Message message;
  message.type = relay::Type::kData;
  message.source_port = impl_->ports.GuestPort(socket);
  message.destination_port = destination.port;
  message.ipv4 = destination.ipv4;
  message.payload.assign(payload.begin(), payload.end());
  impl_->queue.push_back(std::move(message));
  impl_->WakeLocked();
  return static_cast<int>(payload.size());
}
std::expected<int, int> Client::Receive(uint64_t socket, std::span<uint8_t> buffer, int flags,
                                        relay::Endpoint& source) {
  std::array<uint8_t, 65536> bytes{};
  for (;;) {
    relay::Endpoint injector;
    uint64_t token;
    {
      std::lock_guard lock(impl_->mutex);
      if (!impl_->ports.GuestPort(socket))
        return std::unexpected(10022);
      injector = impl_->injector;
      token = impl_->envelope_token;
    }
    sockaddr_in from{};
    AddressSize size = sizeof(from);
    int count =
        recvfrom(static_cast<Native>(socket), reinterpret_cast<char*>(bytes.data()),
                 static_cast<int>(bytes.size()), flags, reinterpret_cast<sockaddr*>(&from), &size);
    if (count < 0)
      return std::unexpected(SocketError());
    auto packet = Unwrap(token, {bytes.data(), static_cast<size_t>(count)});
    if (relay::Endpoint{from.sin_addr.s_addr, ntohs(from.sin_port)} != injector || !packet) {
      // Invalid peeked packets must be consumed or this loop would see them forever.
      if (flags & MSG_PEEK)
        recvfrom(static_cast<Native>(socket), reinterpret_cast<char*>(bytes.data()),
                 static_cast<int>(bytes.size()), flags & ~MSG_PEEK, nullptr, nullptr);
      std::lock_guard lock(impl_->mutex);
      ++impl_->stats.invalid_envelope;
      // Native recvfrom waits again on blocking sockets; nonblocking sockets
      // return would-block once all invalid datagrams have been consumed.
      continue;
    }
    auto copied = std::min(buffer.size(), packet->bytes.size());
    std::copy_n(packet->bytes.begin(), copied, buffer.begin());
#if defined(_WIN32)
    if (copied < packet->bytes.size())
      return std::unexpected(kMessageSize);
#else
    if (flags & MSG_TRUNC) {
      source = packet->endpoint;
      return static_cast<int>(packet->bytes.size());
    }
#endif
    source = packet->endpoint;
    return static_cast<int>(copied);
  }
}
bool Client::PublishSession(const relay::SessionRecord& record) {
  relay::Message message;
  message.type = relay::Type::kPublish;
  message.record = record;
  message.record.host_peer_id = 0;
  message.record.host_ipv4 = 0;
  if (!relay::EncodeRecord(message.record))
    return false;
  std::lock_guard lock(impl_->mutex);
  if (impl_->stats.state != Association::kLive || impl_->updates.size() >= kMaxUpdates)
    return false;
  message.request_id = impl_->WireIdLocked();
  message.sequence = impl_->UpdateSequenceLocked();
  impl_->updates.push_back({std::move(message)});
  impl_->WakeLocked();
  return true;
}
bool Client::RemoveSession(uint64_t session_id) {
  if (!session_id)
    return false;
  relay::Message message;
  message.type = relay::Type::kRemove;
  message.session_id = session_id;
  std::lock_guard lock(impl_->mutex);
  if (impl_->stats.state != Association::kLive || impl_->updates.size() >= kMaxUpdates)
    return false;
  message.request_id = impl_->WireIdLocked();
  message.sequence = impl_->UpdateSequenceLocked();
  impl_->updates.push_back({std::move(message)});
  impl_->WakeLocked();
  return true;
}
RequestHandle Client::BeginRequest(relay::Message message) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->requests.size() >= kMaxRequests)
    return 0;
  do
    ++impl_->next_handle;
  while (!impl_->next_handle || impl_->requests.contains(impl_->next_handle));
  auto& request = impl_->requests[impl_->next_handle];
  request.message = std::move(message);
  request.message.request_id = impl_->WireIdLocked();
  request.deadline = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
  if (impl_->stats.state != Association::kLive || !relay::Encode(request.message))
    impl_->FailRequestLocked(request, impl_->stats.state != Association::kLive
                                          ? RequestFailure::kAssociation
                                          : RequestFailure::kInvalid);
  else
    impl_->WakeLocked();
  return impl_->next_handle;
}
RequestHandle Client::BeginSearch(uint16_t max_results, uint32_t required_flags) {
  relay::Message message;
  message.type = relay::Type::kSearch;
  message.max_results = max_results;
  message.required_flags = required_flags;
  return BeginRequest(std::move(message));
}
RequestHandle Client::BeginLookup(uint64_t session_id) {
  relay::Message message;
  message.type = relay::Type::kLookup;
  message.session_id = session_id;
  return BeginRequest(std::move(message));
}
RequestHandle Client::BeginTokenRequest(uint64_t session_id) {
  relay::Message message;
  message.type = relay::Type::kTokenRequest;
  message.session_id = session_id;
  return BeginRequest(std::move(message));
}
RequestHandle Client::BeginProbe(uint32_t ipv4, unsigned count) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->probes.size() >= kMaxProbeSets)
    return 0;
  do
    ++impl_->next_probe_handle;
  while (!impl_->next_probe_handle || impl_->probes.contains(impl_->next_probe_handle));
  auto& set = impl_->probes[impl_->next_probe_handle];
  set.ipv4 = ipv4;
  set.count = std::clamp(count, 1u, kMaxProbesPerSet);
  set.begin = Clock::now();
  do
    ++impl_->next_probe_id;
  while (!impl_->next_probe_id);
  set.id = impl_->next_probe_id;
  if (impl_->stats.state != Association::kLive || !IsVirtualAddress(ipv4)) {
    set.state = RequestState::kFailed;
    set.failure = impl_->stats.state != Association::kLive ? RequestFailure::kAssociation
                                                           : RequestFailure::kInvalid;
  } else {
    impl_->WakeLocked();
  }
  return impl_->next_probe_handle;
}
ProbePoll Client::PollProbe(RequestHandle handle) {
  std::lock_guard lock(impl_->mutex);
  auto found = impl_->probes.find(handle);
  if (found == impl_->probes.end())
    return {};
  auto& set = found->second;
  ProbePoll result{set.state, set.failure, static_cast<unsigned>(set.sent_at.size()), {}};
  if (result.state != RequestState::kPending) {
    result.rtt_ms = std::move(set.rtt_ms);
    impl_->probes.erase(found);
  }
  return result;
}
void Client::SetProbeReplies(bool enabled) {
  std::lock_guard lock(impl_->mutex);
  impl_->probe_replies = enabled;
}
void Client::SetDirect(DirectOptions options) {
  std::lock_guard lock(impl_->mutex);
  impl_->direct_options = options;
}
void Client::SetSideChannelHandler(SideChannelHandler handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->side_channel = std::move(handler);
}
bool Client::SendSideChannel(uint32_t ipv4, std::span<const uint8_t> payload) {
  if (!IsVirtualAddress(ipv4) || !IsSideChannel(payload) || payload.size() > relay::kMaxPayload)
    return false;
  std::lock_guard lock(impl_->mutex);
  if (impl_->stats.state != Association::kLive || impl_->queue.size() >= kMaxQueued)
    return false;
  relay::Message message;
  message.type = relay::Type::kData;
  message.ipv4 = ipv4;
  message.source_port = kProbePort;
  message.destination_port = kProbePort;
  message.payload.assign(payload.begin(), payload.end());
  impl_->queue.push_back(std::move(message));
  ++impl_->stats.side_channel_out;
  impl_->WakeLocked();
  return true;
}
RecordsPoll Client::PollRecords(RequestHandle handle) {
  std::lock_guard lock(impl_->mutex);
  auto found = impl_->requests.find(handle);
  if (found == impl_->requests.end() || found->second.message.type == relay::Type::kTokenRequest)
    return {};
  RecordsPoll result{found->second.state, found->second.failure, {}};
  if (result.state != RequestState::kPending) {
    result.records = std::move(found->second.records);
    impl_->requests.erase(found);
  }
  return result;
}
TokenPoll Client::PollToken(RequestHandle handle) {
  std::lock_guard lock(impl_->mutex);
  auto found = impl_->requests.find(handle);
  if (found == impl_->requests.end() || found->second.message.type != relay::Type::kTokenRequest)
    return {};
  if (found->second.state == RequestState::kPending)
    return {RequestState::kPending, RequestFailure::kNone, std::nullopt};
  TokenPoll result{found->second.state, found->second.failure, std::move(found->second.token)};
  impl_->requests.erase(found);
  return result;
}
Client& SystemLinkClient() {
  static Client client;
  return client;
}

std::expected<int, int> Client::ReceiveFrom(uint64_t socket, std::span<uint8_t> buffer, int flags,
                                            system::N_XSOCKADDR_IN& source, uint32_t& source_size) {
  relay::Endpoint endpoint{};
  auto result = Receive(socket, buffer, flags, endpoint);
  if (result) {
    source.sin_family = system::XSocket::X_AF_INET;
    source.sin_addr = ntohl(endpoint.ipv4);
    source.sin_port = endpoint.port;
    std::memset(source.x_sin_zero, 0, sizeof(source.x_sin_zero));
    source_size = sizeof(system::XSOCKADDR_IN);
  }
  return result;
}

}  // namespace rex::kernel::xam::relay_link
