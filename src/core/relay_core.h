#pragma once

#include "relay_protocol.h"

#include <compare>
#include <map>
#include <optional>

namespace rex::relay {

struct Endpoint {
  uint32_t ipv4 = 0;
  uint16_t port = 0;
  auto operator<=>(const Endpoint&) const = default;
};
struct Datagram {
  Endpoint endpoint;
  Bytes bytes;
};
struct Config {
  size_t max_rooms = 16;
  size_t max_members = 4;
  uint32_t packets_per_second = 500;
  uint32_t bytes_per_second = 256 * 1024;
  uint32_t token_lifetime_seconds = 15 * 60;
};
constexpr size_t kMaxRooms = 256;
constexpr size_t kMaxMembers = 64;
constexpr size_t kMaxPending = 64;
constexpr size_t kMaxPendingPerIp = 4;
constexpr size_t kMaxJoinAttempts = 3;
constexpr size_t kMaxSessionsPerPeer = 2;
constexpr size_t kMaxSessionsPerRoom = 64;
constexpr uint32_t kQueriesPerSecond = 4;   // SEARCH and LOOKUP
constexpr uint32_t kUpdatesPerSecond = 10;  // PUBLISH, REMOVE and TOKEN_REQUEST
constexpr size_t kMaxTokensPerSession = 4;
constexpr uint32_t kConnectsPerSecond = 4;  // CANDIDATES and CONNECT_REQUEST
constexpr uint64_t kAttemptReuseMs = 10000;
constexpr size_t kMaxAttempts = 4096;
enum class Drop : size_t {
  kVersion,
  kMalformed,
  kAuth,
  kRoomFull,
  kTooManyRooms,
  kFingerprintMismatch,
  kRateLimited,
  kDestination,
  kDirection,
  kExpiredChallenge,
  kIdle,
  kDirectoryRateLimited,
  kSessionConflict,
  kSessionLimit,
  kTokenDenied,
  kTokenLimit,
  kCount,
};
// Connect drop reasons, logged after the Drop reasons on the relay CLI counter line.
enum class ConnectDrop : size_t {
  kRateLimited,
  kUnavailable,
  kLimit,
  kAuth,  // CANDIDATES or CONNECT_REQUEST whose K_assoc tag does not verify
  kCount,
};
struct Counters {
  size_t rooms = 0;
  size_t peers = 0;
  size_t pending = 0;
  size_t sessions = 0;
  size_t tokens = 0;
  size_t attempts = 0;  // direct-path pairs with a live attempt
  uint64_t packets_in = 0;
  uint64_t bytes_in = 0;
  uint64_t packets_out = 0;
  uint64_t bytes_out = 0;
  std::array<uint64_t, static_cast<size_t>(Drop::kCount)> drops{};
  std::array<uint64_t, static_cast<size_t>(ConnectDrop::kCount)> connect_drops{};
};

class Core {
 public:
  // Production seeds require fresh OS randomness; time is monotonic
  // milliseconds. Invalid config or empty secret throws std::invalid_argument;
  // authenticated Directory/CONNECT replies may exceed request size.
  Core(Config config, std::string secret, Digest seed);
  std::vector<Datagram> Input(Endpoint source, std::span<const uint8_t> bytes, uint64_t now);
  void Tick(uint64_t now);
  // CONNECT_CLOSE notices produced by Tick; Input returns its own with its output.
  std::vector<Datagram> TakeNotices();
  Counters counters() const;

 private:
  struct Pending {
    Message hello;
    Nonce server;
    uint64_t created;
    size_t failed_joins = 0;
  };
  struct Peer {
    uint32_t id;
    uint64_t token;
    uint32_t ipv4;
    std::string room;
    uint64_t last_seen;
    uint64_t refill;
    double packets;
    double bytes;
    double queries;
    double updates;
    struct JoinReply {
      Digest auth;
      Message welcome;
      uint64_t created;
    };
    std::optional<JoinReply> join_reply;
    // Last PUBLISH or REMOVE applied; older or repeated sequences are acknowledged only.
    uint32_t last_update = 0;
    Status last_update_status = Status::kOk;
    double connects = kConnectsPerSecond;
    // Set by CANDIDATES or CONNECT_REQUEST: the client takes part in direct paths.
    bool direct = false;
    std::vector<Candidate> candidates{};
    TokenSecret assoc{};  // K_assoc; never sent
  };
  // A direct-path attempt for one pair of peers, keyed by (lower id, higher id).
  struct Attempt {
    uint32_t id;
    TokenSecret key;
    uint64_t created;
  };
  struct Room {
    Digest fingerprint;
  };
  using SessionKey = std::pair<std::string, uint64_t>;  // room, session id
  struct Session {
    SessionRecord record;
    uint64_t sequence;
  };
  struct Token {
    SessionKey session;
    TokenSecret secret;
    uint64_t expires;
    uint32_t peer_id;     // host that requested the grant
    uint32_t request_id;  // its TOKEN_REQUEST id, so a retry receives the same grant
  };
  Digest Random();
  void Remove(std::map<Endpoint, Peer>::iterator peer);
  void RemoveSession(std::map<SessionKey, Session>::iterator session);
  template <typename Predicate>
  void EraseTokens(Predicate predicate);
  void Directory(Endpoint source, Peer& peer, const Message& message,
                 std::vector<Datagram>& output);
  void Connect(Endpoint source, Peer& peer, const Message& message, std::vector<Datagram>& output);
  std::vector<Datagram> Receive(Endpoint source, std::span<const uint8_t> bytes, uint64_t now);
  void Emit(Endpoint destination, const Message& message, std::vector<Datagram>& output);
  void Count(Drop reason);
  void Count(ConnectDrop reason);
  Config config_;
  std::string secret_;
  Digest seed_;
  uint64_t random_counter_ = 0;
  uint64_t now_ = 0;
  uint32_t next_peer_id_ = 0;
  std::map<Endpoint, Pending> pending_;
  std::map<Endpoint, Peer> peers_;
  std::map<std::string, Room> rooms_;
  std::map<SessionKey, Session> sessions_;
  std::map<TokenId, Token> tokens_;
  uint64_t next_sequence_ = 0;
  uint32_t directory_version_ = 0;
  std::map<std::pair<uint32_t, uint32_t>, Attempt> attempts_;
  uint32_t next_attempt_id_ = 0;
  std::vector<Datagram> notices_;
  Counters counters_;
};

}  // namespace rex::relay
