#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <rex/session_data.h>

#include "lan_system_link.h"

// Host session data: the protocol, state machine and transports behind
// rex::session_data. See docs/MAINTAINING.md, "Host session data".
namespace rex::kernel::xam::host_data {

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<uint8_t>;
using SessionId = std::array<uint8_t, 8>;
using SessionKey = std::array<uint8_t, 16>;
using Nonce = std::array<uint8_t, 16>;

// Message: ASCII RXSD, version:u8, type:u8, data length:u16 LE, session id[8],
// nonce[16], data[length], then HMAC-SHA256(key, kMacLabel || every preceding
// byte). Requests carry no data. Replies echo the request's nonce. Types 3/4
// request/serve only a prepared game's snapshot. Old peers reject these types.
// Game messages add a u64 LE generation after the nonce: minimum requested on
// a request, actual prepared generation on a reply, included in the HMAC.
// Then requester id[16] and attempt:u64 LE bind replies and retries to one
// joined instance's game attempt, including when the earlier reply was lost.
constexpr std::string_view kMagic = "RXSD";
constexpr std::string_view kMacLabel = "RXSD-HOSTDATA-v1";
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr size_t kMacSize = 32;
constexpr size_t kRequestSize = kHeaderSize + kMacSize;
constexpr size_t kGameHeaderSize = kHeaderSize + 8 + 16 + 8;
constexpr size_t kMaxMessageSize = kGameHeaderSize + kMacSize + rex::session_data::kMaxBytes;

// Native UDP port of the direct-LAN side socket, bound to the selected address.
constexpr uint16_t kLanPort = 30110;

// A joiner retries after 250 ms, doubling to 2 s, and gives up 15 s after create.
constexpr std::chrono::milliseconds kFirstRetry{250};
constexpr std::chrono::milliseconds kMaxRetry{2000};
constexpr std::chrono::milliseconds kGiveUp{15000};

enum class Type : uint8_t { kRequest = 1, kReply = 2, kGameRequest = 3, kGameReply = 4 };

struct Message {
  Type type = Type::kRequest;
  SessionId session_id{};
  Nonce nonce{};
  Bytes data;
  uint64_t game_generation = 0;
  Nonce requester_id{};
  uint64_t game_attempt = 0;
};

// True when `bytes` starts with kMagic; used to route side-channel datagrams.
bool HasMagic(std::span<const uint8_t> bytes);
// Empty when the data exceeds kMaxBytes or a request carries data.
Bytes Encode(const Message& message, const SessionKey& key);
// Nullopt for malformed, oversized or unauthenticated bytes.
std::optional<Message> Decode(std::span<const uint8_t> bytes, const SessionKey& key);

// XSESSION_INFO: XNKID at +0, XNADDR (ina first) at +8, XNKEY at +44.
struct SessionInfo {
  SessionId id{};
  uint32_t host_ipv4 = 0;  // network order, as in XNADDR
  SessionKey key{};
};
SessionInfo ParseSessionInfo(std::span<const uint8_t, 60> info);

enum class Link : uint8_t { kNone, kRelay, kLan };
struct Route {
  Link link = Link::kNone;
  uint32_t ipv4 = 0;  // network order
  uint16_t port = 0;  // host order; zero on the relay
  bool operator==(const Route&) const = default;
};
struct Outgoing {
  Route route;
  Bytes bytes;
};

// Allow kRepliesPerNonce replies for each nonce, retaining at most
// kMaxAnsweredNonces per session. The aggregate rate is kRepliesPerSecond
// with a burst of that size. Joiners change nonce after kRepliesPerNonce requests.
constexpr unsigned kRepliesPerNonce = 3;
constexpr size_t kMaxAnsweredNonces = 64;
constexpr double kRepliesPerSecond = 20;
// Retain every served requester until session deletion to preserve freshness
// evidence. A full cache refuses new IDs instead of evicting existing records.
constexpr size_t kMaxGameRequesters = 64;

// Session state shared by the guest thread (create and delete), the transport
// threads (Receive) and the timer (Due). Every member is thread-safe. Each
// create and delete starts a new session generation.
class Core {
 public:
  using Provider = std::function<Bytes()>;
  using RandomNonce = std::function<Nonce()>;

  // `random` supplies requester IDs and request nonces. The default uses the OS random source.
  explicit Core(RandomNonce random = {});

  void SetProvider(Provider provider);
  // Start at kNone and call the provider outside the lock. Cap its output at
  // kMaxBytes; discard it if deletion or a newer create replaces the generation.
  // Log provider exceptions without publishing bytes. Info requests work on any link.
  void HostCreated(const SessionInfo& info);
  // Enters kPending and returns the first request for `host`. A route without
  // a link or address enters kUnavailable at once and returns nothing.
  std::optional<Outgoing> JoinCreated(const SessionInfo& info, Route host, Clock::time_point now);
  void Deleted();
  // Capture once on the host, or start a fresh game-only joiner request.
  // Until host preparation finishes no game request receives a reply.
  std::optional<Outgoing> BeginGame(Clock::time_point now);
  // Invalidate readiness and any in-flight capture/request for the ended game.
  void EndGame();

  // Handles one datagram from `from`; returns the reply to a valid request.
  // Invalid, unexpected, oversized, replayed or rate-limited datagrams are
  // dropped with a debug log.
  std::optional<Outgoing> Receive(Route from, std::span<const uint8_t> bytes,
                                  Clock::time_point now);
  // Returns a retry when one is due; past kGiveUp enters kUnavailable.
  std::optional<Outgoing> Due(Clock::time_point now);
  // The next retry or give-up time while pending.
  std::optional<Clock::time_point> NextDeadline() const;

  rex::session_data::Status Current() const;
  // Waits while kPending. When a delete or create ends the waited session,
  // returns kNone without bytes even if a newer session exists.
  rex::session_data::Status Wait(std::chrono::milliseconds timeout);

 private:
  void StartGenerationLocked(const SessionInfo& info, bool new_session = true);
  void CaptureHost(const SessionInfo& info, bool for_game, uint64_t expected_generation = 0);
  std::optional<Outgoing> StartJoinLocked(const SessionInfo& info, Route host,
                                          Clock::time_point now, bool for_game);
  Outgoing RequestLocked();
  bool TakeReplyTokenLocked(Clock::time_point now);
  bool BindGameRequestLocked(const Message& message, Route from);

  struct GameRequester {
    Nonce id{};
    Route route;
    uint64_t attempt = 0;
    uint64_t generation = 0;
  };

  RandomNonce random_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Provider provider_;
  uint64_t generation_ = 0;
  rex::session_data::State state_ = rex::session_data::State::kNone;
  Bytes bytes_;
  SessionInfo info_{};
  bool serving_ = false;  // host with a session id and key to answer requests for
  bool host_session_ = false;
  bool preparing_game_ = false;
  bool game_ready_ = false;
  bool game_request_ = false;
  uint64_t game_generation_ = 0;  // host generation or joiner's accepted/retired floor
  uint64_t requested_game_generation_ = 0;
  Nonce requester_id_{};
  uint64_t game_attempt_ = 0;
  std::vector<GameRequester> game_requesters_;
  std::vector<std::pair<Nonce, unsigned>> answered_;  // host: replies per request nonce
  double reply_tokens_ = kRepliesPerSecond;           // shared by every session
  std::optional<Clock::time_point> reply_refill_;
  Route host_{};
  std::vector<Nonce> nonces_;  // joiner: every nonce this session has sent
  unsigned nonce_uses_ = 0;    // requests sent with nonces_.back()
  Clock::time_point give_up_{};
  Clock::time_point next_retry_{};
  std::chrono::milliseconds retry_delay_{};
};

// Native UDP side socket for direct LAN. One worker thread owns the socket,
// binds it to the requested address and port, and passes every datagram with
// the magic to the handler. Bind and Send never wait for the worker.
class LanLink {
 public:
  using Handler = std::function<void(Route from, std::span<const uint8_t> bytes)>;
  // `port` zero binds an ephemeral port, for tests.
  LanLink(uint16_t port, Handler handler);
  ~LanLink();
  LanLink(const LanLink&) = delete;
  LanLink& operator=(const LanLink&) = delete;

  // Requests a bind to `ipv4` (network order); zero closes the socket. A failed
  // bind is logged and retried on the next call.
  void Bind(uint32_t ipv4);
  bool Send(Route to, std::span<const uint8_t> bytes);
  // Joins the worker and closes the socket; later Bind calls do nothing.
  void Stop();
  // The bound port, or zero while unbound.
  uint16_t port() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Owns a Core, the LAN link and the retry timer. Sends go to `sender`, or, when
// it is null, through the process relay client and the LAN link; only such a
// service registers the relay side-channel handler.
class Service {
 public:
  using Sender = std::function<bool(const Outgoing&)>;
  explicit Service(Sender sender = {}, uint16_t lan_port = kLanPort);
  ~Service();
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  Core& core() { return core_; }
  // XGI hooks. A create reads the session the LAN state retained for it; a
  // create without System Link freezes host bytes but serves nobody, and a
  // non-host create without System Link is unavailable at once.
  void Created(uint32_t flags, const lan::SessionSnapshot& session);
  // Enters kPending toward `host`, sends the first request and arms the timer.
  void Join(const SessionInfo& info, Route host);
  void BeginGame();
  void Receive(Route from, std::span<const uint8_t> bytes);
  // Stops and joins the timer and LAN worker and removes the relay handler.
  // Idempotent; afterwards nothing is sent and state queries still work.
  void Shutdown();

 private:
  void Send(const Outgoing& outgoing);
  void Kick();
  void TimerLoop();

  Sender sender_;
  bool relay_handler_ = false;
  Core core_;
  LanLink lan_;
  std::mutex timer_mutex_;
  std::condition_variable timer_changed_;
  std::thread timer_;
  bool wake_ = false;
  bool stopped_ = false;
};

// The process service behind rex::session_data. It is never freed; an atexit
// hook registered after the relay client exists shuts it down first.
Service& ProcessService();
void OnSessionCreate(uint32_t flags, const lan::SessionSnapshot& session);
void OnSessionDelete();
void OnSessionStart();
void OnSessionEnd();
// The last XNetCleanup ends any session data, like a delete.
void OnNetworkCleanup();

}  // namespace rex::kernel::xam::host_data
