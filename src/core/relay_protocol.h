#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <compare>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <vector>
#include <rex/result.h>

namespace rex::relay {

constexpr uint8_t kProtocolVersion = 2;
constexpr size_t kMaxPayload = 1200;
constexpr size_t kHeaderSize = 8;
constexpr size_t kMaxDatagram = 1224;
constexpr std::string_view kJoinLabel = "RRLY-JOIN-v1";
using Nonce = std::array<uint8_t, 16>;
using Digest = std::array<uint8_t, 32>;
using TokenId = std::array<uint8_t, 16>;
using Bytes = std::vector<uint8_t>;

constexpr size_t kSessionKeySize = 16;
constexpr size_t kMaxRecordProperties = 384;
constexpr size_t kMaxRecordContexts = 128;
constexpr size_t kMaxRecordQos = 256;
constexpr size_t kMaxSearchResults = 200;
// Fixed record fields before the three variable blobs; see docs/SYSTEM_LINK_RELAY.md.
constexpr size_t kRecordFixedSize = 50;
constexpr size_t kMaxRecordSize =
    kRecordFixedSize + kMaxRecordProperties + kMaxRecordContexts + kMaxRecordQos;
constexpr size_t kResultsHeaderSize = kHeaderSize + 13;

// The 32-byte join-token secret. Every copy wipes itself on destruction and a
// moved-from secret is wiped, so moves and returns leave no unwiped temporary.
class TokenSecret {
 public:
  static constexpr size_t kSize = 32;
  TokenSecret() = default;
  explicit TokenSecret(std::span<const uint8_t, kSize> bytes);
  TokenSecret(const TokenSecret& other);
  TokenSecret(TokenSecret&& other) noexcept;
  TokenSecret& operator=(const TokenSecret& other);
  TokenSecret& operator=(TokenSecret&& other) noexcept;
  ~TokenSecret();
  uint8_t* data() { return bytes_.data(); }
  const uint8_t* data() const { return bytes_.data(); }
  static constexpr size_t size() { return kSize; }
  std::span<const uint8_t> bytes() const { return bytes_; }
  bool operator==(const TokenSecret& other) const;

 private:
  std::array<uint8_t, kSize> bytes_{};
};

// The relay treats properties, contexts and qos as opaque size-capped blobs.
struct SessionRecord {
  uint64_t session_id = 0;  // nonzero; client-chosen
  std::array<uint8_t, kSessionKeySize> key{};
  uint32_t flags = 0;  // XSession create flags
  uint16_t max_public = 0, max_private = 0, open_public = 0, open_private = 0;
  Bytes properties, contexts, qos;
  uint32_t host_peer_id = 0;  // set by the relay in results only
  uint32_t host_ipv4 = 0;     // relay virtual address, results only
  bool operator==(const SessionRecord&) const = default;
};

enum class Type : uint8_t {
  kHello = 1,
  kChallenge,
  kJoin,
  kWelcome,
  kReject,
  kData,
  kDeliver,
  kHeartbeat,
  kAck,
  kBye,
  kPublish,
  kRemove,
  kSearch,
  kLookup,
  kResults,
  kTokenRequest,
  kTokenGrant,
  kStatus,
  kCandidates,
  kConnectRequest,
  kConnectInfo,
  kConnectClose,
};
// Direct-path candidates: a local interface endpoint or the relay-observed one.
constexpr size_t kMaxCandidates = 4;
struct Candidate {
  uint32_t ipv4 = 0;  // network byte order, as in_addr::s_addr
  uint16_t port = 0;  // host order
  auto operator<=>(const Candidate&) const = default;
};
using KeyTag = std::array<uint8_t, 16>;
using WrappedKey = std::array<uint8_t, 32>;
enum class Reason : uint8_t {
  kVersion = 1,
  kMalformed,
  kAuth,
  kRoomFull,
  kTooManyRooms,
  kFingerprintMismatch,
  kRateLimited,
};
// Directory request outcome carried by STATUS.
enum class Status : uint8_t {
  kOk = 0,
  kConflict,
  kLimit,
  kNotFound,
};

// IPv4 values have the same in-memory network byte order as in_addr::s_addr.
// Only fields belonging to type are encoded; Decode initializes all other fields.
// A HELLO with token_join names a relay-issued join token instead of a room.
struct Message {
  Type type = Type::kHello;
  Nonce nonce{};
  std::string room;
  bool token_join = false;
  TokenId join_token{};
  Digest fingerprint{};
  Digest auth{};
  uint32_t peer_id = 0;
  uint64_t token = 0;
  uint32_t ipv4 = 0;
  uint16_t source_port = 0;
  uint16_t destination_port = 0;
  bool broadcast = false;
  Reason reason = Reason::kMalformed;
  Bytes payload;
  uint32_t request_id = 0;
  uint32_t sequence = 0;  // PUBLISH and REMOVE order within one association, nonzero
  uint32_t snapshot = 0;  // RESULTS directory version
  uint64_t session_id = 0;
  uint16_t max_results = 0;
  uint32_t required_flags = 0;
  uint16_t page = 0;
  uint16_t page_count = 0;
  SessionRecord record;
  std::vector<SessionRecord> records;
  TokenSecret token_secret;
  uint32_t expiry_seconds = 0;
  Status status = Status::kOk;
  // CANDIDATES and CONNECT_INFO; CONNECT_REQUEST, CONNECT_INFO and CONNECT_CLOSE
  // carry the other peer's virtual IPv4 in ipv4.
  std::vector<Candidate> candidates;
  Candidate observed;  // CONNECT_INFO: the other peer's relay-observed endpoint
  uint32_t attempt_id = 0;
  WrappedKey wrapped_key{};
  KeyTag key_tag{};  // CANDIDATES, CONNECT_REQUEST, CONNECT_INFO and CONNECT_CLOSE
};

std::expected<Bytes, Reason> Encode(const Message& message);
std::expected<Message, Reason> Decode(std::span<const uint8_t> bytes);
std::expected<Bytes, Reason> EncodeRecord(const SessionRecord& record);
// Decodes exactly one record; trailing bytes are malformed.
std::expected<SessionRecord, Reason> DecodeRecord(std::span<const uint8_t> bytes);
// Packs records in order into RESULTS datagrams of at most kMaxDatagram bytes.
// Zero records produce one empty page.
std::expected<std::vector<Bytes>, Reason> EncodeResults(uint32_t request_id, uint32_t snapshot,
                                                        std::span<const SessionRecord> records);
// Overwrite the bytes without allowing the compiler to remove the writes.
void SecureWipe(std::span<uint8_t> bytes);
Digest HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> data);
Digest JoinAuth(std::string_view secret, const Nonce& client, const Nonce& server,
                std::string_view room, const Digest& fingerprint);
// JoinAuth keyed by the token secret, with the token id's raw bytes in the room position.
Digest TokenJoinAuth(std::span<const uint8_t> token_secret, const Nonce& client,
                     const Nonce& server, const TokenId& token, const Digest& fingerprint);
bool ConstantTimeEqual(const Digest& left, const Digest& right);
bool ConstantTimeEqual(std::span<const uint8_t> left, std::span<const uint8_t> right);
Result<Digest> RandomSeed();

// Direct paths; see "Direct paths" in docs/SYSTEM_LINK_RELAY.md.
constexpr std::string_view kAssociationLabel = "RRLY-ASSOC-v1";
constexpr std::string_view kPairLabel = "RRLY-PAIR-v1";
constexpr std::string_view kPairTagLabel = "RRLY-PAIR-TAG-v1";
constexpr std::string_view kCloseTagLabel = "RRLY-CLOSE-v1";
constexpr std::string_view kCandidatesTagLabel = "RRLY-CAND-v1";
constexpr std::string_view kConnectRequestTagLabel = "RRLY-CREQ-v1";
constexpr std::string_view kDirectLabel = "RRLY-DIRECT-v1";
// K_assoc: HMAC(JOIN key, label || client nonce || server nonce || peer id:u32).
Digest AssociationKey(std::span<const uint8_t> secret, const Nonce& client, const Nonce& server,
                      uint32_t peer_id);
// Fills info.wrapped_key and info.key_tag for the CONNECT_INFO recipient whose
// association key is assoc. The tag covers the attempt id, the other virtual
// IPv4, the observed endpoint and candidates as encoded, and the wrapped key.
void WrapPairKey(std::span<const uint8_t> assoc, std::span<const uint8_t> pair_key, Message& info);
// Returns the pair key of a CONNECT_INFO, or nullopt when its tag does not verify.
std::optional<TokenSecret> UnwrapPairKey(std::span<const uint8_t> assoc, const Message& info);
KeyTag CloseTag(std::span<const uint8_t> assoc, uint32_t attempt_id, uint32_t other_ipv4);
// CANDIDATES and CONNECT_REQUEST end with HMAC(K_assoc, label || every encoded
// byte before the tag)[0:16]. SignConnect fills message.key_tag and returns
// false when the message does not encode; VerifyConnect checks received bytes.
bool SignConnect(std::span<const uint8_t> assoc, Message& message);
bool VerifyConnect(std::span<const uint8_t> assoc, std::span<const uint8_t> bytes);
// K_dir for frames sent by sender_ipv4 under one pair key.
Digest DirectKey(std::span<const uint8_t> pair_key, uint32_t sender_ipv4);

// Direct frames travel peer to peer with their own magic and a truncated MAC.
enum class DirectType : uint8_t {
  kProbe = 1,
  kProbeAck,
  kData,
  kKeepalive,
  kKeepaliveAck,
};
constexpr uint8_t kDirectVersion = 1;
constexpr size_t kDirectHeaderSize = 22;
constexpr size_t kDirectMacSize = 16;
// Control frames carry one u64 echo: zero in requests, the answered sequence in acks.
constexpr size_t kDirectControlSize = kDirectHeaderSize + 8 + kDirectMacSize;
constexpr size_t kDirectDataOverhead = kDirectHeaderSize + 4 + kDirectMacSize;
constexpr size_t kMaxDirectDatagram = kDirectDataOverhead + kMaxPayload;
struct DirectFrame {
  DirectType type = DirectType::kProbe;
  uint32_t attempt_id = 0;
  uint32_t sender_ipv4 = 0;
  uint64_t sequence = 0;  // nonzero, per direction and attempt
  uint64_t echo = 0;
  uint16_t source_port = 0;
  uint16_t destination_port = 0;
  Bytes payload;
};
std::expected<Bytes, Reason> EncodeDirect(const DirectFrame& frame, std::span<const uint8_t> key);
// Parses the structure only; callers then check the MAC with the sender's key.
std::optional<DirectFrame> ParseDirect(std::span<const uint8_t> bytes);
bool VerifyDirect(std::span<const uint8_t> bytes, std::span<const uint8_t> key);
// 64-entry sliding window over one direction's sequence numbers.
class ReplayWindow {
 public:
  static constexpr uint64_t kSize = 64;
  bool Fresh(uint64_t sequence) const;
  void Accept(uint64_t sequence);

 private:
  uint64_t highest_ = 0;
  uint64_t seen_ = 0;  // bit i marks highest_ - i
};

}  // namespace rex::relay
