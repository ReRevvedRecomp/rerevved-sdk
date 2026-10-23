#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/ostream_sink.h>

#include <rex/logging.h>

#include "discord/discord_rpc_payload.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

using namespace rex::discord_rpc;
using namespace rex::discord_rpc::detail;

namespace {
Presence MenuPresence() {
  Presence presence;
  presence.details_ = "In \"Menu\"";
  presence.large_image_key_ = "logo";
  return presence;
}
}  // namespace

TEST_CASE("Discord activity without a party keeps the presence shape", "[discord]") {
  const auto payload = ActivityPayload(MenuPresence(), {}, 1700000000, 42, "7");
  CHECK(payload ==
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":{\"details\":\"In "
        "\\\"Menu\\\"\",\"timestamps\":{\"start\":1700000000},\"assets\":{\"large_image\":"
        "\"logo\"},\"instance\":false}},\"nonce\":\"7\"}");
  CHECK(payload.find("party") == std::string::npos);
  CHECK(payload.find("secrets") == std::string::npos);
  CHECK(ActivityPayload({}, {}, 0, 1, "n") ==
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":1,\"activity\":{\"instance\":false}},"
        "\"nonce\":\"n\"}");
}

TEST_CASE("Discord activity merges the party and join secret", "[discord]") {
  PartyState party{"session-1", 2, 4, "rr1.AbC-_9"};
  CHECK(ActivityPayload({}, party, 0, 1, "n") ==
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":1,\"activity\":{\"party\":{\"id\":"
        "\"session-1\",\"size\":[2,4]},\"secrets\":{\"join\":\"rr1.AbC-_9\"},\"instance\":false}},"
        "\"nonce\":\"n\"}");
  party.max = 0;
  party.join_secret.clear();
  CHECK(ActivityPayload({}, party, 0, 1, "n") ==
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":1,\"activity\":{\"party\":{\"id\":"
        "\"session-1\"},\"instance\":false}},\"nonce\":\"n\"}");
  party.id.clear();
  party.join_secret = "s\"x";
  CHECK(ActivityPayload({}, party, 0, 1, "n") ==
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":1,\"activity\":{\"secrets\":{\"join\":"
        "\"s\\\"x\"},\"instance\":false}},\"nonce\":\"n\"}");
  CHECK(ClearActivityPayload(9, "n") ==
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":9,\"activity\":null},\"nonce\":\"n\"}");
  CHECK(SubscribePayload("ACTIVITY_JOIN", "5") ==
        "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"5\"}");
}

TEST_CASE("Discord join events yield their secret with JSON escapes decoded", "[discord]") {
  auto event = ParseEvent(
      "{\"cmd\":\"DISPATCH\",\"data\":{\"secret\":\"rr1.AbC-_9\"},\"evt\":\"ACTIVITY_JOIN\","
      "\"nonce\":null}");
  CHECK(event.evt == "ACTIVITY_JOIN");
  CHECK(event.secret == "rr1.AbC-_9");

  event = ParseEvent(
      "{ \"evt\" : \"ACTIVITY_JOIN\" , \"data\" : { \"secret\" : "
      "\"a\\\"b\\\\c\\/d\\n\\u0041\\u00e9\\ud83d\\ude00\" } }");
  CHECK(event.evt == "ACTIVITY_JOIN");
  CHECK(event.secret == "a\"b\\c/d\nA\xc3\xa9\xf0\x9f\x98\x80");

  // A "secret" key before data is not the join secret.
  event = ParseEvent(
      "{\"secret\":\"wrong\",\"evt\":\"ACTIVITY_JOIN\",\"data\":{\"secret\":\"right\"}}");
  CHECK(event.secret == "right");

  event = ParseEvent(
      "{\"cmd\":\"DISPATCH\",\"data\":{\"user\":{\"id\":\"1\",\"username\":\"Friend\"}},"
      "\"evt\":\"ACTIVITY_JOIN_REQUEST\"}");
  CHECK(event.evt == "ACTIVITY_JOIN_REQUEST");
  CHECK(event.user == "Friend");
  CHECK(event.secret.empty());

  event = ParseEvent("{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1},\"evt\":\"READY\"}");
  CHECK(event.evt == "READY");
}

TEST_CASE("Discord string reader rejects malformed values", "[discord]") {
  CHECK_FALSE(JsonString("{\"secret\":1}", "secret"));
  CHECK_FALSE(JsonString("{\"secret\":\"open", "secret"));
  CHECK_FALSE(JsonString("{\"secret\":\"bad\\q\"}", "secret"));
  CHECK_FALSE(JsonString("{\"secret\":\"\\u12\"}", "secret"));
  CHECK_FALSE(JsonString("{\"secret\":\"\\ud83d\"}", "secret"));
  CHECK_FALSE(JsonString("{\"secret\":\"\\ude00\"}", "secret"));
  CHECK_FALSE(JsonString("{\"other\":\"secret\"}", "secret"));
  CHECK(JsonString("{\"v\":\"secret\",\"secret\":\"x\"}", "secret") == "x");
  CHECK(JsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001");
}

namespace {
std::string Header(uint32_t op, uint32_t length) {
  std::string header(8, '\0');
  std::memcpy(header.data(), &op, 4);
  std::memcpy(header.data() + 4, &length, 4);
  return header;
}
std::string Frame(uint32_t op, std::string_view body) {
  return Header(op, static_cast<uint32_t>(body.size())) + std::string(body);
}

// Captures the core logger at every level for the test's lifetime.
struct LogCapture {
  std::ostringstream text;
  std::shared_ptr<spdlog::logger> logger;
  spdlog::level::level_enum level;
  LogCapture() {
    rex::InitLoggingEarly();
    logger = rex::GetLogger(rex::log::core());
    REQUIRE(logger);
    level = logger->level();
    logger->set_level(spdlog::level::trace);
    logger->sinks().push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
  }
  ~LogCapture() {
    logger->sinks().pop_back();
    logger->set_level(level);
  }
};
}  // namespace

TEST_CASE("Discord frame reader reassembles split frames", "[discord]") {
  FrameReader reader;
  const auto frame = Frame(1, "{\"evt\":\"READY\"}");
  CHECK(reader.Wanted() == 8);
  CHECK(reader.Feed({frame.data(), 3}) == FrameReader::State::kPartial);
  CHECK(reader.Wanted() == 5);
  CHECK(reader.Feed({frame.data() + 3, 5}) == FrameReader::State::kPartial);
  CHECK(reader.Wanted() == frame.size() - 8);
  CHECK(reader.Feed({frame.data() + 8, 4}) == FrameReader::State::kPartial);
  CHECK(reader.Feed({frame.data() + 12, frame.size() - 12}) == FrameReader::State::kFrame);
  CHECK(reader.op() == 1);
  CHECK(reader.body() == "{\"evt\":\"READY\"}");
  reader.Reset();
  CHECK(reader.body().empty());
  CHECK(reader.Wanted() == 8);

  const auto empty = Frame(3, "");
  CHECK(reader.Feed({empty.data(), empty.size()}) == FrameReader::State::kFrame);
  CHECK(reader.op() == 3);
  CHECK(reader.body().empty());
}

TEST_CASE("Discord frame reader refuses oversized frames before allocating", "[discord]") {
  FrameReader reader;
  SECTION("hostile length") {
    const auto header = Header(1, 0xFFFFFFFF);
    CHECK(reader.Feed({header.data(), header.size()}) == FrameReader::State::kOversized);
    CHECK(reader.body().capacity() < 1024);
  }
  SECTION("one byte over the cap") {
    const auto header = Header(1, kMaxFrameBytes + 1);
    CHECK(reader.Feed({header.data(), header.size()}) == FrameReader::State::kOversized);
  }
  SECTION("exactly the cap") {
    const auto header = Header(1, kMaxFrameBytes);
    CHECK(reader.Feed({header.data(), header.size()}) == FrameReader::State::kPartial);
    CHECK(reader.Wanted() == kMaxFrameBytes);
  }
}

TEST_CASE("Discord errors are described by code and message only", "[discord]") {
  const auto described = DescribeError(
      "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"activity\":{\"secrets\":{\"join\":\"rr1.SECRET\"}}},"
      "\"data\":{\"code\":4000,\"message\":\"Bad join secret\"},\"evt\":\"ERROR\"}");
  CHECK(described == "code 4000: Bad join secret");
  CHECK(DescribeError("{\"code\":1000,\"message\":\"closed\"}") == "code 1000: closed");
  CHECK(DescribeError("{\"secret\":\"rr1.SECRET\"}") == "code unknown");
  CHECK(JsonInteger("{\"code\" : -12}", "code") == -12);
  CHECK_FALSE(JsonInteger("{\"code\":\"x\"}", "code"));
}

TEST_CASE("Discord error messages never carry a Live invite code", "[discord]") {
  const std::string code = "rr1.AQEjRWeJq83vMTExMTExMTExMTExMTExMUJCQkJC-_09";
  CHECK(DescribeError("{\"data\":{\"code\":4000,\"message\":\"bad secret " + code +
                      " here\"},\"evt\":\"ERROR\"}") == "code 4000: bad secret <invite> here");
  CHECK(DescribeError("{\"code\":1,\"message\":\"RR1.abc,rr1.\"}") == "code 1: <invite>,<invite>");
  std::string text = "x rr1 rr2.abc " + code + code;
  RedactInviteCodes(text);
  // Adjacent codes merge into one run, so no part of the second survives.
  CHECK(text == "x rr1 rr2.abc <invite>");
}

TEST_CASE("Discord string wipes cover the whole buffer", "[discord]") {
  std::string secret(100, 'S');
  secret.resize(10);
  const char* data = secret.data();
  const size_t capacity = secret.capacity();
  REQUIRE(capacity >= 100);
  WipeString(secret);
  CHECK(secret.empty());
  CHECK(secret.data() == data);
  CHECK(std::all_of(data, data + capacity, [](char c) { return c == '\0'; }));
}

TEST_CASE("Discord activity payloads fit their reserved buffer", "[discord]") {
  PartyState party{"session-1", 2, 4, std::string(128, '\x01')};
  Presence presence = MenuPresence();
  presence.state_ = std::string(300, '\x02');
  const auto payload = ActivityPayload(presence, party, 1, 1, "n");
  // Every control character escapes to six bytes, the reserved worst case.
  CHECK(payload.find(std::string("\\u0001")) != std::string::npos);
  const size_t text = presence.details_.size() + presence.state_.size() +
                      presence.large_image_key_.size() + party.id.size() +
                      party.join_secret.size() + 1;
  // The reservation made before writing covers the whole payload.
  CHECK(payload.size() <= 256 + 6 * text);
}

#if defined(_WIN32)
namespace {
// Private-pipe Discord peer reads handshake, writes each script chunk, then
// records client output until disconnect. With nonempty flood, repeatedly write
// it after script until disconnect without reading.
class FakeDiscord {
 public:
  explicit FakeDiscord(std::vector<std::string> script, std::string flood = {}) {
    static int count = 0;
    name_ = "\\\\.\\pipe\\rex-discord-test-" + std::to_string(::GetCurrentProcessId()) + "-" +
            std::to_string(++count);
    pipe_ = ::CreateNamedPipeA(name_.c_str(), PIPE_ACCESS_DUPLEX,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0,
                               nullptr);
    REQUIRE(pipe_ != INVALID_HANDLE_VALUE);
    thread_ = std::thread([this, script = std::move(script), flood = std::move(flood)] {
      if (!::ConnectNamedPipe(pipe_, nullptr) && ::GetLastError() != ERROR_PIPE_CONNECTED)
        return;
      char buffer[4096];
      DWORD n = 0;
      if (!::ReadFile(pipe_, buffer, sizeof(buffer), &n, nullptr))
        return;
      received_.append(buffer, n);
      for (const auto& chunk : script) {
        DWORD written = 0;
        ::WriteFile(pipe_, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      for (DWORD written = 0;
           !flood.empty() &&
           ::WriteFile(pipe_, flood.data(), static_cast<DWORD>(flood.size()), &written, nullptr);) {
      }
      while (::ReadFile(pipe_, buffer, sizeof(buffer), &n, nullptr) && n > 0)
        received_.append(buffer, n);
    });
    rex::discord_rpc::detail::testing::SetIpcPath(name_);
  }
  ~FakeDiscord() {
    rex::discord_rpc::detail::testing::SetIpcPath({});
    // Unblocks a server still waiting for a client that never came.
    HANDLE client = ::CreateFileA(name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
    if (client != INVALID_HANDLE_VALUE)
      ::CloseHandle(client);
    if (thread_.joinable())
      thread_.join();
    ::CloseHandle(pipe_);
  }
  // What the client sent; valid once the client has disconnected.
  std::string Finish() {
    if (thread_.joinable())
      thread_.join();
    return received_;
  }

 private:
  std::string name_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::thread thread_;
  std::string received_;
};

const std::string kReady =
    Frame(1, "{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1},\"evt\":\"READY\",\"nonce\":null}");
std::string Join(std::string_view secret) {
  return Frame(1, "{\"cmd\":\"DISPATCH\",\"data\":{\"secret\":\"" + std::string(secret) +
                      "\"},\"evt\":\"ACTIVITY_JOIN\",\"nonce\":null}");
}
}  // namespace

TEST_CASE("Discord cold-start join wait returns the join secret", "[discord]") {
  LogCapture log;
  const auto join = Join("rr1.AbC-_9");
  const auto error =
      Frame(1,
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"secrets\":{\"join\":\"rr1.LEAKED\"}},"
            "\"data\":{\"code\":4000,\"message\":\"Bad activity\"},\"evt\":\"ERROR\"}");
  // The join frame arrives in two pieces to exercise reassembly across reads.
  FakeDiscord discord({kReady, error, join.substr(0, 20), join.substr(20)});
  const auto secret = WaitForJoin("123", std::chrono::seconds(10));
  CHECK(secret == "rr1.AbC-_9");
  const auto sent = discord.Finish();
  CHECK(sent.find("\"client_id\":\"123\"") != std::string::npos);
  CHECK(sent.find("\"evt\":\"ACTIVITY_JOIN\"") != std::string::npos);
  CHECK(sent.find("\"evt\":\"ACTIVITY_JOIN_REQUEST\"") != std::string::npos);
  const auto logged = log.text.str();
  CHECK(logged.find("code 4000: Bad activity") != std::string::npos);
  CHECK(logged.find("rr1.LEAKED") == std::string::npos);
  CHECK(logged.find("rr1.AbC-_9") == std::string::npos);
}

TEST_CASE("Discord cold-start join wait keeps its deadline against a withheld body", "[discord]") {
  // A header promising 100 bytes, followed by only part of the body.
  FakeDiscord discord({kReady, Header(1, 100) + "{\"cmd\":\"DIS"});
  const auto start = std::chrono::steady_clock::now();
  const auto secret = WaitForJoin("123", std::chrono::milliseconds(300));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(secret.empty());
  CHECK(elapsed >= std::chrono::milliseconds(300));
  CHECK(elapsed < std::chrono::seconds(5));
  // The client closed the connection cleanly at its deadline.
  CHECK(discord.Finish().find(Header(2, 2) + "{}") != std::string::npos);
}

TEST_CASE("Discord oversized IPC frames close the connection", "[discord]") {
  LogCapture log;
  FakeDiscord discord({kReady, Header(1, 0xFFFFFFFF)});
  const auto start = std::chrono::steady_clock::now();
  const auto secret = WaitForJoin("123", std::chrono::seconds(10));
  CHECK(secret.empty());
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
  // Dropped without a close frame, as for any broken peer.
  CHECK(discord.Finish().find(Header(2, 2) + "{}") == std::string::npos);
  CHECK(log.text.str().find("IPC frame over 65536 bytes") != std::string::npos);
}

TEST_CASE("Discord join wait keeps its deadline against a flooding peer", "[discord]") {
  FakeDiscord discord({kReady}, Frame(1, "{\"cmd\":\"DISPATCH\",\"evt\":\"NOISE\"}"));
  const auto start = std::chrono::steady_clock::now();
  CHECK(WaitForJoin("123", std::chrono::milliseconds(300)).empty());
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
}

TEST_CASE("Discord blocked writes end by the join wait's deadline", "[discord]") {
  // Pongs block within the first pass; the write bound is the 100 ms wait, not 500 ms.
  FakeDiscord discord({kReady}, Frame(3, std::string(60000, 'p')));
  const auto start = std::chrono::steady_clock::now();
  CHECK(WaitForJoin("123", std::chrono::milliseconds(100)).empty());
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(450));
}

TEST_CASE("Discord writes to a peer that stops reading time out and disconnect", "[discord]") {
  LogCapture log;
  // Each ping needs a pong as large; the peer never reads them.
  FakeDiscord discord({kReady}, Frame(3, std::string(60000, 'p')));
  const auto start = std::chrono::steady_clock::now();
  CHECK(WaitForJoin("123", std::chrono::seconds(10)).empty());
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
  CHECK(log.text.str().find("IPC write failed or timed out") != std::string::npos);
}
#endif
