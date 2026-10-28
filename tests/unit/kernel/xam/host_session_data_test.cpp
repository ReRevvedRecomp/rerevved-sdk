#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/session_data.h>

#include "core/relay_core.h"
#include "kernel/xam/host_session_data.h"
#include "kernel/xam/lan_system_link.h"
#include "kernel/xam/relay_system_link.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace rex::kernel::xam::host_data {
namespace {
using namespace std::chrono_literals;
using rex::session_data::State;

SessionInfo TestInfo(uint8_t seed = 1) {
  SessionInfo info;
  info.id.fill(seed);
  info.id[0] &= 0x1F;
  info.host_ipv4 = htonl(0xc6120001);
  info.key.fill(static_cast<uint8_t>(seed + 0x40));
  return info;
}

Core::RandomNonce FixedNonce(uint8_t value) {
  return [value] {
    Nonce nonce;
    nonce.fill(value);
    return nonce;
  };
}

Core::Provider Returns(Bytes bytes) {
  return [bytes] { return bytes; };
}

const Route kHostRoute{Link::kRelay, htonl(0xc6120001), 0};
const Route kJoinerRoute{Link::kRelay, htonl(0xc6120002), 0};

TEST_CASE("host session data messages round trip and reject tampering", "[host_data]") {
  const auto info = TestInfo();
  Message request{Type::kRequest, info.id, {}, {}};
  request.nonce.fill(7);
  const auto encoded = Encode(request, info.key);
  REQUIRE(encoded.size() == kRequestSize);
  CHECK(HasMagic(encoded));
  auto decoded = Decode(encoded, info.key);
  REQUIRE(decoded);
  CHECK(decoded->type == Type::kRequest);
  CHECK(decoded->session_id == info.id);
  CHECK(decoded->nonce == request.nonce);
  CHECK(decoded->data.empty());

  Message reply{Type::kReply, info.id, request.nonce, Bytes(rex::session_data::kMaxBytes, 0x5a)};
  const auto reply_bytes = Encode(reply, info.key);
  REQUIRE(reply_bytes.size() == kRequestSize + rex::session_data::kMaxBytes);
  decoded = Decode(reply_bytes, info.key);
  REQUIRE(decoded);
  CHECK(decoded->type == Type::kReply);
  CHECK(decoded->data == reply.data);

  auto game_reply = reply;
  game_reply.type = Type::kGameReply;
  game_reply.game_generation = 0x1020304050607080ull;
  game_reply.requester_id.fill(7);
  game_reply.game_attempt = 0x8070605040302010ull;
  const auto game_bytes = Encode(game_reply, info.key);
  REQUIRE(game_bytes.size() == kMaxMessageSize);
  const auto decoded_game = Decode(game_bytes, info.key);
  REQUIRE(decoded_game);
  CHECK(decoded_game->game_generation == 0x1020304050607080ull);
  CHECK(decoded_game->requester_id == game_reply.requester_id);
  CHECK(decoded_game->game_attempt == 0x8070605040302010ull);
  CHECK(decoded_game->data == reply.data);
  for (size_t i = 0; i < game_bytes.size(); ++i) {
    auto tampered = game_bytes;
    tampered[i] ^= 1;
    CHECK_FALSE(Decode(tampered, info.key));
  }

  // Any changed byte, a different key, truncation or trailing bytes fail.
  for (size_t i = 0; i < encoded.size(); ++i) {
    auto tampered = encoded;
    tampered[i] ^= 0x01;
    CHECK_FALSE(Decode(tampered, info.key));
  }
  CHECK_FALSE(Decode(encoded, TestInfo(2).key));
  CHECK_FALSE(Decode(std::span(encoded).first(encoded.size() - 1), info.key));
  auto trailing = encoded;
  trailing.push_back(0);
  CHECK_FALSE(Decode(trailing, info.key));
  CHECK_FALSE(Decode({}, info.key));

  // Oversized data, data on a request and unknown types do not encode.
  reply.data.push_back(0);
  CHECK(Encode(reply, info.key).empty());
  request.data = {1};
  CHECK(Encode(request, info.key).empty());
  CHECK(Encode({static_cast<Type>(5), info.id, {}, {}}, info.key).empty());
  CHECK_FALSE(HasMagic(Bytes{'R', 'X', 'Q', 'P'}));
}

TEST_CASE("host session data parses XSESSION_INFO fields", "[host_data]") {
  std::array<uint8_t, 60> raw{};
  for (size_t i = 0; i < raw.size(); ++i)
    raw[i] = static_cast<uint8_t>(i);
  const auto info = ParseSessionInfo(raw);
  CHECK(info.id == SessionId{0, 1, 2, 3, 4, 5, 6, 7});
  const std::array<uint8_t, 4> ina{8, 9, 10, 11};
  CHECK(std::memcmp(&info.host_ipv4, ina.data(), ina.size()) == 0);
  CHECK(info.key[0] == 44);
  CHECK(info.key[15] == 59);
}

TEST_CASE("host session data freezes and truncates host bytes", "[host_data]") {
  Core core;
  CHECK(core.Current().state == State::kNone);
  core.SetProvider(Returns({1, 2, 3}));
  core.HostCreated(TestInfo());
  core.SetProvider(Returns({9}));
  auto status = core.Current();
  CHECK(status.state == State::kHost);
  CHECK(status.bytes == Bytes{1, 2, 3});
  core.Deleted();
  CHECK(core.Current().state == State::kNone);
  CHECK(core.Current().bytes.empty());

  Bytes large(rex::session_data::kMaxBytes + 100);
  for (size_t i = 0; i < large.size(); ++i)
    large[i] = static_cast<uint8_t>(i);
  core.SetProvider(Returns(large));
  core.HostCreated(TestInfo());
  status = core.Current();
  REQUIRE(status.bytes.size() == rex::session_data::kMaxBytes);
  CHECK(std::equal(status.bytes.begin(), status.bytes.end(), large.begin()));

  // No provider freezes no bytes; a host without a session id serves nobody.
  core.SetProvider({});
  core.HostCreated({});
  CHECK(core.Current().state == State::kHost);
  CHECK(core.Current().bytes.empty());
  Core joiner(FixedNonce(3));
  auto request = joiner.JoinCreated({}, kHostRoute, Clock::now());
  REQUIRE(request);
  CHECK_FALSE(core.Receive(kJoinerRoute, request->bytes, Clock::now()));
}

TEST_CASE("host session data refreshes at game start and freezes until game end", "[host_data]") {
  const auto info = TestInfo();
  const auto now = Clock::now();
  Core host;
  Bytes selected{1};
  unsigned captures = 0;
  host.SetProvider([&] {
    ++captures;
    return selected;
  });
  host.HostCreated(info);
  CHECK_FALSE(host.Current().game_ready);
  uint8_t next_nonce = 0;
  Core joiner([&] {
    Nonce nonce{};
    nonce.fill(++next_nonce);
    return nonce;
  });
  auto lobby_request = joiner.JoinCreated(info, kHostRoute, now);
  REQUIRE(lobby_request);
  const auto lobby_reply = host.Receive(kJoinerRoute, lobby_request->bytes, now);
  REQUIRE(lobby_reply);
  joiner.Receive(kHostRoute, lobby_reply->bytes, now);
  CHECK(joiner.Current().bytes == Bytes{1});
  CHECK_FALSE(joiner.Current().game_ready);

  // A joiner starts first and must not freeze stale lobby bytes.
  auto game_request = joiner.BeginGame(now);
  REQUIRE(game_request);
  CHECK(joiner.Current().state == State::kPending);
  CHECK(joiner.Current().bytes.empty());
  CHECK_FALSE(host.Receive(kJoinerRoute, game_request->bytes, now));
  joiner.Receive(kHostRoute, lobby_reply->bytes, now);
  CHECK(joiner.Current().state == State::kPending);
  selected = {2, 3};  // Changed after lobby creation.
  host.BeginGame(now);
  CHECK(host.Current().game_ready);
  CHECK(captures == 2);
  auto reply = host.Receive(kJoinerRoute, game_request->bytes, now);
  REQUIRE(reply);
  auto tampered = reply->bytes;
  tampered[kHeaderSize] ^= 1;
  joiner.Receive(kHostRoute, tampered, now);
  CHECK(joiner.Current().state == State::kPending);
  joiner.Receive(kHostRoute, reply->bytes, now);
  const auto running = joiner.Current();
  CHECK(running.game_ready);
  CHECK(running.bytes == Bytes{2, 3});

  selected = {4};
  host.BeginGame(now);
  CHECK_FALSE(joiner.BeginGame(now));
  CHECK(captures == 2);
  CHECK(host.Current().bytes == Bytes{2, 3});
  joiner.EndGame();
  CHECK_FALSE(joiner.Current().game_ready);
  auto next_request = joiner.BeginGame(now);
  REQUIRE(next_request);
  // The peer can advance before the host ends the old game. Its request for the next
  // generation must not receive the snapshot from the game still running.
  CHECK(host.Current().game_ready);
  CHECK_FALSE(host.Receive(kJoinerRoute, next_request->bytes, now));
  host.EndGame();
  CHECK_FALSE(host.Current().game_ready);
  CHECK_FALSE(host.Receive(kJoinerRoute, next_request->bytes, now));
  joiner.Receive(kHostRoute, reply->bytes, now);
  CHECK(joiner.Current().state == State::kPending);
  host.BeginGame(now);
  auto next_reply = host.Receive(kJoinerRoute, next_request->bytes, now);
  REQUIRE(next_reply);
  joiner.Receive(kHostRoute, next_reply->bytes, now);
  CHECK(joiner.Current().game_ready);
  CHECK(joiner.Current().bytes == Bytes{4});
  CHECK(captures == 3);
  CHECK(running.bytes == Bytes{2, 3});  // Previously frozen consumer remains stable.

  // A late join learns the host's actual generation, even if it missed games.
  Core late(FixedNonce(10));
  REQUIRE(late.JoinCreated(info, kHostRoute, now));
  auto late_request = late.BeginGame(now);
  REQUIRE(late_request);
  auto late_reply = host.Receive(kJoinerRoute, late_request->bytes, now);
  REQUIRE(late_reply);
  late.Receive(kHostRoute, late_reply->bytes, now);
  CHECK(late.Current().game_ready);
  CHECK(late.Current().bytes == Bytes{4});
  late.EndGame();
  auto future_request = late.BeginGame(now);
  REQUIRE(future_request);
  CHECK_FALSE(host.Receive(kJoinerRoute, future_request->bytes, now));
}

TEST_CASE("host session data retires unanswered game generations", "[host_data]") {
  for (const bool timeout : {false, true}) {
    INFO("timeout=" << timeout);
    const auto info = TestInfo();
    const auto now = Clock::now();
    Core host;
    host.SetProvider(Returns({1}));
    host.HostCreated(info);
    host.BeginGame(now);
    Core joiner(FixedNonce(9));
    REQUIRE(joiner.JoinCreated(info, kHostRoute, now));
    auto first_request = joiner.BeginGame(now);
    REQUIRE(first_request);
    auto decoded_first = Decode(first_request->bytes, info.key);
    REQUIRE(decoded_first);
    CHECK(decoded_first->game_generation == 1);
    CHECK_FALSE(joiner.BeginGame(now));
    auto retry = joiner.Due(now + kFirstRetry);
    REQUIRE(retry);
    const auto decoded_retry = Decode(retry->bytes, info.key);
    REQUIRE(decoded_retry);
    CHECK(decoded_retry->game_generation == 1);
    // The host prepared game 1, but its reply never reached the joiner.
    REQUIRE(host.Receive(kJoinerRoute, retry->bytes, now));
    if (timeout) {
      CHECK_FALSE(joiner.Due(now + kGiveUp));
      CHECK(joiner.Current().state == State::kUnavailable);
      CHECK_FALSE(joiner.BeginGame(now + kGiveUp));
    }
    joiner.EndGame();
    joiner.EndGame();  // Duplicate lifecycle end must not retire another game.
    const auto next_time = now + kGiveUp;
    auto next_request = joiner.BeginGame(next_time);
    REQUIRE(next_request);
    const auto decoded_next = Decode(next_request->bytes, info.key);
    REQUIRE(decoded_next);
    CHECK(decoded_next->game_generation == 2);
    CHECK_FALSE(joiner.BeginGame(next_time));
    CHECK_FALSE(host.Receive(kJoinerRoute, next_request->bytes, next_time));
    // Even a correctly authenticated reply using the current nonce cannot
    // satisfy this attempt with the prior game's generation.
    Message stale{
        Type::kGameReply,          info.id, decoded_next->nonce, {1}, 1, decoded_next->requester_id,
        decoded_next->game_attempt};
    joiner.Receive(kHostRoute, Encode(stale, info.key), next_time);
    CHECK(joiner.Current().state == State::kPending);
    CHECK_FALSE(joiner.Current().game_ready);
    host.EndGame();
    host.SetProvider(Returns({2}));
    host.BeginGame(next_time);
    auto fresh_reply = host.Receive(kJoinerRoute, next_request->bytes, next_time);
    REQUIRE(fresh_reply);
    joiner.Receive(kHostRoute, fresh_reply->bytes, next_time);
    CHECK(joiner.Current().game_ready);
    CHECK(joiner.Current().bytes == Bytes{2});
  }
}

TEST_CASE("host session data binds a late joiner's lost reply to its game attempt", "[host_data]") {
  const auto info = TestInfo();
  const auto now = Clock::now();
  Core host;
  host.SetProvider(Returns({5}));
  host.HostCreated(info);
  for (unsigned game = 1; game <= 5; ++game) {
    host.BeginGame(now);
    if (game != 5)
      host.EndGame();
  }
  Core joiner(FixedNonce(11));
  REQUIRE(joiner.JoinCreated(info, kHostRoute, now));
  const auto first_request = joiner.BeginGame(now);
  REQUIRE(first_request);
  const auto lost_reply = host.Receive(kJoinerRoute, first_request->bytes, now);
  REQUIRE(lost_reply);
  const auto decoded_lost = Decode(lost_reply->bytes, info.key);
  REQUIRE(decoded_lost);
  CHECK(decoded_lost->game_generation == 5);
  CHECK(decoded_lost->game_attempt == 1);
  const auto retry = joiner.Due(now + kFirstRetry);
  REQUIRE(retry);
  REQUIRE(host.Receive(kJoinerRoute, retry->bytes, now));
  joiner.EndGame();  // No reply ever reached this late joiner.
  const auto next_request = joiner.BeginGame(now);
  REQUIRE(next_request);
  const auto decoded_next = Decode(next_request->bytes, info.key);
  REQUIRE(decoded_next);
  CHECK(decoded_next->game_generation == 2);
  CHECK(decoded_next->game_attempt == 2);
  CHECK(decoded_next->requester_id == decoded_lost->requester_id);
  CHECK_FALSE(host.Receive(kJoinerRoute, next_request->bytes, now));
  // A delayed reply from attempt 1 uses the same injected nonce, but cannot
  // satisfy attempt 2 because the authenticated attempt ordinal differs.
  joiner.Receive(kHostRoute, lost_reply->bytes, now);
  CHECK(joiner.Current().state == State::kPending);
  host.EndGame();
  host.SetProvider(Returns({6}));
  host.BeginGame(now);
  CHECK_FALSE(host.Receive(kJoinerRoute, first_request->bytes, now));
  auto wrong_route = kJoinerRoute;
  ++wrong_route.ipv4;
  CHECK_FALSE(host.Receive(wrong_route, next_request->bytes, now));
  const auto fresh_reply = host.Receive(kJoinerRoute, next_request->bytes, now);
  REQUIRE(fresh_reply);
  auto different_requester = Decode(fresh_reply->bytes, info.key);
  REQUIRE(different_requester);
  different_requester->requester_id[0] ^= 1;
  joiner.Receive(kHostRoute, Encode(*different_requester, info.key), now);
  CHECK(joiner.Current().state == State::kPending);
  joiner.Receive(kHostRoute, fresh_reply->bytes, now);
  CHECK(joiner.Current().game_ready);
  CHECK(joiner.Current().bytes == Bytes{6});
  CHECK_FALSE(joiner.BeginGame(now));
  joiner.EndGame();
  const auto third_request = joiner.BeginGame(now);
  REQUIRE(third_request);
  CHECK_FALSE(host.Receive(kJoinerRoute, third_request->bytes, now));
  host.EndGame();
  host.SetProvider(Returns({7}));
  host.BeginGame(now);
  const auto third_retry = joiner.Due(now + kFirstRetry);
  REQUIRE(third_retry);
  const auto third_reply = host.Receive(kJoinerRoute, third_retry->bytes, now);
  REQUIRE(third_reply);
  joiner.Receive(kHostRoute, third_reply->bytes, now);
  CHECK(joiner.Current().bytes == Bytes{7});
}

TEST_CASE("host session data refuses attempts whose earlier requests never arrived",
          "[host_data]") {
  const auto info = TestInfo();
  const auto now = Clock::now();
  Core host;
  host.HostCreated(info);
  host.BeginGame(now);
  Core joiner(FixedNonce(12));
  REQUIRE(joiner.JoinCreated(info, kHostRoute, now));
  REQUIRE(joiner.BeginGame(now));  // Discard every request from attempt 1.
  joiner.EndGame();
  const auto request = joiner.BeginGame(now);
  REQUIRE(request);
  const auto decoded = Decode(request->bytes, info.key);
  REQUIRE(decoded);
  CHECK(decoded->game_attempt == 2);
  CHECK_FALSE(host.Receive(kJoinerRoute, request->bytes, now));
  host.EndGame();
  host.BeginGame(now);
  const auto retry = joiner.Due(now + kFirstRetry);
  REQUIRE(retry);
  CHECK_FALSE(host.Receive(kJoinerRoute, retry->bytes, now));
  CHECK_FALSE(joiner.Due(now + kGiveUp));
  CHECK(joiner.Current().state == State::kUnavailable);
  CHECK_FALSE(joiner.Current().game_ready);
}

TEST_CASE("host session data retains bounded requester history until session deletion",
          "[host_data]") {
  const auto info = TestInfo();
  const auto start = Clock::now();
  Core host;
  host.HostCreated(info);
  host.BeginGame(start);
  Message request{Type::kGameRequest, info.id, {}, {}, 1, {}, 1};
  auto now = start;
  for (size_t index = 1; index <= kMaxGameRequesters; ++index) {
    request.nonce.fill(static_cast<uint8_t>(index));
    request.requester_id.fill(static_cast<uint8_t>(index));
    now += 1s;
    REQUIRE(host.Receive(kJoinerRoute, Encode(request, info.key), now));
  }
  request.nonce.fill(100);
  request.requester_id.fill(100);
  CHECK_FALSE(host.Receive(kJoinerRoute, Encode(request, info.key), now));
  request.requester_id.fill(1);
  REQUIRE(host.Receive(kJoinerRoute, Encode(request, info.key), now));
  host.EndGame();
  host.BeginGame(now);
  CHECK_FALSE(host.Receive(kJoinerRoute, Encode(request, info.key),
                           now));  // Old attempt cannot take a new game.
  request.game_generation = 2;
  request.game_attempt = 3;
  CHECK_FALSE(
      host.Receive(kJoinerRoute, Encode(request, info.key), now));  // No attempt 2 was served.
  request.game_attempt = 2;
  REQUIRE(host.Receive(kJoinerRoute, Encode(request, info.key), now));
  request.requester_id.fill(100);
  request.game_attempt = 1;
  CHECK_FALSE(host.Receive(kJoinerRoute, Encode(request, info.key),
                           now));  // Refresh did not evict history.
  host.Deleted();
  host.HostCreated(info);
  host.BeginGame(now);
  request.game_generation = 1;
  REQUIRE(host.Receive(kJoinerRoute, Encode(request, info.key), now));
}

TEST_CASE("host session data discards game capture replaced by session end", "[host_data]") {
  Core host;
  host.HostCreated(TestInfo());
  host.SetProvider([&] {
    host.EndGame();
    return Bytes{9};
  });
  host.BeginGame(Clock::now());
  CHECK_FALSE(host.Current().game_ready);
  CHECK(host.Current().bytes.empty());
  host.SetProvider(Returns({7}));
  host.BeginGame(Clock::now());
  CHECK(host.Current().game_ready);
  CHECK(host.Current().bytes == Bytes{7});
}

TEST_CASE("host session data moves a joiner from pending to received", "[host_data]") {
  const auto info = TestInfo();
  Core host;
  host.SetProvider(Returns({0xaa, 0xbb}));
  host.HostCreated(info);
  Core joiner(FixedNonce(5));
  const auto now = Clock::now();
  auto request = joiner.JoinCreated(info, kHostRoute, now);
  REQUIRE(request);
  CHECK(request->route == kHostRoute);
  CHECK(joiner.Current().state == State::kPending);
  CHECK(joiner.NextDeadline() == now + kFirstRetry);

  // A joiner never answers requests and a host never takes replies.
  CHECK_FALSE(joiner.Receive(kJoinerRoute, request->bytes, Clock::now()));
  auto reply = host.Receive(kJoinerRoute, request->bytes, Clock::now());
  REQUIRE(reply);
  CHECK(reply->route == kJoinerRoute);
  CHECK_FALSE(host.Receive(kJoinerRoute, reply->bytes, Clock::now()));
  CHECK(host.Current().state == State::kHost);

  // Replies for another nonce or session, or under another key, are dropped.
  Core stranger(FixedNonce(6));
  auto other_nonce =
      host.Receive(kJoinerRoute, stranger.JoinCreated(info, kHostRoute, now)->bytes, Clock::now());
  REQUIRE(other_nonce);
  CHECK_FALSE(joiner.Receive(kHostRoute, other_nonce->bytes, Clock::now()));
  Message wrong_session{Type::kReply, TestInfo(2).id, Nonce{}, {1}};
  wrong_session.nonce.fill(5);
  CHECK_FALSE(joiner.Receive(kHostRoute, Encode(wrong_session, info.key), Clock::now()));
  Message wrong_key{Type::kReply, info.id, wrong_session.nonce, {1}};
  CHECK_FALSE(joiner.Receive(kHostRoute, Encode(wrong_key, TestInfo(2).key), Clock::now()));
  CHECK(joiner.Current().state == State::kPending);

  CHECK_FALSE(joiner.Receive(kHostRoute, reply->bytes, Clock::now()));
  const auto status = joiner.Current();
  CHECK(status.state == State::kReceived);
  CHECK(status.bytes == Bytes{0xaa, 0xbb});
  CHECK_FALSE(joiner.NextDeadline());
  CHECK_FALSE(joiner.Due(now + 1h));
  // A duplicate reply changes nothing.
  CHECK_FALSE(joiner.Receive(kHostRoute, reply->bytes, Clock::now()));
  CHECK(joiner.Current().bytes == Bytes{0xaa, 0xbb});
}

TEST_CASE("host session data retries with backoff and gives up", "[host_data]") {
  Core joiner(FixedNonce(1));
  const auto start = Clock::now();
  REQUIRE(joiner.JoinCreated(TestInfo(), kHostRoute, start));
  CHECK_FALSE(joiner.Due(start + kFirstRetry - 1ms));
  std::vector<std::chrono::milliseconds> sent;
  for (auto t = 0ms; t < kGiveUp; t += 1ms) {
    if (auto retry = joiner.Due(start + t)) {
      CHECK(retry->route == kHostRoute);
      CHECK(retry->bytes.size() == kRequestSize);
      sent.push_back(t);
    }
  }
  const std::vector<std::chrono::milliseconds> expected{250ms,  750ms,  1750ms,  3750ms, 5750ms,
                                                        7750ms, 9750ms, 11750ms, 13750ms};
  CHECK(sent == expected);
  CHECK(joiner.Current().state == State::kPending);
  CHECK(joiner.NextDeadline() == start + kGiveUp);
  CHECK_FALSE(joiner.Due(start + kGiveUp));
  CHECK(joiner.Current().state == State::kUnavailable);
  CHECK_FALSE(joiner.NextDeadline());
}

TEST_CASE("host session data resets on delete and without a route", "[host_data]") {
  const auto info = TestInfo();
  Core host;
  host.SetProvider(Returns({1}));
  host.HostCreated(info);
  Core joiner(FixedNonce(2));
  auto request = joiner.JoinCreated(info, kHostRoute, Clock::now());
  REQUIRE(request);
  auto reply = host.Receive(kJoinerRoute, request->bytes, Clock::now());
  REQUIRE(reply);
  joiner.Deleted();
  CHECK(joiner.Current().state == State::kNone);
  CHECK_FALSE(joiner.Receive(kHostRoute, reply->bytes, Clock::now()));
  CHECK(joiner.Current().state == State::kNone);

  host.Deleted();
  CHECK_FALSE(host.Receive(kJoinerRoute, request->bytes, Clock::now()));

  CHECK_FALSE(joiner.JoinCreated(info, {}, Clock::now()));
  CHECK(joiner.Current().state == State::kUnavailable);
  CHECK_FALSE(joiner.JoinCreated(info, {Link::kLan, 0, kLanPort}, Clock::now()));
  CHECK(joiner.Current().state == State::kUnavailable);
}

TEST_CASE("host session data wait returns at once unless pending", "[host_data]") {
  Core core(FixedNonce(4));
  auto start = Clock::now();
  CHECK(core.Wait(5s).state == State::kNone);
  core.HostCreated(TestInfo());
  CHECK(core.Wait(5s).state == State::kHost);
  CHECK(Clock::now() - start < 1s);

  REQUIRE(core.JoinCreated(TestInfo(), kHostRoute, Clock::now()));
  start = Clock::now();
  CHECK(core.Wait(100ms).state == State::kPending);
  const auto waited = Clock::now() - start;
  CHECK(waited >= 90ms);
  CHECK(waited < 2s);

  // A reply on another thread ends the wait early.
  Core host;
  host.SetProvider(Returns({7}));
  host.HostCreated(TestInfo());
  auto request = core.JoinCreated(TestInfo(), kHostRoute, Clock::now());
  REQUIRE(request);
  auto reply = host.Receive(kJoinerRoute, request->bytes, Clock::now());
  REQUIRE(reply);
  std::thread deliver([&] {
    std::this_thread::sleep_for(50ms);
    core.Receive(kHostRoute, reply->bytes, Clock::now());
  });
  start = Clock::now();
  const auto status = core.Wait(10s);
  deliver.join();
  CHECK(status.state == State::kReceived);
  CHECK(status.bytes == Bytes{7});
  CHECK(Clock::now() - start < 5s);

  // Delete and give-up also end a wait.
  REQUIRE(core.JoinCreated(TestInfo(), kHostRoute, Clock::now()));
  std::thread remove([&] {
    std::this_thread::sleep_for(50ms);
    core.Deleted();
  });
  CHECK(core.Wait(10s).state == State::kNone);
  remove.join();
  const auto joined = Clock::now();
  REQUIRE(core.JoinCreated(TestInfo(), kHostRoute, joined));
  std::thread expire([&] {
    std::this_thread::sleep_for(50ms);
    core.Due(joined + kGiveUp);
  });
  CHECK(core.Wait(10s).state == State::kUnavailable);
  expire.join();
}

TEST_CASE("host session data discards a provider result for a replaced session", "[host_data]") {
  Core core(FixedNonce(1));
  // A delete while the provider runs wins over the provider's result.
  core.SetProvider([&] {
    core.Deleted();
    return Bytes{1};
  });
  core.HostCreated(TestInfo());
  CHECK(core.Current().state == State::kNone);
  CHECK(core.Current().bytes.empty());

  // So does a newer create, joiner or host.
  core.SetProvider([&] {
    core.JoinCreated(TestInfo(2), kHostRoute, Clock::now());
    return Bytes{2};
  });
  core.HostCreated(TestInfo());
  CHECK(core.Current().state == State::kPending);
  int depth = 0;
  core.SetProvider([&] {
    if (depth++ == 0)
      core.HostCreated(TestInfo(2));
    return Bytes{static_cast<uint8_t>(depth)};
  });
  core.HostCreated(TestInfo());
  CHECK(core.Current().state == State::kHost);
  CHECK(core.Current().bytes == Bytes{2});

  // The provider sees kNone, never the previous session's bytes.
  core.SetProvider(Returns({7}));
  core.HostCreated(TestInfo());
  State seen = State::kHost;
  core.SetProvider([&] {
    seen = core.Current().state;
    return Bytes{8};
  });
  core.HostCreated(TestInfo());
  CHECK(seen == State::kNone);
  CHECK(core.Current().bytes == Bytes{8});
}

TEST_CASE("host session data hosts without bytes when the provider throws", "[host_data]") {
  Core host;
  host.SetProvider([]() -> Bytes { throw std::runtime_error("bad settings"); });
  REQUIRE_NOTHROW(host.HostCreated(TestInfo()));
  CHECK(host.Current().state == State::kHost);
  CHECK(host.Current().bytes.empty());
  host.SetProvider([]() -> Bytes { throw 42; });
  REQUIRE_NOTHROW(host.HostCreated(TestInfo()));
  CHECK(host.Current().state == State::kHost);

  // The host still answers, with empty data.
  Core joiner(FixedNonce(1));
  auto request = joiner.JoinCreated(TestInfo(), kHostRoute, Clock::now());
  REQUIRE(request);
  auto reply = host.Receive(kJoinerRoute, request->bytes, Clock::now());
  REQUIRE(reply);
  joiner.Receive(kHostRoute, reply->bytes, Clock::now());
  CHECK(joiner.Current().state == State::kReceived);
  CHECK(joiner.Current().bytes.empty());
}

TEST_CASE("host session data wait ends when its session is replaced", "[host_data]") {
  Core core(FixedNonce(1));
  Core host;
  host.SetProvider(Returns({3}));
  for (int round = 0; round < 5; ++round) {
    host.HostCreated(TestInfo(2));  // forgets the joiner's fixed nonce
    REQUIRE(core.JoinCreated(TestInfo(), kHostRoute, Clock::now()));
    // Delete, then a new session that receives at once: the waiter must not
    // return the new session's data as its own.
    std::thread replace([&] {
      std::this_thread::sleep_for(50ms);
      core.Deleted();
      auto request = core.JoinCreated(TestInfo(2), kHostRoute, Clock::now());
      if (auto reply = host.Receive(kJoinerRoute, request->bytes, Clock::now()))
        core.Receive(kHostRoute, reply->bytes, Clock::now());
    });
    const auto start = Clock::now();
    const auto status = core.Wait(10s);
    replace.join();
    CHECK(status.state == State::kNone);
    CHECK(status.bytes.empty());
    CHECK(Clock::now() - start < 5s);
    CHECK(core.Current().state == State::kReceived);
    CHECK(core.Current().bytes == Bytes{3});
  }
  // A new create while still pending is caught too.
  REQUIRE(core.JoinCreated(TestInfo(), kHostRoute, Clock::now()));
  std::thread rejoin([&] {
    std::this_thread::sleep_for(20ms);
    core.JoinCreated(TestInfo(2), kHostRoute, Clock::now());
  });
  CHECK(core.Wait(10s).state == State::kNone);
  rejoin.join();
  CHECK(core.Current().state == State::kPending);
}

TEST_CASE("host session data answers each request nonce a bounded number of times", "[host_data]") {
  const auto info = TestInfo();
  Core host;
  host.SetProvider(Returns({1}));
  host.HostCreated(info);
  Core joiner(FixedNonce(1));
  auto request = joiner.JoinCreated(info, kHostRoute, Clock::now());
  REQUIRE(request);
  auto now = Clock::now();
  int replies = 0;
  for (int i = 0; i < 10; ++i, now += 1s)
    replies += host.Receive(kJoinerRoute, request->bytes, now) ? 1 : 0;
  CHECK(replies == static_cast<int>(kRepliesPerNonce));

  // A new session forgets the answered nonces.
  host.HostCreated(info);
  CHECK(host.Receive(kJoinerRoute, request->bytes, now));
}

TEST_CASE("host session data rate limits replies across nonces", "[host_data]") {
  const auto info = TestInfo();
  Core host;
  host.SetProvider(Returns({1}));
  host.HostCreated(info);
  uint8_t next = 0;
  auto request = [&] {
    Message message{Type::kRequest, info.id, {}, {}};
    message.nonce.fill(++next);
    return Encode(message, info.key);
  };
  const auto start = Clock::now();
  int replies = 0;
  for (int i = 0; i < 30; ++i)
    replies += host.Receive(kJoinerRoute, request(), start) ? 1 : 0;
  CHECK(replies == static_cast<int>(kRepliesPerSecond));
  CHECK_FALSE(host.Receive(kJoinerRoute, request(), start + 10ms));
  CHECK(host.Receive(kJoinerRoute, request(), start + 100ms));
  replies = 0;
  for (int i = 0; i < 30; ++i)
    replies += host.Receive(kJoinerRoute, request(), start + 2s) ? 1 : 0;
  CHECK(replies == static_cast<int>(kRepliesPerSecond));
  // A clock that goes backward refills nothing.
  CHECK_FALSE(host.Receive(kJoinerRoute, request(), start));
}

TEST_CASE("host session data joiner retries stay answerable", "[host_data]") {
  const auto info = TestInfo();
  Core host;
  host.SetProvider(Returns({5, 6}));
  host.HostCreated(info);
  uint8_t counter = 0;
  Core joiner([&] {
    Nonce nonce;
    nonce.fill(++counter);
    return nonce;
  });
  const auto start = Clock::now();
  std::vector<Outgoing> requests{*joiner.JoinCreated(info, kHostRoute, start)};
  for (auto t = 0ms; t < kGiveUp; t += 1ms)
    if (auto retry = joiner.Due(start + t))
      requests.push_back(*retry);
  REQUIRE(requests.size() == 10);
  // Every request, including every retry, gets a reply although the host
  // answers each nonce only kRepliesPerNonce times.
  std::vector<Outgoing> replies;
  auto now = start;
  for (const auto& request : requests) {
    now += 1s;
    auto reply = host.Receive(kJoinerRoute, request.bytes, now);
    REQUIRE(reply);
    replies.push_back(*reply);
  }
  CHECK(counter == 4);
  // A late reply to the first nonce is still accepted.
  joiner.Receive(kHostRoute, replies.front().bytes, now);
  CHECK(joiner.Current().state == State::kReceived);
  CHECK(joiner.Current().bytes == Bytes{5, 6});
}

TEST_CASE("host session data service stops its timer on shutdown", "[host_data]") {
  std::atomic<int> sent = 0;
  Service service([&](const Outgoing&) {
    ++sent;
    return true;
  });
  service.Join(TestInfo(), kHostRoute);
  CHECK(sent == 1);
  const auto deadline = Clock::now() + 5s;
  while (sent < 2 && Clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  REQUIRE(sent == 2);  // the 250 ms retry
  service.Shutdown();
  service.Shutdown();
  const int after = sent;
  std::this_thread::sleep_for(800ms);  // past the 750 ms retry
  CHECK(sent == after);
  CHECK(service.core().Current().state == State::kPending);
  // Later joins and replies send nothing and start no timer.
  service.Join(TestInfo(), kHostRoute);
  service.Receive(kJoinerRoute, Bytes(kRequestSize));
  CHECK(sent == after);
  CHECK(service.core().Current().state == State::kPending);
}

TEST_CASE("host session data travels over loopback LAN side sockets", "[host_data][network]") {
  const auto info = TestInfo();
  Core host;
  host.SetProvider(Returns({1, 2, 3, 4}));
  host.HostCreated(info);
  Core joiner(FixedNonce(9));
  LanLink* host_link_ptr = nullptr;
  LanLink host_link(0, [&](Route from, std::span<const uint8_t> bytes) {
    if (auto reply = host.Receive(from, bytes, Clock::now()))
      host_link_ptr->Send(reply->route, reply->bytes);
  });
  host_link_ptr = &host_link;
  LanLink joiner_link(0, [&](Route from, std::span<const uint8_t> bytes) {
    joiner.Receive(from, bytes, Clock::now());
  });
  const uint32_t loopback = htonl(INADDR_LOOPBACK);
  host_link.Bind(loopback);
  joiner_link.Bind(loopback);
  const auto bind_deadline = Clock::now() + 5s;
  while ((!host_link.port() || !joiner_link.port()) && Clock::now() < bind_deadline)
    std::this_thread::sleep_for(1ms);
  if (!host_link.port() || !joiner_link.port())
    SKIP("Loopback UDP socket unavailable");

  const Route host_route{Link::kLan, loopback, host_link.port()};
  auto request = joiner.JoinCreated(info, host_route, Clock::now());
  REQUIRE(request);
  REQUIRE(joiner_link.Send(request->route, request->bytes));
  const auto status = joiner.Wait(5s);
  CHECK(status.state == State::kReceived);
  CHECK(status.bytes == Bytes{1, 2, 3, 4});

  // Non-magic and unauthenticated datagrams are dropped without a reply.
  CHECK(joiner_link.Send(host_route, Bytes{'x', 'y', 'z'}));
  Message forged{Type::kRequest, info.id, {}, {}};
  CHECK(joiner_link.Send(host_route, Encode(forged, TestInfo(2).key)));
  CHECK_FALSE(joiner_link.Send({Link::kLan, loopback, 0}, request->bytes));
  joiner_link.Bind(0);
  const auto unbind_deadline = Clock::now() + 5s;
  while (joiner_link.port() && Clock::now() < unbind_deadline)
    std::this_thread::sleep_for(1ms);
  CHECK(joiner_link.port() == 0);
  CHECK_FALSE(joiner_link.Send(host_route, request->bytes));
}

TEST_CASE("host session data travels over the relay side channel",
          "[host_data][relay_client][network]") {
  auto server = relay::UdpSocket::Open("127.0.0.1", 0);
  if (!server)
    SKIP("Loopback UDP socket unavailable");
  const auto endpoint = (*server)->endpoint();
  relay::Core relay_core({}, "secret", relay::Digest{42});
  relay::TransportCounters counters;
  std::jthread pump([&](std::stop_token stop) {
    const auto start = Clock::now();
    while (!stop.stop_requested()) {
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
      relay::Pump(**server, relay_core, now.count(), counters, 10);
    }
  });

  const auto info = TestInfo();
  Core host;
  host.SetProvider(Returns({0x10, 0x20, 0x30}));
  host.HostCreated(info);
  Core joiner(FixedNonce(8));
  relay_link::Client host_client, joiner_client;
  std::atomic<int> host_requests = 0;
  host_client.SetSideChannelHandler([&](uint32_t source, std::span<const uint8_t> bytes) {
    ++host_requests;
    if (auto reply = host.Receive({Link::kRelay, source, 0}, bytes, Clock::now()))
      host_client.SendSideChannel(reply->route.ipv4, reply->bytes);
  });
  joiner_client.SetSideChannelHandler([&](uint32_t source, std::span<const uint8_t> bytes) {
    joiner.Receive({Link::kRelay, source, 0}, bytes, Clock::now());
  });
  REQUIRE(host_client.Start(endpoint, "room", "secret", {}));
  REQUIRE(joiner_client.Start(endpoint, "room", "secret", {}));

  const Route host_route{Link::kRelay, host_client.ipv4(), 0};
  auto request = joiner.JoinCreated(info, host_route, Clock::now());
  REQUIRE(request);
  // Only magic payloads to a virtual unicast address within the payload limit are sent.
  CHECK_FALSE(joiner_client.SendSideChannel(0xffffffffu, request->bytes));
  CHECK_FALSE(joiner_client.SendSideChannel(htonl(INADDR_LOOPBACK), request->bytes));
  CHECK_FALSE(joiner_client.SendSideChannel(host_route.ipv4, Bytes{1, 2, 3, 4}));
  Bytes oversized(relay::kMaxPayload + 1);
  std::copy(kMagic.begin(), kMagic.end(), oversized.begin());
  CHECK_FALSE(joiner_client.SendSideChannel(host_route.ipv4, oversized));
  REQUIRE(joiner_client.SendSideChannel(request->route.ipv4, request->bytes));
  const auto status = joiner.Wait(5s);
  CHECK(status.state == State::kReceived);
  CHECK(status.bytes == Bytes{0x10, 0x20, 0x30});
  CHECK(host_requests == 1);

  // The side channel never reaches guest ports or the probe logic.
  const auto host_stats = host_client.stats();
  const auto joiner_stats = joiner_client.stats();
  CHECK(host_stats.side_channel_in == 1);
  CHECK(host_stats.side_channel_out == 1);
  CHECK(joiner_stats.side_channel_in == 1);
  CHECK(joiner_stats.side_channel_out == 1);
  CHECK(host_stats.invalid_probes == 0);
  CHECK(joiner_stats.invalid_probes == 0);
  CHECK(host_stats.unknown_port == 0);
  CHECK(joiner_stats.unknown_port == 0);
  CHECK(host_stats.delivered == 0);
  CHECK(joiner_stats.delivered == 0);

  host_client.Stop();
  joiner_client.Stop();
  CHECK_FALSE(joiner_client.SendSideChannel(host_route.ipv4, request->bytes));
  pump.request_stop();
  pump.join();
}

TEST_CASE("host session data hooks follow XGI create and delete without System Link",
          "[host_data]") {
  rex::session_data::SetHostDataProvider([] { return std::vector<uint8_t>{4, 5}; });
  lan::SessionSnapshot session{};
  OnSessionCreate(lan::kSessionCreateHost, session);
  auto status = rex::session_data::Current();
  CHECK(status.state == State::kHost);
  CHECK(status.bytes == Bytes{4, 5});
  OnSessionDelete();
  CHECK(rex::session_data::Current().state == State::kNone);

  OnSessionCreate(0, session);
  const auto start = Clock::now();
  CHECK(rex::session_data::WaitForHostData(5s).state == State::kUnavailable);
  CHECK(Clock::now() - start < 1s);
  OnSessionDelete();
  CHECK(rex::session_data::Current().state == State::kNone);

  // The last XNetCleanup ends a hosted session too.
  OnSessionCreate(lan::kSessionCreateHost, session);
  CHECK(rex::session_data::Current().state == State::kHost);
  OnNetworkCleanup();
  CHECK(rex::session_data::Current().state == State::kNone);
  CHECK(rex::session_data::Current().bytes.empty());
  rex::session_data::SetHostDataProvider({});
}

}  // namespace
}  // namespace rex::kernel::xam::host_data
