#include "relay_core.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <ranges>
#include <cstring>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace rex::relay {
namespace {
uint32_t Address(uint32_t slot) {
  const uint32_t host = 0xc6120000 + slot;
  const std::array<uint8_t, 4> bytes{static_cast<uint8_t>(host >> 24),
                                     static_cast<uint8_t>(host >> 16),
                                     static_cast<uint8_t>(host >> 8), static_cast<uint8_t>(host)};
  uint32_t result;
  std::memcpy(&result, bytes.data(), 4);
  return result;
}
}  // namespace

Core::Core(Config config, std::string secret, Digest seed)
    : config_(config), secret_(std::move(secret)), seed_(seed) {
  if (secret_.empty() || !config.max_rooms || config.max_rooms > kMaxRooms || !config.max_members ||
      config.max_members > kMaxMembers || !config.packets_per_second || !config.bytes_per_second ||
      !config.token_lifetime_seconds) {
    throw std::invalid_argument("Invalid relay configuration");
  }
}

Digest Core::Random() {
  std::array<uint8_t, 8> counter{};
  ++random_counter_;
  for (size_t i = 0; i < counter.size(); ++i)
    counter[i] = static_cast<uint8_t>(random_counter_ >> (8 * i));
  return HmacSha256(seed_, counter);
}

void Core::Count(Drop reason) {
  ++counters_.drops[static_cast<size_t>(reason)];
}

void Core::Count(ConnectDrop reason) {
  ++counters_.connect_drops[static_cast<size_t>(reason)];
}

template <typename Predicate>
void Core::EraseTokens(Predicate predicate) {
  // TokenSecret wipes itself when the map node is destroyed.
  std::erase_if(tokens_, [&](const auto& entry) { return predicate(entry.second); });
}

void Core::RemoveSession(std::map<SessionKey, Session>::iterator session) {
  EraseTokens([&](const Token& token) { return token.session == session->first; });
  sessions_.erase(session);
  ++directory_version_;
}

void Core::Emit(Endpoint destination, const Message& message, std::vector<Datagram>& output) {
  auto encoded = Encode(message);
  if (!encoded)
    return;
  ++counters_.packets_out;
  counters_.bytes_out += encoded->size();
  output.push_back({destination, std::move(*encoded)});
}

void Core::Remove(std::map<Endpoint, Peer>::iterator peer) {
  const auto room = peer->second.room;
  const auto id = peer->second.id;
  const auto ipv4 = peer->second.ipv4;
  peers_.erase(peer);
  // The other member of each pair learns that this peer's direct path is gone.
  for (auto it = attempts_.begin(); it != attempts_.end();) {
    if (it->first.first != id && it->first.second != id) {
      ++it;
      continue;
    }
    const auto other_id = it->first.first == id ? it->first.second : it->first.first;
    const auto other = std::find_if(peers_.begin(), peers_.end(),
                                    [&](const auto& entry) { return entry.second.id == other_id; });
    if (other != peers_.end()) {
      Message close;
      close.type = Type::kConnectClose;
      close.attempt_id = it->second.id;
      close.ipv4 = ipv4;
      close.key_tag = CloseTag(other->second.assoc.bytes(), it->second.id, ipv4);
      Emit(other->first, close, notices_);
    }
    it = attempts_.erase(it);
  }
  const bool empty = std::none_of(peers_.begin(), peers_.end(),
                                  [&](const auto& entry) { return entry.second.room == room; });
  for (auto it = sessions_.lower_bound({room, 0});
       it != sessions_.end() && it->first.first == room;) {
    if (empty || it->second.record.host_peer_id == id)
      RemoveSession(it++);
    else
      ++it;
  }
  if (empty)
    rooms_.erase(room);
}

void Core::Directory(Endpoint source, Peer& peer, const Message& message,
                     std::vector<Datagram>& output) {
  auto emit = [&](Bytes bytes) {
    ++counters_.packets_out;
    counters_.bytes_out += bytes.size();
    output.push_back({source, std::move(bytes)});
  };
  auto reply = [&](const Message& response) {
    auto encoded = Encode(response);
    if (encoded)
      emit(std::move(*encoded));
  };
  auto status = [&](Status value) {
    Message response;
    response.type = Type::kStatus;
    response.request_id = message.request_id;
    response.status = value;
    reply(response);
  };
  auto results = [&](const std::vector<SessionRecord>& records) {
    auto pages = EncodeResults(message.request_id, directory_version_, records);
    if (pages)
      for (auto& page : *pages)
        emit(std::move(page));
  };
  const SessionKey key{peer.room, message.type == Type::kPublish ? message.record.session_id
                                                                 : message.session_id};
  const auto session = sessions_.find(key);
  const bool exists = session != sessions_.end();
  const bool owned = exists && session->second.record.host_peer_id == peer.id;
  // Updates apply in client order; a stale retransmission is acknowledged, not applied.
  const bool update = message.type == Type::kPublish || message.type == Type::kRemove;
  // Serial-number order (RFC 1982 style) keeps comparisons valid across wrap.
  if (update && static_cast<int32_t>(message.sequence - peer.last_update) <= 0) {
    status(message.sequence == peer.last_update ? peer.last_update_status : Status::kOk);
    return;
  }
  auto finish = [&](Status value) {
    peer.last_update = message.sequence;
    peer.last_update_status = value;
    status(value);
  };
  auto room_sessions = [&] {
    return std::ranges::subrange(sessions_.lower_bound({peer.room, 0}),
                                 sessions_.upper_bound({peer.room, UINT64_MAX}));
  };
  switch (message.type) {
    case Type::kPublish: {
      if (exists && !owned) {
        Count(Drop::kSessionConflict);
        finish(Status::kConflict);
        return;
      }
      if (!exists) {
        size_t in_room = 0, by_peer = 0;
        for (const auto& [_, entry] : room_sessions()) {
          ++in_room;
          by_peer += entry.record.host_peer_id == peer.id;
        }
        if (by_peer >= kMaxSessionsPerPeer || in_room >= kMaxSessionsPerRoom) {
          Count(Drop::kSessionLimit);
          finish(Status::kLimit);
          return;
        }
      }
      auto record = message.record;
      record.host_peer_id = peer.id;
      record.host_ipv4 = peer.ipv4;
      sessions_.insert_or_assign(key, Session{std::move(record), ++next_sequence_});
      ++directory_version_;
      finish(Status::kOk);
      return;
    }
    case Type::kRemove:
      if (exists && !owned) {
        Count(Drop::kSessionConflict);
        finish(Status::kConflict);
        return;
      }
      if (owned)
        RemoveSession(session);
      finish(Status::kOk);
      return;
    case Type::kSearch: {
      std::vector<const Session*> matches;
      for (const auto& [_, entry] : room_sessions()) {
        const auto& record = entry.record;
        if (record.host_peer_id != peer.id &&
            (record.flags & message.required_flags) == message.required_flags &&
            record.open_public > 0)
          matches.push_back(&entry);
      }
      std::ranges::sort(matches, std::greater{}, &Session::sequence);
      if (matches.size() > message.max_results)
        matches.resize(message.max_results);
      std::vector<SessionRecord> records;
      records.reserve(matches.size());
      for (const auto* match : matches)
        records.push_back(match->record);
      results(records);
      return;
    }
    case Type::kLookup:
      results(exists ? std::vector<SessionRecord>{session->second.record}
                     : std::vector<SessionRecord>{});
      return;
    case Type::kTokenRequest: {
      if (!owned) {
        Count(Drop::kTokenDenied);
        status(Status::kNotFound);
        return;
      }
      Message grant;
      grant.type = Type::kTokenGrant;
      grant.request_id = message.request_id;
      // A retried request receives its original grant instead of a second token.
      for (const auto& [id, token] : tokens_) {
        if (token.peer_id == peer.id && token.request_id == message.request_id &&
            token.session == key) {
          grant.join_token = id;
          grant.token_secret = token.secret;
          grant.expiry_seconds = static_cast<uint32_t>((token.expires - now_ + 999) / 1000);
          reply(grant);
          return;
        }
      }
      const auto live = std::ranges::count_if(
          tokens_, [&](const auto& entry) { return entry.second.session == key; });
      if (static_cast<size_t>(live) >= kMaxTokensPerSession) {
        Count(Drop::kTokenLimit);
        status(Status::kLimit);
        return;
      }
      do {
        const auto random = Random();
        std::copy_n(random.begin(), grant.join_token.size(), grant.join_token.begin());
      } while (tokens_.contains(grant.join_token));
      auto random = Random();
      grant.token_secret = TokenSecret(random);
      SecureWipe(random);
      grant.expiry_seconds = config_.token_lifetime_seconds;
      tokens_.emplace(grant.join_token,
                      Token{key, grant.token_secret,
                            now_ + uint64_t(config_.token_lifetime_seconds) * 1000, peer.id,
                            message.request_id});
      reply(grant);
      return;
    }
    default:
      return;
  }
}

void Core::Tick(uint64_t now) {
  now_ = std::max(now_, now);
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (now_ - it->second.created >= 5000) {
      it = pending_.erase(it);
      Count(Drop::kExpiredChallenge);
    } else
      ++it;
  }
  EraseTokens([&](const Token& token) { return now_ >= token.expires; });
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (it->second.join_reply && now_ - it->second.join_reply->created >= 5000)
      it->second.join_reply.reset();
    if (now_ - it->second.last_seen >= 15000) {
      Remove(it++);
      Count(Drop::kIdle);
    } else
      ++it;
  }
}

std::vector<Datagram> Core::TakeNotices() {
  return std::exchange(notices_, {});
}

void Core::Connect(Endpoint source, Peer& peer, const Message& message,
                   std::vector<Datagram>& output) {
  peer.direct = true;
  if (message.type == Type::kCandidates) {
    peer.candidates = message.candidates;
    return;
  }
  const auto target = std::find_if(peers_.begin(), peers_.end(), [&](const auto& entry) {
    return entry.first != source && entry.second.room == peer.room &&
           entry.second.ipv4 == message.ipv4;
  });
  if (target == peers_.end() || !target->second.direct) {
    Count(ConnectDrop::kUnavailable);
    return;
  }
  const std::pair<uint32_t, uint32_t> key = std::minmax(peer.id, target->second.id);
  auto attempt = attempts_.find(key);
  if (attempt == attempts_.end() || now_ - attempt->second.created >= kAttemptReuseMs) {
    if (attempt == attempts_.end() && attempts_.size() >= kMaxAttempts) {
      Count(ConnectDrop::kLimit);
      return;
    }
    do
      ++next_attempt_id_;
    while (!next_attempt_id_);
    auto random = Random();
    attempt =
        attempts_.insert_or_assign(key, Attempt{next_attempt_id_, TokenSecret(random), now_}).first;
    SecureWipe(random);
  }
  // Each recipient learns the other peer's endpoints and the pair key under its own K_assoc.
  auto inform = [&](Endpoint to, const Peer& recipient, Endpoint other_endpoint,
                    const Peer& other) {
    Message info;
    info.type = Type::kConnectInfo;
    info.attempt_id = attempt->second.id;
    info.ipv4 = other.ipv4;
    info.observed = {other_endpoint.ipv4, other_endpoint.port};
    info.candidates = other.candidates;
    WrapPairKey(recipient.assoc.bytes(), attempt->second.key.bytes(), info);
    Emit(to, info, output);
  };
  inform(source, peer, target->first, target->second);
  inform(target->first, target->second, source, peer);
}

Counters Core::counters() const {
  auto result = counters_;
  result.rooms = rooms_.size();
  result.peers = peers_.size();
  result.pending = pending_.size();
  result.sessions = sessions_.size();
  result.tokens = tokens_.size();
  result.attempts = attempts_.size();
  return result;
}

std::vector<Datagram> Core::Input(Endpoint source, std::span<const uint8_t> bytes, uint64_t now) {
  auto output = Receive(source, bytes, now);
  for (auto& notice : notices_)
    output.push_back(std::move(notice));
  notices_.clear();
  return output;
}

std::vector<Datagram> Core::Receive(Endpoint source, std::span<const uint8_t> bytes, uint64_t now) {
  Tick(now);
  ++counters_.packets_in;
  counters_.bytes_in += bytes.size();
  std::vector<Datagram> output;
  auto send = [&](Endpoint destination, const Message& message) {
    auto encoded = Encode(message);
    if (encoded && encoded->size() <= bytes.size() && output.size() < kMaxMembers) {
      ++counters_.packets_out;
      counters_.bytes_out += encoded->size();
      output.push_back({destination, std::move(*encoded)});
    }
  };
  auto reject = [&](Reason reason) {
    Count(static_cast<Drop>(static_cast<size_t>(reason) - 1));
    Message response;
    response.type = Type::kReject;
    response.reason = reason;
    send(source, response);
  };
  auto decoded = Decode(bytes);
  if (!decoded) {
    Count(decoded.error() == Reason::kVersion ? Drop::kVersion : Drop::kMalformed);
    // A well-formed v1 HELLO gets a v1 REJECT so an old client reports the version.
    if (decoded.error() == Reason::kVersion && bytes[4] == 1 &&
        bytes[5] == static_cast<uint8_t>(Type::kHello) && bytes.size() >= 58 &&
        bytes[24] >= 1 && bytes[24] <= 32 && bytes.size() == size_t(57 + bytes[24])) {
      Bytes reject{'R', 'R', 'L', 'Y', 1, static_cast<uint8_t>(Type::kReject), 9, 0,
                   static_cast<uint8_t>(Reason::kVersion)};
      ++counters_.packets_out;
      counters_.bytes_out += reject.size();
      output.push_back({source, std::move(reject)});
    }
    return output;
  }
  const auto& message = *decoded;
  if (message.type == Type::kHello) {
    auto found = pending_.find(source);
    if (found != pending_.end() && found->second.hello.nonce == message.nonce &&
        found->second.hello.room == message.room &&
        found->second.hello.token_join == message.token_join &&
        found->second.hello.join_token == message.join_token &&
        found->second.hello.fingerprint == message.fingerprint) {
      Message challenge;
      challenge.type = Type::kChallenge;
      challenge.nonce = found->second.server;
      send(source, challenge);
      return output;
    }
    const auto source_pending =
        std::count_if(pending_.begin(), pending_.end(),
                      [&](const auto& entry) { return entry.first.ipv4 == source.ipv4; });
    if (found == pending_.end() &&
        (pending_.size() >= kMaxPending || source_pending >= kMaxPendingPerIp)) {
      Count(Drop::kRateLimited);
      return output;
    }
    Message challenge;
    challenge.type = Type::kChallenge;
    const auto random = Random();
    std::copy_n(random.begin(), challenge.nonce.size(), challenge.nonce.begin());
    const auto created = found == pending_.end() ? now_ : found->second.created;
    const auto attempts = found == pending_.end() ? 0 : found->second.failed_joins;
    pending_.insert_or_assign(source, Pending{message, challenge.nonce, created, attempts});
    send(source, challenge);
    return output;
  }
  if (message.type == Type::kJoin) {
    const auto peer = peers_.find(source);
    if (peer != peers_.end() && peer->second.join_reply &&
        ConstantTimeEqual(message.auth, peer->second.join_reply->auth)) {
      send(source, peer->second.join_reply->welcome);
      return output;
    }
    auto found = pending_.find(source);
    if (found == pending_.end()) {
      reject(Reason::kAuth);
      return output;
    }
    const auto& pending = found->second;
    auto failed_join = [&](Reason reason) {
      if (++found->second.failed_joins >= kMaxJoinAttempts)
        pending_.erase(found);
      reject(reason);
    };
    const auto& hello = pending.hello;
    // A token join takes its room from a live token and proves the token secret instead.
    std::string room_name = hello.room;
    bool authentic = false;
    TokenSecret token_key;  // the token join's HMAC key, kept to derive K_assoc
    if (hello.token_join) {
      const auto token = tokens_.find(hello.join_token);
      if (token != tokens_.end() && now_ < token->second.expires) {
        room_name = token->second.session.first;
        token_key = token->second.secret;
        authentic = ConstantTimeEqual(
            message.auth, TokenJoinAuth(token->second.secret.bytes(), hello.nonce, pending.server,
                                        hello.join_token, hello.fingerprint));
      }
    } else {
      authentic = ConstantTimeEqual(
          message.auth,
          JoinAuth(secret_, hello.nonce, pending.server, hello.room, hello.fingerprint));
    }
    if (!authentic) {
      failed_join(Reason::kAuth);
      return output;
    }
    auto room = rooms_.find(room_name);
    const auto members = std::count_if(peers_.begin(), peers_.end(), [&](const auto& entry) {
      return entry.first != source && entry.second.room == room_name;
    });
    if (members && room->second.fingerprint != hello.fingerprint) {
      failed_join(Reason::kFingerprintMismatch);
      return output;
    }
    const auto old_peer = peers_.find(source);
    const bool releases_room =
        old_peer != peers_.end() &&
        std::none_of(peers_.begin(), peers_.end(), [&](const auto& entry) {
          return entry.first != source && entry.second.room == old_peer->second.room;
        });
    if (!members && rooms_.size() - size_t(releases_room) >= config_.max_rooms) {
      failed_join(Reason::kTooManyRooms);
      return output;
    }
    if (static_cast<size_t>(members) >= config_.max_members) {
      failed_join(Reason::kRoomFull);
      return output;
    }
    if (old_peer != peers_.end())
      Remove(old_peer);
    uint32_t slot = 1;
    while (std::any_of(peers_.begin(), peers_.end(), [&](const auto& entry) {
      return entry.second.room == room_name && entry.second.ipv4 == Address(slot);
    }))
      ++slot;
    uint64_t token;
    do {
      const auto random = Random();
      token = 0;
      for (size_t i = 0; i < 8; ++i)
        token |= uint64_t(random[i]) << (8 * i);
    } while (!token || std::any_of(peers_.begin(), peers_.end(),
                                   [&](const auto& entry) { return entry.second.token == token; }));
    do {
      ++next_peer_id_;
    } while (!next_peer_id_ || std::any_of(peers_.begin(), peers_.end(), [&](const auto& entry) {
      return entry.second.id == next_peer_id_;
    }));
    rooms_.try_emplace(room_name, Room{hello.fingerprint});
    Message welcome;
    welcome.type = Type::kWelcome;
    welcome.peer_id = next_peer_id_;
    welcome.token = token;
    welcome.ipv4 = Address(slot);
    auto joined = peers_.emplace(
        source, Peer{next_peer_id_, token, Address(slot), room_name, now_, now_,
                     double(config_.packets_per_second), double(config_.bytes_per_second),
                     double(kQueriesPerSecond), double(kUpdatesPerSecond),
                     Peer::JoinReply{message.auth, welcome, now_}});
    auto assoc = AssociationKey(
        hello.token_join ? token_key.bytes()
                         : std::span<const uint8_t>(
                               reinterpret_cast<const uint8_t*>(secret_.data()), secret_.size()),
        hello.nonce, pending.server, next_peer_id_);
    joined.first->second.assoc = TokenSecret(assoc);
    SecureWipe(assoc);
    pending_.erase(found);
    send(source, welcome);
    return output;
  }
  const bool directory = message.type == Type::kPublish || message.type == Type::kRemove ||
                         message.type == Type::kSearch || message.type == Type::kLookup ||
                         message.type == Type::kTokenRequest;
  const bool connect = message.type == Type::kCandidates || message.type == Type::kConnectRequest;
  if (message.type != Type::kData && message.type != Type::kHeartbeat &&
      message.type != Type::kBye && !directory && !connect) {
    Count(Drop::kDirection);
    return output;
  }
  auto found = peers_.find(source);
  if (found == peers_.end() || found->second.token != message.token) {
    Count(Drop::kAuth);
    return output;
  }
  auto& peer = found->second;
  if (message.type == Type::kBye) {
    Remove(found);
    return output;
  }
  // Authenticated traffic proves the client received its WELCOME.
  peer.join_reply.reset();
  const double elapsed = double(now_ - peer.refill) / 1000.0;
  peer.packets = std::min(double(config_.packets_per_second),
                          peer.packets + elapsed * config_.packets_per_second);
  peer.queries = std::min(double(kQueriesPerSecond), peer.queries + elapsed * kQueriesPerSecond);
  peer.updates = std::min(double(kUpdatesPerSecond), peer.updates + elapsed * kUpdatesPerSecond);
  peer.connects =
      std::min(double(kConnectsPerSecond), peer.connects + elapsed * kConnectsPerSecond);
  peer.bytes =
      std::min(double(config_.bytes_per_second), peer.bytes + elapsed * config_.bytes_per_second);
  peer.refill = now_;
  if (peer.packets < 1 || peer.bytes < bytes.size()) {
    Count(Drop::kRateLimited);
    return output;
  }
  peer.packets -= 1;
  peer.bytes -= bytes.size();
  if (directory) {
    auto& bucket = message.type == Type::kSearch || message.type == Type::kLookup ? peer.queries
                                                                                   : peer.updates;
    if (bucket < 1) {
      Count(Drop::kDirectoryRateLimited);
      return output;
    }
    bucket -= 1;
    peer.last_seen = now_;
    Directory(source, peer, message, output);
    return output;
  }
  if (connect) {
    // After the packet bucket, so forged requests cannot drain the connect bucket.
    if (!VerifyConnect(peer.assoc.bytes(), bytes)) {
      Count(ConnectDrop::kAuth);
      return output;
    }
    if (peer.connects < 1) {
      Count(ConnectDrop::kRateLimited);
      return output;
    }
    peer.connects -= 1;
    peer.last_seen = now_;
    Connect(source, peer, message, output);
    return output;
  }
  if (message.type == Type::kHeartbeat) {
    peer.last_seen = now_;
    Message ack;
    ack.type = Type::kAck;
    ack.token = peer.token;
    send(source, ack);
    return output;
  }
  const bool broadcast = message.ipv4 == 0xffffffff;
  Message delivered;
  delivered.type = Type::kDeliver;
  delivered.ipv4 = peer.ipv4;
  delivered.source_port = message.source_port;
  delivered.destination_port = message.destination_port;
  delivered.broadcast = broadcast;
  delivered.payload = message.payload;
  bool destination_exists = broadcast;
  for (const auto& [endpoint, target] : peers_) {
    if ((!broadcast || endpoint != source) && target.room == peer.room &&
        (broadcast || target.ipv4 == message.ipv4)) {
      destination_exists = true;
      send(endpoint, delivered);
    }
  }
  if (destination_exists)
    peer.last_seen = now_;
  else
    Count(Drop::kDestination);
  return output;
}

}  // namespace rex::relay
