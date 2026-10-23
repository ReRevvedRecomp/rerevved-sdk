#include "relay_protocol.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

#include "thirdparty/crypto/sha256.h"

namespace rex::relay {
Result<Digest> RandomSeed() {
  Digest seed{};
#if defined(_WIN32)
  if (BCryptGenRandom(nullptr, seed.data(), static_cast<ULONG>(seed.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    return std::unexpected(Error{ErrorCategory::Crypto, "Relay random seed failed"});
#else
  std::ifstream random("/dev/urandom", std::ios::binary);
  if (!random.read(reinterpret_cast<char*>(seed.data()), seed.size()))
    return std::unexpected(Error{ErrorCategory::Crypto, "Relay random seed failed"});
#endif
  return seed;
}

namespace {

void Put(Bytes& bytes, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

void Append(Bytes& bytes, std::span<const uint8_t> value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

std::span<const uint8_t> Raw(std::string_view value) {
  return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

void PutIp(Bytes& bytes, uint32_t ip) {
  Append(bytes, {reinterpret_cast<const uint8_t*>(&ip), sizeof(ip)});
}

Digest Hash(std::span<const uint8_t> bytes) {
  ::sha256::SHA256 hash;
  hash.add(bytes.data(), bytes.size());
  Digest result{};
  hash.getHash(result.data());
  return result;
}

void Wipe(std::span<uint8_t> bytes) {
  volatile uint8_t* data = bytes.data();
  for (size_t i = 0; i < bytes.size(); ++i)
    data[i] = 0;
}

bool ValidRecord(const SessionRecord& r) {
  return r.session_id && r.open_public <= r.max_public && r.open_private <= r.max_private &&
         r.properties.size() <= kMaxRecordProperties && r.contexts.size() <= kMaxRecordContexts &&
         r.qos.size() <= kMaxRecordQos;
}

void PutRecord(Bytes& bytes, const SessionRecord& r) {
  Put(bytes, r.session_id, 8);
  Append(bytes, r.key);
  Put(bytes, r.flags, 4);
  Put(bytes, r.max_public, 2);
  Put(bytes, r.max_private, 2);
  Put(bytes, r.open_public, 2);
  Put(bytes, r.open_private, 2);
  Put(bytes, r.host_peer_id, 4);
  PutIp(bytes, r.host_ipv4);
  for (const Bytes* blob : {&r.properties, &r.contexts, &r.qos}) {
    Put(bytes, blob->size(), 2);
    Append(bytes, *blob);
  }
}

size_t RecordSize(const SessionRecord& r) {
  return kRecordFixedSize + r.properties.size() + r.contexts.size() + r.qos.size();
}

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> bytes, size_t pos = 0) : bytes_(bytes), pos_(pos) {}
  bool Has(size_t count) const { return bytes_.size() - pos_ >= count; }
  size_t remaining() const { return bytes_.size() - pos_; }
  uint64_t Get(size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
      value |= uint64_t(bytes_[pos_++]) << (8 * i);
    return value;
  }
  template <typename T>
  void Copy(T& value) {
    std::memcpy(value.data(), bytes_.data() + pos_, value.size());
    pos_ += value.size();
  }
  uint32_t Ip() {
    uint32_t value;
    std::memcpy(&value, bytes_.data() + pos_, 4);
    pos_ += 4;
    return value;
  }
  void Blob(Bytes& value, size_t count) {
    value.assign(bytes_.begin() + pos_, bytes_.begin() + pos_ + count);
    pos_ += count;
  }
  void Skip(size_t count) { pos_ += count; }

 private:
  std::span<const uint8_t> bytes_;
  size_t pos_;
};

std::expected<SessionRecord, Reason> ReadRecord(Reader& in) {
  if (!in.Has(kRecordFixedSize))
    return std::unexpected(Reason::kMalformed);
  SessionRecord r;
  r.session_id = in.Get(8);
  in.Copy(r.key);
  r.flags = static_cast<uint32_t>(in.Get(4));
  r.max_public = static_cast<uint16_t>(in.Get(2));
  r.max_private = static_cast<uint16_t>(in.Get(2));
  r.open_public = static_cast<uint16_t>(in.Get(2));
  r.open_private = static_cast<uint16_t>(in.Get(2));
  r.host_peer_id = static_cast<uint32_t>(in.Get(4));
  r.host_ipv4 = in.Ip();
  const std::array<std::pair<Bytes*, size_t>, 3> blobs{
      {{&r.properties, kMaxRecordProperties},
       {&r.contexts, kMaxRecordContexts},
       {&r.qos, kMaxRecordQos}}};
  for (size_t i = 0; i < blobs.size(); ++i) {
    // The fixed-size check above already covered the first length field.
    if (i && !in.Has(2))
      return std::unexpected(Reason::kMalformed);
    const auto size = static_cast<size_t>(in.Get(2));
    if (size > blobs[i].second || !in.Has(size))
      return std::unexpected(Reason::kMalformed);
    in.Blob(*blobs[i].first, size);
  }
  if (!ValidRecord(r))
    return std::unexpected(Reason::kMalformed);
  return r;
}

bool ValidCandidate(const Candidate& candidate) {
  return candidate.ipv4 && candidate.port;
}
bool ValidCandidates(const std::vector<Candidate>& candidates) {
  return candidates.size() <= kMaxCandidates &&
         std::all_of(candidates.begin(), candidates.end(), ValidCandidate);
}
// count:u8, then count x (IPv4[4], port:u16).
void PutCandidates(Bytes& bytes, const std::vector<Candidate>& candidates) {
  Put(bytes, candidates.size(), 1);
  for (const auto& candidate : candidates) {
    PutIp(bytes, candidate.ipv4);
    Put(bytes, candidate.port, 2);
  }
}
bool ReadCandidates(Reader& in, std::vector<Candidate>& candidates) {
  if (!in.Has(1))
    return false;
  const auto count = static_cast<size_t>(in.Get(1));
  if (count > kMaxCandidates || !in.Has(count * 6))
    return false;
  for (size_t i = 0; i < count; ++i) {
    Candidate candidate;
    candidate.ipv4 = in.Ip();
    candidate.port = static_cast<uint16_t>(in.Get(2));
    if (!ValidCandidate(candidate))
      return false;
    candidates.push_back(candidate);
  }
  return true;
}

void Finish(Bytes& bytes) {
  bytes[6] = static_cast<uint8_t>(bytes.size());
  bytes[7] = static_cast<uint8_t>(bytes.size() >> 8);
}

}  // namespace

std::expected<Bytes, Reason> EncodeRecord(const SessionRecord& record) {
  if (!ValidRecord(record))
    return std::unexpected(Reason::kMalformed);
  Bytes bytes;
  bytes.reserve(RecordSize(record));
  PutRecord(bytes, record);
  return bytes;
}

std::expected<SessionRecord, Reason> DecodeRecord(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  auto record = ReadRecord(in);
  if (record && in.remaining())
    return std::unexpected(Reason::kMalformed);
  return record;
}

void SecureWipe(std::span<uint8_t> bytes) {
  Wipe(bytes);
}

TokenSecret::TokenSecret(std::span<const uint8_t, kSize> bytes) {
  std::copy(bytes.begin(), bytes.end(), bytes_.begin());
}
TokenSecret::TokenSecret(const TokenSecret& other) : bytes_(other.bytes_) {}
TokenSecret::TokenSecret(TokenSecret&& other) noexcept : bytes_(other.bytes_) {
  Wipe(other.bytes_);
}
TokenSecret& TokenSecret::operator=(const TokenSecret& other) {
  bytes_ = other.bytes_;
  return *this;
}
TokenSecret& TokenSecret::operator=(TokenSecret&& other) noexcept {
  if (this != &other) {
    bytes_ = other.bytes_;
    Wipe(other.bytes_);
  }
  return *this;
}
TokenSecret::~TokenSecret() {
  Wipe(bytes_);
}
bool TokenSecret::operator==(const TokenSecret& other) const {
  volatile uint8_t difference = 0;
  for (size_t i = 0; i < kSize; ++i)
    difference = static_cast<uint8_t>(difference | (bytes_[i] ^ other.bytes_[i]));
  return difference == 0;
}

std::expected<std::vector<Bytes>, Reason> EncodeResults(uint32_t request_id, uint32_t snapshot,
                                                        std::span<const SessionRecord> records) {
  if (records.size() > kMaxSearchResults)
    return std::unexpected(Reason::kMalformed);
  std::vector<Bytes> pages;
  auto open = [&] {
    Bytes page{'R', 'R', 'L', 'Y', kProtocolVersion, static_cast<uint8_t>(Type::kResults), 0, 0};
    Put(page, request_id, 4);
    Put(page, snapshot, 4);
    Put(page, 0, 5);  // Page index, page count and record count are filled below.
    pages.push_back(std::move(page));
  };
  open();
  for (const auto& record : records) {
    if (!ValidRecord(record))
      return std::unexpected(Reason::kMalformed);
    if (pages.back().size() + RecordSize(record) > kMaxDatagram || pages.back()[20] == 255)
      open();
    PutRecord(pages.back(), record);
    ++pages.back()[20];
  }
  for (size_t i = 0; i < pages.size(); ++i) {
    auto& page = pages[i];
    page[16] = static_cast<uint8_t>(i);
    page[17] = static_cast<uint8_t>(i >> 8);
    page[18] = static_cast<uint8_t>(pages.size());
    page[19] = static_cast<uint8_t>(pages.size() >> 8);
    Finish(page);
  }
  return pages;
}

std::expected<Bytes, Reason> Encode(const Message& m) {
  Bytes bytes{'R', 'R', 'L', 'Y', kProtocolVersion, static_cast<uint8_t>(m.type), 0, 0};
  // One allocation up front: growth would leave freed copies of TOKEN_GRANT secrets.
  bytes.reserve(kMaxDatagram);
  switch (m.type) {
    case Type::kHello:
      Append(bytes, m.nonce);
      if (m.token_join) {
        if (!m.room.empty())
          return std::unexpected(Reason::kMalformed);
        Put(bytes, 0, 1);
        Append(bytes, m.join_token);
      } else {
        if (m.room.empty() || m.room.size() > 32)
          return std::unexpected(Reason::kMalformed);
        Put(bytes, m.room.size(), 1);
        Append(bytes, Raw(m.room));
      }
      Append(bytes, m.fingerprint);
      break;
    case Type::kChallenge:
      Append(bytes, m.nonce);
      break;
    case Type::kJoin:
      Append(bytes, m.auth);
      break;
    case Type::kWelcome:
      Put(bytes, m.peer_id, 4);
      Put(bytes, m.token, 8);
      PutIp(bytes, m.ipv4);
      break;
    case Type::kReject:
      if (m.reason < Reason::kVersion || m.reason > Reason::kRateLimited)
        return std::unexpected(Reason::kMalformed);
      Put(bytes, static_cast<uint8_t>(m.reason), 1);
      break;
    case Type::kData:
    case Type::kDeliver:
      if (m.payload.size() > kMaxPayload)
        return std::unexpected(Reason::kMalformed);
      if (m.type == Type::kData) {
        Put(bytes, m.token, 8);
        Put(bytes, m.source_port, 2);
        PutIp(bytes, m.ipv4);
        Put(bytes, m.destination_port, 2);
      } else {
        PutIp(bytes, m.ipv4);
        Put(bytes, m.source_port, 2);
        Put(bytes, m.destination_port, 2);
        Put(bytes, m.broadcast ? 1 : 0, 1);
      }
      Append(bytes, m.payload);
      break;
    case Type::kHeartbeat:
    case Type::kAck:
    case Type::kBye:
      Put(bytes, m.token, 8);
      break;
    case Type::kPublish:
      if (!ValidRecord(m.record) || !m.sequence)
        return std::unexpected(Reason::kMalformed);
      Put(bytes, m.token, 8);
      Put(bytes, m.request_id, 4);
      Put(bytes, m.sequence, 4);
      PutRecord(bytes, m.record);
      break;
    case Type::kRemove:
      if (!m.session_id || !m.sequence)
        return std::unexpected(Reason::kMalformed);
      Put(bytes, m.token, 8);
      Put(bytes, m.request_id, 4);
      Put(bytes, m.sequence, 4);
      Put(bytes, m.session_id, 8);
      break;
    case Type::kLookup:
    case Type::kTokenRequest:
      if (!m.session_id)
        return std::unexpected(Reason::kMalformed);
      Put(bytes, m.token, 8);
      Put(bytes, m.request_id, 4);
      Put(bytes, m.session_id, 8);
      break;
    case Type::kSearch:
      if (!m.max_results || m.max_results > kMaxSearchResults)
        return std::unexpected(Reason::kMalformed);
      Put(bytes, m.token, 8);
      Put(bytes, m.request_id, 4);
      Put(bytes, m.max_results, 2);
      Put(bytes, m.required_flags, 4);
      break;
    case Type::kResults:
      if (!m.page_count || m.page >= m.page_count || m.page_count > kMaxSearchResults ||
          m.records.size() > 255)
        return std::unexpected(Reason::kMalformed);
      Put(bytes, m.request_id, 4);
      Put(bytes, m.snapshot, 4);
      Put(bytes, m.page, 2);
      Put(bytes, m.page_count, 2);
      Put(bytes, m.records.size(), 1);
      for (const auto& record : m.records) {
        if (!ValidRecord(record))
          return std::unexpected(Reason::kMalformed);
        PutRecord(bytes, record);
      }
      if (bytes.size() > kMaxDatagram)
        return std::unexpected(Reason::kMalformed);
      break;
    case Type::kTokenGrant:
      Put(bytes, m.request_id, 4);
      Append(bytes, m.join_token);
      Append(bytes, m.token_secret.bytes());
      Put(bytes, m.expiry_seconds, 4);
      break;
    case Type::kStatus:
      if (m.status > Status::kNotFound)
        return std::unexpected(Reason::kMalformed);
      Put(bytes, m.request_id, 4);
      Put(bytes, static_cast<uint8_t>(m.status), 1);
      break;
    case Type::kCandidates:
      if (!ValidCandidates(m.candidates))
        return std::unexpected(Reason::kMalformed);
      Put(bytes, m.token, 8);
      PutCandidates(bytes, m.candidates);
      Append(bytes, m.key_tag);
      break;
    case Type::kConnectRequest:
      Put(bytes, m.token, 8);
      PutIp(bytes, m.ipv4);
      Append(bytes, m.key_tag);
      break;
    case Type::kConnectInfo:
      if (!m.attempt_id || !ValidCandidates(m.candidates) || !ValidCandidate(m.observed))
        return std::unexpected(Reason::kMalformed);
      Put(bytes, m.attempt_id, 4);
      PutIp(bytes, m.ipv4);
      PutIp(bytes, m.observed.ipv4);
      Put(bytes, m.observed.port, 2);
      PutCandidates(bytes, m.candidates);
      Append(bytes, m.wrapped_key);
      Append(bytes, m.key_tag);
      break;
    case Type::kConnectClose:
      if (!m.attempt_id)
        return std::unexpected(Reason::kMalformed);
      Put(bytes, m.attempt_id, 4);
      PutIp(bytes, m.ipv4);
      Append(bytes, m.key_tag);
      break;
    default:
      return std::unexpected(Reason::kMalformed);
  }
  Finish(bytes);
  return bytes;
}

std::expected<Message, Reason> Decode(std::span<const uint8_t> bytes) {
  if (bytes.size() < kHeaderSize || bytes.size() > kMaxDatagram ||
      std::memcmp(bytes.data(), "RRLY", 4) != 0)
    return std::unexpected(Reason::kMalformed);
  if (bytes[4] != kProtocolVersion)
    return std::unexpected(Reason::kVersion);
  if (size_t(bytes[6] | (bytes[7] << 8)) != bytes.size())
    return std::unexpected(Reason::kMalformed);
  Message m;
  m.type = static_cast<Type>(bytes[5]);
  Reader in(bytes, kHeaderSize);
  const size_t size = bytes.size();
  switch (m.type) {
    case Type::kHello:
      if (size >= 25 && bytes[24] == 0) {
        if (size != 73)
          return std::unexpected(Reason::kMalformed);
        in.Copy(m.nonce);
        in.Skip(1);
        m.token_join = true;
        in.Copy(m.join_token);
        in.Copy(m.fingerprint);
        break;
      }
      if (size < 58 || size > 89 || bytes[24] > 32 || size != size_t(57 + bytes[24]))
        return std::unexpected(Reason::kMalformed);
      in.Copy(m.nonce);
      in.Skip(1);
      m.room.assign(reinterpret_cast<const char*>(bytes.data() + 25), bytes[24]);
      in.Skip(bytes[24]);
      in.Copy(m.fingerprint);
      break;
    case Type::kChallenge:
      if (size != 24)
        return std::unexpected(Reason::kMalformed);
      in.Copy(m.nonce);
      break;
    case Type::kJoin:
      if (size != 40)
        return std::unexpected(Reason::kMalformed);
      in.Copy(m.auth);
      break;
    case Type::kWelcome:
      if (size != 24)
        return std::unexpected(Reason::kMalformed);
      m.peer_id = static_cast<uint32_t>(in.Get(4));
      m.token = in.Get(8);
      m.ipv4 = in.Ip();
      break;
    case Type::kReject:
      if (size != 9 || bytes[8] < 1 || bytes[8] > 7)
        return std::unexpected(Reason::kMalformed);
      m.reason = static_cast<Reason>(in.Get(1));
      break;
    case Type::kData:
    case Type::kDeliver: {
      const size_t fixed = m.type == Type::kData ? 24 : 17;
      if (size < fixed || size > fixed + kMaxPayload)
        return std::unexpected(Reason::kMalformed);
      if (m.type == Type::kData) {
        m.token = in.Get(8);
        m.source_port = static_cast<uint16_t>(in.Get(2));
        m.ipv4 = in.Ip();
        m.destination_port = static_cast<uint16_t>(in.Get(2));
      } else {
        m.ipv4 = in.Ip();
        m.source_port = static_cast<uint16_t>(in.Get(2));
        m.destination_port = static_cast<uint16_t>(in.Get(2));
        if (bytes[16] > 1)
          return std::unexpected(Reason::kMalformed);
        m.broadcast = in.Get(1) != 0;
      }
      in.Blob(m.payload, in.remaining());
      break;
    }
    case Type::kHeartbeat:
    case Type::kAck:
    case Type::kBye:
      if (size != 16)
        return std::unexpected(Reason::kMalformed);
      m.token = in.Get(8);
      break;
    case Type::kPublish: {
      if (size < 24)
        return std::unexpected(Reason::kMalformed);
      m.token = in.Get(8);
      m.request_id = static_cast<uint32_t>(in.Get(4));
      m.sequence = static_cast<uint32_t>(in.Get(4));
      auto record = ReadRecord(in);
      if (!record || in.remaining() || !m.sequence)
        return std::unexpected(Reason::kMalformed);
      m.record = std::move(*record);
      break;
    }
    case Type::kRemove:
      if (size != 32)
        return std::unexpected(Reason::kMalformed);
      m.token = in.Get(8);
      m.request_id = static_cast<uint32_t>(in.Get(4));
      m.sequence = static_cast<uint32_t>(in.Get(4));
      m.session_id = in.Get(8);
      if (!m.session_id || !m.sequence)
        return std::unexpected(Reason::kMalformed);
      break;
    case Type::kLookup:
    case Type::kTokenRequest:
      if (size != 28)
        return std::unexpected(Reason::kMalformed);
      m.token = in.Get(8);
      m.request_id = static_cast<uint32_t>(in.Get(4));
      m.session_id = in.Get(8);
      if (!m.session_id)
        return std::unexpected(Reason::kMalformed);
      break;
    case Type::kSearch:
      if (size != 26)
        return std::unexpected(Reason::kMalformed);
      m.token = in.Get(8);
      m.request_id = static_cast<uint32_t>(in.Get(4));
      m.max_results = static_cast<uint16_t>(in.Get(2));
      m.required_flags = static_cast<uint32_t>(in.Get(4));
      if (!m.max_results || m.max_results > kMaxSearchResults)
        return std::unexpected(Reason::kMalformed);
      break;
    case Type::kResults: {
      if (size < kResultsHeaderSize)
        return std::unexpected(Reason::kMalformed);
      m.request_id = static_cast<uint32_t>(in.Get(4));
      m.snapshot = static_cast<uint32_t>(in.Get(4));
      m.page = static_cast<uint16_t>(in.Get(2));
      m.page_count = static_cast<uint16_t>(in.Get(2));
      const auto count = static_cast<size_t>(in.Get(1));
      if (!m.page_count || m.page >= m.page_count || m.page_count > kMaxSearchResults)
        return std::unexpected(Reason::kMalformed);
      for (size_t i = 0; i < count; ++i) {
        auto record = ReadRecord(in);
        if (!record)
          return std::unexpected(Reason::kMalformed);
        m.records.push_back(std::move(*record));
      }
      if (in.remaining())
        return std::unexpected(Reason::kMalformed);
      break;
    }
    case Type::kTokenGrant:
      if (size != 64)
        return std::unexpected(Reason::kMalformed);
      m.request_id = static_cast<uint32_t>(in.Get(4));
      in.Copy(m.join_token);
      in.Copy(m.token_secret);
      m.expiry_seconds = static_cast<uint32_t>(in.Get(4));
      break;
    case Type::kStatus:
      if (size != 13 || bytes[12] > static_cast<uint8_t>(Status::kNotFound))
        return std::unexpected(Reason::kMalformed);
      m.request_id = static_cast<uint32_t>(in.Get(4));
      m.status = static_cast<Status>(in.Get(1));
      break;
    case Type::kCandidates:
      if (size < 33 || size > 33 + 6 * kMaxCandidates)
        return std::unexpected(Reason::kMalformed);
      m.token = in.Get(8);
      if (!ReadCandidates(in, m.candidates) || in.remaining() != m.key_tag.size())
        return std::unexpected(Reason::kMalformed);
      in.Copy(m.key_tag);
      break;
    case Type::kConnectRequest:
      if (size != 36)
        return std::unexpected(Reason::kMalformed);
      m.token = in.Get(8);
      m.ipv4 = in.Ip();
      in.Copy(m.key_tag);
      break;
    case Type::kConnectInfo:
      if (size < 71 || size > 71 + 6 * kMaxCandidates)
        return std::unexpected(Reason::kMalformed);
      m.attempt_id = static_cast<uint32_t>(in.Get(4));
      m.ipv4 = in.Ip();
      m.observed.ipv4 = in.Ip();
      m.observed.port = static_cast<uint16_t>(in.Get(2));
      if (!m.attempt_id || !ValidCandidate(m.observed) || !ReadCandidates(in, m.candidates) ||
          in.remaining() != m.wrapped_key.size() + m.key_tag.size())
        return std::unexpected(Reason::kMalformed);
      in.Copy(m.wrapped_key);
      in.Copy(m.key_tag);
      break;
    case Type::kConnectClose:
      if (size != 32)
        return std::unexpected(Reason::kMalformed);
      m.attempt_id = static_cast<uint32_t>(in.Get(4));
      m.ipv4 = in.Ip();
      in.Copy(m.key_tag);
      if (!m.attempt_id)
        return std::unexpected(Reason::kMalformed);
      break;
    default:
      return std::unexpected(Reason::kMalformed);
  }
  return m;
}

Digest HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> data) {
  std::array<uint8_t, 64> block{};
  if (key.size() > block.size()) {
    auto digest = Hash(key);
    std::copy(digest.begin(), digest.end(), block.begin());
    Wipe(digest);
  } else {
    std::copy(key.begin(), key.end(), block.begin());
  }
  Bytes inner(64), outer(64);
  // Allocate before filling the pads so growth cannot leave unwiped copies.
  inner.reserve(64 + data.size());
  outer.reserve(64 + Digest{}.size());
  for (size_t i = 0; i < block.size(); ++i) {
    inner[i] = block[i] ^ 0x36;
    outer[i] = block[i] ^ 0x5c;
  }
  Append(inner, data);
  auto inner_hash = Hash(inner);
  Append(outer, inner_hash);
  auto result = Hash(outer);
  Wipe(block);
  Wipe(inner);
  Wipe(outer);
  Wipe(inner_hash);
  return result;
}

Digest JoinAuth(std::string_view secret, const Nonce& client, const Nonce& server,
                std::string_view room, const Digest& fingerprint) {
  Bytes data;
  Append(data, Raw(kJoinLabel));
  Append(data, client);
  Append(data, server);
  Put(data, room.size(), 1);
  Append(data, Raw(room));
  Append(data, fingerprint);
  return HmacSha256(Raw(secret), data);
}

Digest TokenJoinAuth(std::span<const uint8_t> token_secret, const Nonce& client,
                     const Nonce& server, const TokenId& token, const Digest& fingerprint) {
  return JoinAuth({reinterpret_cast<const char*>(token_secret.data()), token_secret.size()},
                  client, server, {reinterpret_cast<const char*>(token.data()), token.size()},
                  fingerprint);
}

bool ConstantTimeEqual(const Digest& left, const Digest& right) {
  volatile uint8_t difference = 0;
  for (size_t i = 0; i < left.size(); ++i) {
    difference = static_cast<uint8_t>(difference | (left[i] ^ right[i]));
  }
  return difference == 0;
}

bool ConstantTimeEqual(std::span<const uint8_t> left, std::span<const uint8_t> right) {
  if (left.size() != right.size())
    return false;
  volatile uint8_t difference = 0;
  for (size_t i = 0; i < left.size(); ++i)
    difference = static_cast<uint8_t>(difference | (left[i] ^ right[i]));
  return difference == 0;
}

namespace {
// HMAC(key, label || attempt id:u32 || IPv4[4] || extra).
Digest LabeledMac(std::span<const uint8_t> key, std::string_view label, uint32_t attempt_id,
                  uint32_t ipv4, std::span<const uint8_t> extra = {}) {
  Bytes data;
  Append(data, Raw(label));
  Put(data, attempt_id, 4);
  PutIp(data, ipv4);
  Append(data, extra);
  return HmacSha256(key, data);
}
KeyTag Truncate(Digest digest) {
  KeyTag tag{};
  std::copy_n(digest.begin(), tag.size(), tag.begin());
  Wipe(digest);
  return tag;
}
}  // namespace

Digest AssociationKey(std::span<const uint8_t> secret, const Nonce& client, const Nonce& server,
                      uint32_t peer_id) {
  Bytes data;
  Append(data, Raw(kAssociationLabel));
  Append(data, client);
  Append(data, server);
  Put(data, peer_id, 4);
  return HmacSha256(secret, data);
}

namespace {
// Observed IPv4[4] || observed port:u16 || count:u8 || candidates || ct, as encoded.
KeyTag PairTag(std::span<const uint8_t> assoc, const Message& info) {
  Bytes bound;
  PutIp(bound, info.observed.ipv4);
  Put(bound, info.observed.port, 2);
  PutCandidates(bound, info.candidates);
  Append(bound, info.wrapped_key);
  return Truncate(LabeledMac(assoc, kPairTagLabel, info.attempt_id, info.ipv4, bound));
}
}  // namespace

void WrapPairKey(std::span<const uint8_t> assoc, std::span<const uint8_t> pair_key, Message& info) {
  auto pad = LabeledMac(assoc, kPairLabel, info.attempt_id, info.ipv4);
  info.wrapped_key = {};
  for (size_t i = 0; i < info.wrapped_key.size() && i < pair_key.size(); ++i)
    info.wrapped_key[i] = pair_key[i] ^ pad[i];
  Wipe(pad);
  info.key_tag = PairTag(assoc, info);
}

std::optional<TokenSecret> UnwrapPairKey(std::span<const uint8_t> assoc, const Message& info) {
  if (info.candidates.size() > kMaxCandidates ||
      !ConstantTimeEqual(PairTag(assoc, info), info.key_tag))
    return std::nullopt;
  auto pad = LabeledMac(assoc, kPairLabel, info.attempt_id, info.ipv4);
  TokenSecret key;
  for (size_t i = 0; i < TokenSecret::kSize; ++i)
    key.data()[i] = info.wrapped_key[i] ^ pad[i];
  Wipe(pad);
  return key;
}

KeyTag CloseTag(std::span<const uint8_t> assoc, uint32_t attempt_id, uint32_t other_ipv4) {
  return Truncate(LabeledMac(assoc, kCloseTagLabel, attempt_id, other_ipv4));
}

namespace {
// The tag of a CANDIDATES or CONNECT_REQUEST whose bytes end with its tag slot.
std::optional<KeyTag> ConnectTag(std::span<const uint8_t> assoc, std::span<const uint8_t> bytes) {
  if (bytes.size() < kHeaderSize + sizeof(KeyTag))
    return std::nullopt;
  std::string_view label;
  if (bytes[5] == static_cast<uint8_t>(Type::kCandidates))
    label = kCandidatesTagLabel;
  else if (bytes[5] == static_cast<uint8_t>(Type::kConnectRequest))
    label = kConnectRequestTagLabel;
  else
    return std::nullopt;
  Bytes data;
  Append(data, Raw(label));
  Append(data, bytes.first(bytes.size() - sizeof(KeyTag)));
  return Truncate(HmacSha256(assoc, data));
}
}  // namespace

bool SignConnect(std::span<const uint8_t> assoc, Message& message) {
  message.key_tag = {};
  const auto bytes = Encode(message);
  if (!bytes)
    return false;
  const auto tag = ConnectTag(assoc, *bytes);
  if (!tag)
    return false;
  message.key_tag = *tag;
  return true;
}

bool VerifyConnect(std::span<const uint8_t> assoc, std::span<const uint8_t> bytes) {
  const auto tag = ConnectTag(assoc, bytes);
  return tag && ConstantTimeEqual(*tag, bytes.last(sizeof(KeyTag)));
}

Digest DirectKey(std::span<const uint8_t> pair_key, uint32_t sender_ipv4) {
  Bytes data;
  Append(data, Raw(kDirectLabel));
  PutIp(data, sender_ipv4);
  return HmacSha256(pair_key, data);
}

namespace {
bool Control(DirectType type) {
  return type != DirectType::kData;
}
bool Acknowledgement(DirectType type) {
  return type == DirectType::kProbeAck || type == DirectType::kKeepaliveAck;
}
}  // namespace

std::expected<Bytes, Reason> EncodeDirect(const DirectFrame& f, std::span<const uint8_t> key) {
  if (f.type < DirectType::kProbe || f.type > DirectType::kKeepaliveAck || !f.attempt_id ||
      !f.sequence || f.payload.size() > kMaxPayload ||
      (Control(f.type) && (Acknowledgement(f.type) != (f.echo != 0) || !f.payload.empty())))
    return std::unexpected(Reason::kMalformed);
  Bytes bytes{'R', 'R', 'D', 'T', kDirectVersion, static_cast<uint8_t>(f.type)};
  bytes.reserve(kMaxDirectDatagram);
  Put(bytes, f.attempt_id, 4);
  PutIp(bytes, f.sender_ipv4);
  Put(bytes, f.sequence, 8);
  if (Control(f.type)) {
    Put(bytes, f.echo, 8);
  } else {
    Put(bytes, f.source_port, 2);
    Put(bytes, f.destination_port, 2);
    Append(bytes, f.payload);
  }
  const auto mac = Truncate(HmacSha256(key, bytes));
  Append(bytes, mac);
  return bytes;
}

std::optional<DirectFrame> ParseDirect(std::span<const uint8_t> bytes) {
  if (bytes.size() < kDirectDataOverhead || bytes.size() > kMaxDirectDatagram ||
      std::memcmp(bytes.data(), "RRDT", 4) || bytes[4] != kDirectVersion ||
      bytes[5] < static_cast<uint8_t>(DirectType::kProbe) ||
      bytes[5] > static_cast<uint8_t>(DirectType::kKeepaliveAck))
    return std::nullopt;
  DirectFrame f;
  f.type = static_cast<DirectType>(bytes[5]);
  Reader in(bytes.first(bytes.size() - kDirectMacSize), 6);
  f.attempt_id = static_cast<uint32_t>(in.Get(4));
  f.sender_ipv4 = in.Ip();
  f.sequence = in.Get(8);
  if (Control(f.type)) {
    if (in.remaining() != 8)
      return std::nullopt;
    f.echo = in.Get(8);
    if (Acknowledgement(f.type) != (f.echo != 0))
      return std::nullopt;
  } else {
    f.source_port = static_cast<uint16_t>(in.Get(2));
    f.destination_port = static_cast<uint16_t>(in.Get(2));
    in.Blob(f.payload, in.remaining());
  }
  if (!f.attempt_id || !f.sequence)
    return std::nullopt;
  return f;
}

bool VerifyDirect(std::span<const uint8_t> bytes, std::span<const uint8_t> key) {
  if (bytes.size() < kDirectDataOverhead)
    return false;
  const auto body = bytes.first(bytes.size() - kDirectMacSize);
  return ConstantTimeEqual(Truncate(HmacSha256(key, body)), bytes.last(kDirectMacSize));
}

bool ReplayWindow::Fresh(uint64_t sequence) const {
  if (!sequence)
    return false;
  if (sequence > highest_)
    return true;
  const uint64_t age = highest_ - sequence;
  return age < kSize && !((seen_ >> age) & 1);
}

void ReplayWindow::Accept(uint64_t sequence) {
  if (sequence > highest_) {
    const uint64_t shift = sequence - highest_;
    seen_ = shift >= kSize ? 0 : seen_ << shift;
    seen_ |= 1;
    highest_ = sequence;
  } else if (highest_ - sequence < kSize) {
    seen_ |= uint64_t{1} << (highest_ - sequence);
  }
}

}  // namespace rex::relay
