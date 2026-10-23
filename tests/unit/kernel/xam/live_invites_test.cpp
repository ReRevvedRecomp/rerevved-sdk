#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spdlog/sinks/ostream_sink.h>

#include <rex/cvar.h>
#include <rex/live_invite.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "core/live_invite.h"
#include "core/relay_transport.h"
#include "kernel/xam/live_sessions.h"
#include "kernel/xam/relay_system_link.h"
#include "kernel/xam/system_link_config.h"
#include "test_memory.h"

namespace rex::kernel::xam::live {
namespace {
using namespace std::chrono_literals;
using relay_link::RecordsPoll;
using relay_link::RequestFailure;
using relay_link::RequestHandle;
using relay_link::RequestState;
using relay_link::TokenPoll;
using rex::live_invite::JoinResult;

uint32_t Address(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const uint8_t bytes[4]{a, b, c, d};
  uint32_t value = 0;
  std::memcpy(&value, bytes, 4);
  return value;
}
const uint32_t kHost = Address(198, 18, 0, 10);
const Ethernet kEthernet{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
constexpr uint64_t kSession = 0x0012345678ABCDEFull;
constexpr uint64_t kXuid = 0xB13E000000001234ull;
constexpr uint32_t kTitle = 0x4D5307E6;
constexpr std::string_view kRelay = "relay.example.net:30100";

uint32_t Load32(const uint8_t* p) {
  return memory::load_and_swap<uint32_t>(p);
}
void Store32(uint8_t* p, uint32_t v) {
  memory::store_and_swap<uint32_t>(p, v);
}

struct Scratch {
  rex::memory::Memory& memory;
  uint32_t base = 0;
  uint32_t size = 0;
  explicit Scratch(uint32_t bytes) : memory(rex::testing::GetTestMemory()) {
    size = (bytes + 4095) / 4096 * 4096;
    auto* heap = memory.LookupHeap(0x10000000);
    REQUIRE(heap != nullptr);
    REQUIRE(heap->Alloc(
        size, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &base));
    std::memset(at(0), 0, size);
  }
  uint8_t* at(uint32_t offset) { return memory.TranslateVirtual<uint8_t*>(base + offset); }
  uint32_t addr(uint32_t offset) const { return base + offset; }
};

// Records directory calls; tests finish each request by hand.
class InviteDirectory final : public Directory {
 public:
  bool live = true;
  std::vector<relay::SessionRecord> published;
  std::vector<uint64_t> lookups;
  std::vector<uint64_t> token_requests;
  std::map<RequestHandle, RecordsPoll> record_polls;
  std::map<RequestHandle, TokenPoll> token_polls;

  bool Live() const override { return live; }
  bool Publish(const relay::SessionRecord& record) override {
    published.push_back(record);
    return true;
  }
  bool Remove(uint64_t) override { return true; }
  RequestHandle BeginSearch(uint16_t, uint32_t) override { return 0; }
  RecordsPoll PollRecords(RequestHandle handle) override {
    auto found = record_polls.find(handle);
    if (found == record_polls.end())
      return {};
    auto poll = found->second;
    if (poll.state != RequestState::kPending)
      record_polls.erase(found);
    return poll;
  }
  RequestHandle BeginProbe(uint32_t, unsigned) override { return 0; }
  relay_link::ProbePoll PollProbe(RequestHandle) override { return {}; }
  RequestHandle BeginLookup(uint64_t session_id) override {
    if (!live)
      return 0;
    lookups.push_back(session_id);
    const auto handle = ++next_;
    record_polls[handle].state = RequestState::kPending;
    return handle;
  }
  RequestHandle BeginTokenRequest(uint64_t session_id) override {
    if (!live)
      return 0;
    token_requests.push_back(session_id);
    const auto handle = ++next_;
    token_polls.emplace(handle, TokenPoll{RequestState::kPending, RequestFailure::kNone, {}});
    return handle;
  }
  TokenPoll PollToken(RequestHandle handle) override {
    auto found = token_polls.find(handle);
    if (found == token_polls.end())
      return {};
    if (found->second.state == RequestState::kPending)
      return {RequestState::kPending, RequestFailure::kNone, std::nullopt};
    auto poll = std::move(found->second);
    token_polls.erase(found);
    return poll;
  }
  RequestHandle last() const { return next_; }
  void FinishLookup(RequestHandle handle, std::vector<relay::SessionRecord> records) {
    auto& poll = record_polls[handle];
    poll.state = RequestState::kDone;
    poll.failure = RequestFailure::kNone;
    poll.records = std::move(records);
  }
  void FailLookup(RequestHandle handle) {
    auto& poll = record_polls[handle];
    poll.state = RequestState::kFailed;
    poll.failure = RequestFailure::kTimeout;
  }
  void Grant(RequestHandle handle, uint8_t fill, uint32_t lifetime) {
    relay::TokenId id;
    id.fill(fill);
    Settle(handle, TokenPoll{RequestState::kDone, RequestFailure::kNone,
                             relay_link::JoinToken{id,
                                                   relay_link::Secret(std::string(
                                                       relay::TokenSecret::kSize, char(fill + 1))),
                                                   lifetime}});
  }
  void FailToken(RequestHandle handle) {
    Settle(handle, TokenPoll{RequestState::kFailed, RequestFailure::kTimeout, {}});
  }
  // TokenPoll holds a move-only secret, so it is replaced rather than assigned.
  void Settle(RequestHandle handle, TokenPoll poll) {
    token_polls.erase(handle);
    token_polls.emplace(handle, std::move(poll));
  }

 private:
  RequestHandle next_ = 0;
};

struct Hooks {
  std::vector<std::pair<uint32_t, uint32_t>> completed;
  std::vector<uint32_t> prepared;
  unsigned invites = 0;
  std::vector<uint64_t> open;  // masks of the guest's open listeners
  // The guest creates a listener, then the kernel tells the service.
  void Open(Service& service, uint64_t mask, Clock::time_point now) {
    open.push_back(mask);
    service.ListenerCreated(mask, now);
  }
  void CloseAll() { open.clear(); }
  GuestHooks get() {
    GuestHooks hooks;
    hooks.listening = [this] {
      return std::any_of(open.begin(), open.end(),
                         [](uint64_t mask) { return (mask & kNotifyLiveArea) != 0; });
    };
    hooks.prepare = [this](uint32_t overlapped) { prepared.push_back(overlapped); };
    hooks.complete = [this](uint32_t overlapped, uint32_t status) {
      completed.emplace_back(overlapped, status);
    };
    hooks.invite = [this] { ++invites; };
    return hooks;
  }
};

relay::SessionRecord MakeRecord(uint64_t id) {
  relay::SessionRecord record;
  record.session_id = id;
  record.host_ipv4 = kHost;
  for (size_t i = 0; i < record.key.size(); ++i)
    record.key[i] = static_cast<uint8_t>(0x60 + i);
  record.flags = kCreateHost | kCreateMatchmaking;
  record.max_public = 4;
  record.open_public = 2;
  record.max_private = 1;
  record.open_private = 1;
  const std::vector<Property> properties{{kPropertyGamerHostName, HostNameValue("Host-1")}};
  record.properties = EncodeProperties(kEthernet, properties, nullptr);
  record.contexts = EncodeContexts(std::vector<Context>{{kContextGameType, 3}}, nullptr);
  return record;
}

lan::SessionSnapshot HostSession(uint64_t id) {
  lan::SessionSnapshot session;
  session.created = true;
  session.flags = kCreateHost | kCreateMatchmaking;
  session.max_public_slots = 4;
  session.max_private_slots = 2;
  session.joined_public = 1;
  session.joined_private = 1;
  for (int i = 0; i < 8; ++i)
    session.info[i] = static_cast<uint8_t>(id >> (56 - 8 * i));
  std::memcpy(session.info.data() + 8, &kHost, 4);
  std::copy(kEthernet.begin(), kEthernet.end(), session.info.begin() + 18);
  return session;
}

// The exact XINVITE_INFO the guest reads for MakeRecord(kSession).
std::array<uint8_t, kInviteInfoSize> ExpectedInviteInfo() {
  std::array<uint8_t, kInviteInfoSize> bytes{};
  for (int i = 0; i < 8; ++i)
    bytes[i] = static_cast<uint8_t>(kXuid >> (56 - 8 * i));
  for (int i = 0; i < 4; ++i)
    bytes[0x10 + i] = static_cast<uint8_t>(kTitle >> (24 - 8 * i));
  for (int i = 0; i < 8; ++i)
    bytes[0x14 + i] = static_cast<uint8_t>(kSession >> (56 - 8 * i));
  std::memcpy(&bytes[0x1C], &kHost, 4);
  std::copy(kEthernet.begin(), kEthernet.end(), bytes.begin() + 0x26);
  for (size_t i = 0; i < 16; ++i)
    bytes[0x40 + i] = static_cast<uint8_t>(0x60 + i);
  return bytes;
}

struct InviteHarness {
  static constexpr uint32_t kRequest = 0;
  static constexpr uint32_t kOverlapped = 64;
  static constexpr uint32_t kInfo = 128;
  static constexpr uint32_t kResults = 512;
  Scratch scratch{SearchBufferSize(1) + 4096};
  InviteDirectory directory;
  Hooks hooks;
  Service service{scratch.memory, directory, hooks.get(), false};
  const uint32_t buffer = SearchBufferSize(1);

  uint32_t SearchById(uint64_t id, bool overlapped = true) {
    Store32(scratch.at(kResults), 0xA5A5A5A5);
    DispatchScope dispatch(overlapped ? scratch.addr(kOverlapped) : 0);
    const uint32_t status = HandleSearchById(service, scratch.memory, scratch.addr(kRequest), id,
                                             buffer, scratch.addr(kResults));
    CHECK(dispatch.deferred() == (status == kErrorIoPending));
    return status;
  }
  uint32_t count() { return Load32(scratch.at(kResults)); }
  std::vector<uint8_t> Results() { return {scratch.at(kResults), scratch.at(kResults) + buffer}; }
};

TEST_CASE("Live startup invite is announced once after the relay and a listener",
          "[live_sessions][live_invite]") {
  InviteHarness h;
  const auto t0 = Clock::now();
  h.directory.live = false;
  h.service.SetStartupInvite(kSession);
  CHECK(h.service.Busy());
  h.service.Pump(t0);
  CHECK(h.directory.lookups.empty());

  h.directory.live = true;
  h.service.Pump(t0);
  REQUIRE(h.directory.lookups == std::vector<uint64_t>{kSession});
  h.hooks.Open(h.service, 0x1, t0);  // system area only
  h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession)});
  h.service.Pump(t0);
  CHECK(h.hooks.invites == 0);
  CHECK_FALSE(h.service.Busy());
  REQUIRE(h.service.AcceptedInvite(0));
  CHECK(h.service.AcceptedInvite(0)->session_id == kSession);
  CHECK_FALSE(h.service.AcceptedInvite(1));
  CHECK(h.service.FoundRecord(kSession));

  // The guest's listener mask 0x2F covers the Live area.
  h.hooks.Open(h.service, 0x2F, t0);
  CHECK(h.hooks.invites == 1);
  h.service.Pump(t0 + 1s);
  h.hooks.Open(h.service, 0x2F, t0 + 1s);
  CHECK(h.hooks.invites == 1);
  CHECK(h.service.counters().invites_delivered == 1);

  SECTION("XInviteGetAcceptedInfo writes the exact XINVITE_INFO") {
    std::memset(h.scratch.at(InviteHarness::kInfo), 0xEE, kInviteInfoSize + 4);
    REQUIRE(WriteInviteInfo(h.scratch.memory, h.scratch.addr(InviteHarness::kInfo), kXuid, kTitle,
                            *h.service.AcceptedInvite(0)));
    const auto expected = ExpectedInviteInfo();
    CHECK(std::equal(expected.begin(), expected.end(), h.scratch.at(InviteHarness::kInfo)));
    CHECK(Load32(h.scratch.at(InviteHarness::kInfo + kInviteInfoSize)) == 0xEEEEEEEE);
    CHECK_FALSE(WriteInviteInfo(h.scratch.memory, 0, kXuid, kTitle, MakeRecord(kSession)));
  }
  SECTION("the guest's argument list names the user and the output buffer") {
    // As the guest marshals it: entries {type 4, pad, sign-extended value}, count at +512.
    constexpr uint32_t kArguments = 4096;
    constexpr uint32_t kUser = 4096 + 1024;
    auto put = [&](uint32_t index, uint32_t value) {
      auto* entry = h.scratch.at(kArguments + index * 16);
      Store32(entry, 4);
      memory::store_and_swap<uint64_t>(entry + 8, static_cast<uint64_t>(int64_t(int32_t(value))));
    };
    put(0, h.scratch.addr(kUser));
    put(1, h.scratch.addr(InviteHarness::kInfo));
    Store32(h.scratch.at(kArguments + 512), kInviteInfoArgumentCount);
    std::memset(h.scratch.at(InviteHarness::kInfo), 0xEE, kInviteInfoSize);
    Store32(h.scratch.at(kUser), 1);
    CHECK_FALSE(HandleInviteInfo(h.service, h.scratch.memory, h.scratch.addr(kArguments), kTitle));
    CHECK(Load32(h.scratch.at(InviteHarness::kInfo)) == 0xEEEEEEEE);
    Store32(h.scratch.at(kUser), 0);
    Store32(h.scratch.at(kArguments + 512), 3);
    CHECK_FALSE(HandleInviteInfo(h.service, h.scratch.memory, h.scratch.addr(kArguments), kTitle));
    Store32(h.scratch.at(kArguments + 512), kInviteInfoArgumentCount);
    REQUIRE(HandleInviteInfo(h.service, h.scratch.memory, h.scratch.addr(kArguments), kTitle));
    // The invitee XUID comes from the frozen configuration; the rest is the record.
    const auto expected = ExpectedInviteInfo();
    CHECK(std::equal(expected.begin() + 8, expected.end(), h.scratch.at(InviteHarness::kInfo) + 8));
  }
  SECTION("search by ID returns the search writer's bytes and clears the invite") {
    REQUIRE(h.SearchById(kSession) == kErrorIoPending);
    CHECK(h.directory.lookups.back() == kSession);
    CHECK(h.hooks.prepared == std::vector<uint32_t>{h.scratch.addr(InviteHarness::kOverlapped)});
    h.service.Pump(t0 + 2s);
    CHECK(h.hooks.completed.empty());
    h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession)});
    h.service.Pump(t0 + 2s);
    REQUIRE(h.hooks.completed.size() == 1);
    CHECK(h.hooks.completed[0].second == 0);
    CHECK(h.count() == 1);
    CHECK_FALSE(h.service.AcceptedInvite(0));
    const auto written = h.Results();
    const std::vector<relay::SessionRecord> records{MakeRecord(kSession)};
    std::memset(h.scratch.at(InviteHarness::kResults), 0, h.buffer);
    REQUIRE(WriteSearchResults(h.scratch.memory, h.scratch.addr(InviteHarness::kResults), h.buffer,
                               records) == 1);
    CHECK(h.Results() == written);
    // The XSESSION_INFO inside XINVITE_INFO matches the result's first 60 bytes.
    const auto expected = ExpectedInviteInfo();
    const auto* result = h.scratch.at(InviteHarness::kResults + kSearchHeaderSize);
    CHECK(std::equal(expected.begin() + kInviteSessionInfoOffset, expected.end() - 4, result));
  }
  SECTION("a missing session gives zero results and keeps the invite") {
    REQUIRE(h.SearchById(kSession + 1) == kErrorIoPending);
    h.directory.FinishLookup(h.directory.last(), {});
    h.service.Pump(t0 + 2s);
    REQUIRE(h.hooks.completed.size() == 1);
    CHECK(h.count() == 0);
    CHECK(h.service.AcceptedInvite(0));
  }
  SECTION("an unsearched invite is dropped after the hold time") {
    h.service.Pump(t0 + std::chrono::milliseconds(kInviteHoldMs) - 1ms);
    CHECK(h.service.AcceptedInvite(0));
    h.service.Pump(t0 + std::chrono::milliseconds(kInviteHoldMs));
    CHECK_FALSE(h.service.AcceptedInvite(0));
  }
}

TEST_CASE("Live startup invite waits for a listener created later",
          "[live_sessions][live_invite]") {
  InviteHarness h;
  const auto t0 = Clock::now();
  h.service.SetStartupInvite(kSession);
  h.service.Pump(t0);
  h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession)});
  h.service.Pump(t0);
  h.service.Pump(t0 + 10s);
  CHECK(h.hooks.invites == 0);
  CHECK(h.service.AcceptedInvite(0));
  h.hooks.Open(h.service, kNotifyLiveArea, t0 + 11s);
  CHECK(h.hooks.invites == 1);
}

TEST_CASE("Live invite waits while the guest has no open listener",
          "[live_sessions][live_invite]") {
  InviteHarness h;
  const auto t0 = Clock::now();
  h.hooks.Open(h.service, 0x2F, t0);
  // The guest closes its only Live listener before the invite is found.
  h.hooks.CloseAll();
  h.service.SetStartupInvite(kSession);
  h.service.Pump(t0);
  h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession)});
  h.service.Pump(t0);
  CHECK(h.hooks.invites == 0);
  CHECK(h.service.counters().invites_delivered == 0);
  // Not marked delivered, so the hold time does not run out.
  h.service.Pump(t0 + std::chrono::milliseconds(kInviteHoldMs) + 1s);
  REQUIRE(h.service.AcceptedInvite(0));
  // A system-only listener does not count; a replacement Live listener does.
  h.hooks.Open(h.service, 0x1, t0 + 61s);
  CHECK(h.hooks.invites == 0);
  h.hooks.Open(h.service, 0x2F, t0 + 62s);
  CHECK(h.hooks.invites == 1);
  h.hooks.Open(h.service, 0x2F, t0 + 63s);
  CHECK(h.hooks.invites == 1);
}

TEST_CASE("Live startup invite for a missing session is dropped", "[live_sessions][live_invite]") {
  InviteHarness h;
  const auto t0 = Clock::now();
  h.hooks.Open(h.service, 0x2F, t0);
  h.service.SetStartupInvite(kSession);
  h.service.Pump(t0);
  SECTION("not found") {
    h.directory.FinishLookup(h.directory.last(), {});
    h.service.Pump(t0);
  }
  SECTION("every retry fails") {
    for (unsigned i = 0; i < kInviteLookupAttempts; ++i) {
      h.directory.FailLookup(h.directory.last());
      h.service.Pump(t0 + std::chrono::seconds(2 * i));
      h.service.Pump(t0 + std::chrono::seconds(2 * i + 1));
    }
    CHECK(h.directory.lookups.size() == kInviteLookupAttempts);
  }
  CHECK_FALSE(h.service.Busy());
  CHECK(h.hooks.invites == 0);
  CHECK_FALSE(h.service.AcceptedInvite(0));
  CHECK(h.service.PollAccept() == AcceptState::kNone);
}

TEST_CASE("Live search by ID probes its size and answers without a relay",
          "[live_sessions][live_invite]") {
  InviteHarness h;
  Store32(h.scratch.at(InviteHarness::kRequest + 12), 0);
  CHECK(HandleSearchById(h.service, h.scratch.memory, h.scratch.addr(InviteHarness::kRequest),
                         kSession, 0, 0) == kErrorInsufficientBuffer);
  CHECK(Load32(h.scratch.at(InviteHarness::kRequest + 12)) == 1334);
  h.directory.live = false;
  CHECK(h.SearchById(kSession) == 0);
  CHECK(h.count() == 0);
  CHECK(h.hooks.prepared.empty());
  CHECK(h.service.counters().search_failures == 1);
}

TEST_CASE("Live search by ID keeps the invite when no record fits the buffer",
          "[live_sessions][live_invite]") {
  InviteHarness h;
  const auto t0 = Clock::now();
  h.hooks.Open(h.service, 0x2F, t0);
  h.service.SetStartupInvite(kSession);
  h.service.Pump(t0);
  h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession)});
  h.service.Pump(t0);
  REQUIRE(h.hooks.invites == 1);

  // A header-only buffer is accepted but cannot hold the record.
  Store32(h.scratch.at(InviteHarness::kResults), 0xA5A5A5A5);
  {
    DispatchScope dispatch(h.scratch.addr(InviteHarness::kOverlapped));
    REQUIRE(HandleSearchById(h.service, h.scratch.memory, h.scratch.addr(InviteHarness::kRequest),
                             kSession, kSearchHeaderSize,
                             h.scratch.addr(InviteHarness::kResults)) == kErrorIoPending);
  }
  h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession)});
  h.service.Pump(t0 + 1s);
  REQUIRE(h.hooks.completed.size() == 1);
  CHECK(h.count() == 0);
  REQUIRE(h.service.AcceptedInvite(0));

  // A retry with room for the record returns it and clears the invite.
  REQUIRE(h.SearchById(kSession) == kErrorIoPending);
  h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession)});
  h.service.Pump(t0 + 2s);
  CHECK(h.count() == 1);
  CHECK_FALSE(h.service.AcceptedInvite(0));
}

TEST_CASE("XMsg overlapped guard accepts only a whole writable XOVERLAPPED",
          "[live_sessions][live_invite]") {
  Scratch scratch(8192);
  auto* heap = scratch.memory.LookupHeap(scratch.base);
  REQUIRE(heap);
  REQUIRE(heap->Protect(scratch.addr(4096), 4096, rex::memory::kMemoryProtectRead));
  CHECK(OverlappedWritable(scratch.memory, scratch.addr(0)));
  CHECK(OverlappedWritable(scratch.memory, scratch.addr(4096 - kOverlappedSize)));
  CHECK_FALSE(OverlappedWritable(scratch.memory, scratch.addr(4096 - kOverlappedSize + 4)));
  CHECK_FALSE(OverlappedWritable(scratch.memory, scratch.addr(4096)));
  CHECK_FALSE(OverlappedWritable(scratch.memory, 0));
  CHECK_FALSE(OverlappedWritable(scratch.memory, 0xFFFFFFF0u));
}

TEST_CASE("Live searches refuse an overlapped the guest cannot write",
          "[live_sessions][live_invite]") {
  InviteHarness h;
  auto* heap = h.scratch.memory.LookupHeap(h.scratch.base);
  REQUIRE(heap);
  REQUIRE(heap->Protect(h.scratch.addr(4096), 4096, rex::memory::kMemoryProtectRead));
  const uint32_t request = h.scratch.addr(InviteHarness::kRequest);
  const uint32_t results = h.scratch.addr(InviteHarness::kResults);
  // Read-only, crossing into read-only, and past the end of the address space.
  for (uint32_t overlapped :
       {h.scratch.addr(4096), h.scratch.addr(4096 - kOverlappedSize + 4), 0xFFFFFFF0u}) {
    DispatchScope dispatch(overlapped);
    CHECK(HandleSearchById(h.service, h.scratch.memory, request, kSession, h.buffer, results) ==
          kErrorInvalidParameter);
    CHECK(HandleSearch(h.service, h.scratch.memory, request, 32) == kErrorInvalidParameter);
    CHECK_FALSE(dispatch.deferred());
  }
  CHECK(h.hooks.prepared.empty());
  CHECK(h.directory.lookups.empty());
  // The last writable XOVERLAPPED before the read-only page is accepted.
  const uint32_t last = h.scratch.addr(4096 - kOverlappedSize);
  DispatchScope dispatch(last);
  CHECK(HandleSearchById(h.service, h.scratch.memory, request, kSession, h.buffer, results) ==
        kErrorIoPending);
  CHECK(h.hooks.prepared == std::vector<uint32_t>{last});
}

std::string Code(uint64_t session_id, std::string_view address) {
  relay::LiveInvite invite;
  invite.session_id = session_id;
  invite.token_id.fill(0x31);
  invite.address = *relay_link::ParseAddress(address);
  return *relay::EncodeLiveInvite(invite);
}

struct JoinHarness {
  Scratch scratch{4096};
  InviteDirectory directory;
  Hooks hooks;
  std::shared_ptr<Service> service =
      std::make_shared<Service>(scratch.memory, directory, hooks.get(), false);
  const relay::Endpoint server{Address(203, 0, 113, 7), 30100};
  struct Resolver {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> resolved;
    bool release = false;  // lets a stalled resolution finish
  };
  std::shared_ptr<Resolver> resolver = std::make_shared<Resolver>();
  InviteJoin join;

  ~JoinHarness() { Release(); }
  void Release() {
    std::lock_guard lock(resolver->mutex);
    resolver->release = true;
    resolver->changed.notify_all();
  }
  std::vector<std::string> resolved() {
    std::lock_guard lock(resolver->mutex);
    return resolver->resolved;
  }

  InviteJoin::Environment Environment() {
    InviteJoin::Environment environment;
    environment.config.mode = system_link::Mode::kRelay;
    environment.config.address = std::string(kRelay);
    environment.config.live_sessions = true;
    environment.live = true;
    environment.server = server;
    environment.service = service;
    environment.resolve = [resolver = resolver,
                           server = server](std::string_view address) -> Result<relay::Endpoint> {
      std::unique_lock lock(resolver->mutex);
      resolver->resolved.emplace_back(address);
      if (address == "stalled.example.net:30100") {
        resolver->changed.wait_for(lock, 10s, [&] { return resolver->release; });
        return server;
      }
      if (address == "other.example.net:30100")
        return relay::Endpoint{Address(203, 0, 113, 99), 30100};
      if (address == "alias.example.net:30100")
        return server;
      return std::unexpected(Error{ErrorCategory::IO, "unresolved"});
    };
    return environment;
  }
  rex::live_invite::JoinStatus Settle() {
    auto status = join.Poll();
    for (int i = 0; i < 500 && status.result == JoinResult::kPending && !directory.last(); ++i) {
      std::this_thread::sleep_for(1ms);
      status = join.Poll();
    }
    return status;
  }
};

TEST_CASE("Live running-game join accepts a session in the same relay room",
          "[live_sessions][live_invite]") {
  JoinHarness h;
  auto status = h.join.Begin(Code(kSession, "RELAY.example.net:30100"), h.Environment());
  CHECK(status.result == JoinResult::kPending);
  CHECK(h.resolved().empty());
  REQUIRE(h.directory.lookups == std::vector<uint64_t>{kSession});
  CHECK(h.join.Poll().result == JoinResult::kPending);
  h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession)});
  h.service->Pump(Clock::now());
  status = h.join.Poll();
  CHECK(status.result == JoinResult::kAccepted);
  CHECK(status.reason.empty());
  REQUIRE(h.service->AcceptedInvite(0));
  CHECK(h.service->AcceptedInvite(0)->session_id == kSession);
  h.hooks.Open(*h.service, 0x2F, Clock::now());
  CHECK(h.hooks.invites == 1);

  // A later accept replaces the pending invite.
  status = h.join.Begin(Code(kSession + 1, kRelay), h.Environment());
  CHECK(status.result == JoinResult::kPending);
  h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession + 1)});
  h.service->Pump(Clock::now());
  CHECK(h.join.Poll().result == JoinResult::kAccepted);
  CHECK(h.service->AcceptedInvite(0)->session_id == kSession + 1);
  CHECK(h.hooks.invites == 2);
}

TEST_CASE("Live running-game join compares resolved relay endpoints",
          "[live_sessions][live_invite]") {
  JoinHarness h;
  SECTION("an alias of the same relay is accepted") {
    CHECK(h.join.Begin(Code(kSession, "alias.example.net:30100"), h.Environment()).result ==
          JoinResult::kPending);
    auto status = h.Settle();
    CHECK(status.result == JoinResult::kPending);
    REQUIRE(h.directory.lookups.size() == 1);
    h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession)});
    h.service->Pump(Clock::now());
    CHECK(h.join.Poll().result == JoinResult::kAccepted);
  }
  SECTION("another relay needs a restart") {
    h.join.Begin(Code(kSession, "other.example.net:30100"), h.Environment());
    auto status = h.Settle();
    for (int i = 0; i < 500 && status.result == JoinResult::kPending; ++i) {
      std::this_thread::sleep_for(1ms);
      status = h.join.Poll();
    }
    CHECK(status.result == JoinResult::kRestartRequired);
    CHECK(h.directory.lookups.empty());
  }
  SECTION("an unresolvable relay is unavailable") {
    h.join.Begin(Code(kSession, "unknown.example.net:30100"), h.Environment());
    auto status = h.Settle();
    for (int i = 0; i < 500 && status.result == JoinResult::kPending; ++i) {
      std::this_thread::sleep_for(1ms);
      status = h.join.Poll();
    }
    CHECK(status.result == JoinResult::kUnavailable);
  }
}

TEST_CASE("Live running-game join needs a restart outside the invite's room",
          "[live_sessions][live_invite]") {
  JoinHarness h;
  const auto code = Code(kSession, kRelay);
  auto environment = h.Environment();
  SECTION("System Link off") {
    environment.config.mode = system_link::Mode::kOff;
  }
  SECTION("LAN") {
    environment.config.mode = system_link::Mode::kLan;
  }
  SECTION("configuration error") {
    environment.config.error = "bad relay settings";
  }
  SECTION("Live sessions off") {
    environment.config.live_sessions = false;
  }
  SECTION("relay not connected") {
    environment.live = false;
  }
  SECTION("session in another room") {
    REQUIRE(h.join.Begin(code, h.Environment()).result == JoinResult::kPending);
    h.directory.FinishLookup(h.directory.last(), {});
    h.service->Pump(Clock::now());
    const auto status = h.join.Poll();
    CHECK(status.result == JoinResult::kRestartRequired);
    CHECK_FALSE(status.reason.empty());
    CHECK_FALSE(h.service->AcceptedInvite(0));
    return;
  }
  const auto status = h.join.Begin(code, std::move(environment));
  CHECK(status.result == JoinResult::kRestartRequired);
  CHECK_FALSE(status.reason.empty());
  CHECK(h.join.Poll().result == JoinResult::kRestartRequired);
  CHECK(h.directory.lookups.empty());
}

TEST_CASE("Live running-game join reports invalid codes and lookup failures",
          "[live_sessions][live_invite]") {
  JoinHarness h;
  CHECK(h.join.Poll().result == JoinResult::kUnavailable);
  for (std::string_view code : {"", "RXR1-ABCD", "rr1.", "rr1.!!!!"})
    CHECK(h.join.Begin(code, h.Environment()).result == JoinResult::kInvalid);
  CHECK(h.join.Begin("  " + Code(kSession, kRelay) + "\n", h.Environment()).result ==
        JoinResult::kPending);
  h.directory.FailLookup(h.directory.last());
  h.service->Pump(Clock::now());
  CHECK(h.join.Poll().result == JoinResult::kUnavailable);
  h.directory.live = false;
  CHECK(h.join.Begin(Code(kSession, kRelay), h.Environment()).result == JoinResult::kUnavailable);
}

TEST_CASE("Live running-game join replaced while pending lets only the latest notify",
          "[live_sessions][live_invite]") {
  JoinHarness h;
  h.hooks.Open(*h.service, 0x2F, Clock::now());
  REQUIRE(h.join.Begin(Code(kSession, kRelay), h.Environment()).result == JoinResult::kPending);
  const auto first = h.directory.last();
  REQUIRE(first);
  SECTION("by an invalid code") {
    CHECK(h.join.Begin("rr1.!!!!", h.Environment()).result == JoinResult::kInvalid);
  }
  SECTION("by a code that needs a restart") {
    auto environment = h.Environment();
    environment.config.live_sessions = false;
    CHECK(h.join.Begin(Code(kSession, kRelay), std::move(environment)).result ==
          JoinResult::kRestartRequired);
  }
  SECTION("by another session") {
    REQUIRE(h.join.Begin(Code(kSession + 1, kRelay), h.Environment()).result ==
            JoinResult::kPending);
    const auto second = h.directory.last();
    REQUIRE(second != first);
    h.directory.FinishLookup(first, {MakeRecord(kSession)});
    h.service->Pump(Clock::now());
    CHECK(h.hooks.invites == 0);
    CHECK_FALSE(h.service->AcceptedInvite(0));
    CHECK(h.join.Poll().result == JoinResult::kPending);
    h.directory.FinishLookup(second, {MakeRecord(kSession + 1)});
    h.service->Pump(Clock::now());
    CHECK(h.join.Poll().result == JoinResult::kAccepted);
    REQUIRE(h.service->AcceptedInvite(0));
    CHECK(h.service->AcceptedInvite(0)->session_id == kSession + 1);
    CHECK(h.hooks.invites == 1);
    return;
  }
  // The replaced lookup completes, but its session never reaches the guest.
  h.directory.FinishLookup(first, {MakeRecord(kSession)});
  h.service->Pump(Clock::now());
  CHECK(h.hooks.invites == 0);
  CHECK_FALSE(h.service->AcceptedInvite(0));
  CHECK(h.service->PollAccept() == AcceptState::kNone);
  CHECK_FALSE(h.service->Busy());
}

TEST_CASE("Live running-game join never waits for a replaced resolution",
          "[live_sessions][live_invite]") {
  JoinHarness h;
  REQUIRE(h.join.Begin(Code(kSession, "stalled.example.net:30100"), h.Environment()).result ==
          JoinResult::kPending);
  for (int i = 0; i < 500 && h.resolved().empty(); ++i)
    std::this_thread::sleep_for(1ms);
  REQUIRE(h.resolved().size() == 1);
  // The first resolution is still stalled while the second attempt begins.
  const auto start = std::chrono::steady_clock::now();
  CHECK(h.join.Begin(Code(kSession + 1, kRelay), h.Environment()).result == JoinResult::kPending);
  CHECK(std::chrono::steady_clock::now() - start < 2s);
  h.Release();
  h.directory.FinishLookup(h.directory.last(), {MakeRecord(kSession + 1)});
  h.service->Pump(Clock::now());
  CHECK(h.join.Poll().result == JoinResult::kAccepted);
  // The released resolution is ignored; only the second session was looked up.
  std::this_thread::sleep_for(50ms);
  CHECK(h.join.Poll().result == JoinResult::kAccepted);
  CHECK(h.directory.lookups == std::vector<uint64_t>{kSession + 1});
}

TEST_CASE("Live host invite requests a token on demand and refreshes it",
          "[live_sessions][live_invite]") {
  Scratch scratch(4096);
  InviteDirectory directory;
  Service service(scratch.memory, directory, {}, false);
  const auto t0 = Clock::now();
  CHECK(EncodeHostInvite(service.HostInvite(t0), kRelay).code.empty());
  CHECK(directory.token_requests.empty());

  service.SessionChanged(HostSession(kSession), t0);
  REQUIRE(service.hosted_session() == kSession);
  service.Pump(t0 + 1s);
  CHECK(directory.token_requests.empty());

  auto state = service.HostInvite(t0 + 1s);
  CHECK(directory.token_requests == std::vector<uint64_t>{kSession});
  auto invite = EncodeHostInvite(state, kRelay);
  CHECK(invite.code.empty());
  CHECK(invite.members == 2);
  CHECK(invite.max_members == 6);
  service.HostInvite(t0 + 1s);
  CHECK(directory.token_requests.size() == 1);

  directory.Grant(directory.last(), 0x10, 900);
  service.Pump(t0 + 2s);
  invite = EncodeHostInvite(service.HostInvite(t0 + 2s), kRelay);
  REQUIRE_FALSE(invite.code.empty());
  auto decoded = relay::DecodeLiveInvite(invite.code);
  REQUIRE(decoded);
  CHECK(decoded->session_id == kSession);
  relay::TokenId first_id;
  first_id.fill(0x10);
  CHECK(decoded->token_id == first_id);
  std::array<uint8_t, relay::TokenSecret::kSize> secret;
  secret.fill(0x11);
  CHECK(decoded->token_secret == relay::TokenSecret(secret));
  CHECK(decoded->address.host == "relay.example.net");
  CHECK(EncodeHostInvite(service.HostInvite(t0 + 2s), std::string(200, 'h') + ":1").code.empty());

  // Refreshed in the background once less than 300 s remain.
  service.Pump(t0 + 2s + 599s);
  CHECK(directory.token_requests.size() == 1);
  service.Pump(t0 + 2s + 601s);
  CHECK(directory.token_requests.size() == 2);
  CHECK(EncodeHostInvite(service.HostInvite(t0 + 2s + 601s), kRelay).code == invite.code);
  directory.Grant(directory.last(), 0x20, 900);
  service.Pump(t0 + 2s + 602s);
  const auto refreshed = EncodeHostInvite(service.HostInvite(t0 + 2s + 602s), kRelay);
  REQUIRE_FALSE(refreshed.code.empty());
  CHECK(refreshed.code != invite.code);
  service.Pump(t0 + 2s + 900s);
  CHECK(directory.token_requests.size() == 2);

  SECTION("requests stop when the session ends") {
    service.EndSession(t0 + 1000s);
    CHECK(EncodeHostInvite(service.HostInvite(t0 + 1000s), kRelay).code.empty());
    service.Pump(t0 + 5000s);
    CHECK(directory.token_requests.size() == 2);
    CHECK_FALSE(service.Busy());
  }
  SECTION("an unanswered request is retried") {
    service.Pump(t0 + 2s + 602s + 601s);
    REQUIRE(directory.token_requests.size() == 3);
    directory.FailToken(directory.last());
    service.Pump(t0 + 1300s);
    CHECK(directory.token_requests.size() == 3);
    service.Pump(t0 + 1300s + std::chrono::milliseconds(kInviteTokenRetryMs));
    CHECK(directory.token_requests.size() == 4);
  }
  SECTION("no invite without a live relay") {
    directory.live = false;
    CHECK(EncodeHostInvite(service.HostInvite(t0 + 1000s), kRelay).code.empty());
  }
}

// Mirrors system_link_config_test's fixture: a fresh snapshot and default flags.
struct ConfigFixture {
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("live-invites-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::path path = directory / "fixture";
  ConfigFixture() {
    REQUIRE(std::filesystem::create_directory(directory));
    system_link::testing::ResetSnapshot();
    rex::cvar::testing::ResetAllForTesting();
  }
  ~ConfigFixture() {
    system_link::testing::ResetSnapshot();
    rex::cvar::testing::ResetAllForTesting();
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(directory, error);
  }
  void Set(std::string_view key, std::string_view value) {
    REQUIRE(rex::cvar::SetFlagByName(key, value));
  }
  void Freeze() {
    system_link::FreezeConfig(path, [] { return rex::relay::Digest{0xCD, 0xAB, 2, 3, 4, 5}; });
  }
};

std::string TokenCode() {
  relay::LiveInvite invite;
  invite.session_id = kSession;
  invite.token_id.fill(0x31);
  std::array<uint8_t, relay::TokenSecret::kSize> secret;
  secret.fill(0x42);
  invite.token_secret = relay::TokenSecret(secret);
  invite.address = {"relay.example.net", 30100};
  return *relay::EncodeLiveInvite(invite);
}

std::string HostInviteCode(ConfigFixture& f, std::string_view advertised) {
  f.Set("system_link_relay_address", "127.0.0.1:30100");
  f.Set("system_link_relay_room", "room");
  f.Set("system_link_relay_secret_file", "secret_file");
  f.Set("live_sessions", "true");
  f.Set("system_link_advertised_address", advertised);
  f.Freeze();
  Scratch scratch(4096);
  InviteDirectory directory;
  Service service(scratch.memory, directory, {}, false);
  testing::SetService(&service);
  Arm();
  struct Disarm {
    ~Disarm() {
      testing::SetService(nullptr);
      Shutdown();
    }
  } disarm;
  const auto now = Clock::now();
  service.SessionChanged(HostSession(kSession), now);
  rex::live_invite::CurrentHostInvite();
  REQUIRE(directory.token_requests == std::vector<uint64_t>{kSession});
  directory.Grant(directory.last(), 0x10, 900);
  service.Pump(Clock::now());
  return rex::live_invite::CurrentHostInvite().code;
}

TEST_CASE("Live host invite advertises system_link_advertised_address", "[live_invite]") {
  ConfigFixture f;
  std::string expected = "127.0.0.1:30100";
  std::string advertised;
  SECTION("unset advertises the connect address") {}
  SECTION("set advertises that address") {
    advertised = "play.example.net:40100";
    expected = advertised;
  }
  SECTION("invalid is ignored") {
    advertised = "play.example.net";
  }
  SECTION("longest host that fits advertises it") {
    advertised = std::string(29, 'h') + ":30100";
    expected = advertised;
  }
  SECTION("host too long for a code is ignored") {
    advertised = std::string(30, 'h') + ":30100";
  }
  const auto code = HostInviteCode(f, advertised);
  REQUIRE_FALSE(code.empty());
  const auto decoded = relay::DecodeLiveInvite(code);
  REQUIRE(decoded);
  CHECK(decoded->address.host + ":" + std::to_string(decoded->address.port) == expected);
  // The room is never in the code; the token and session are unchanged by the address.
  CHECK(decoded->session_id == kSession);
  relay::TokenId id;
  id.fill(0x10);
  CHECK(decoded->token_id == id);
  CHECK(decoded->token_secret == relay::TokenSecret([] {
          std::array<uint8_t, relay::TokenSecret::kSize> secret;
          secret.fill(0x11);
          return secret;
        }()));

  CHECK(system_link::CurrentConfig().address == "127.0.0.1:30100");
}

TEST_CASE("live_invite option is a session-only Online setting", "[system_link][live_invite]") {
  ConfigFixture f;
  const auto* flag = rex::cvar::GetFlagInfo("live_invite");
  REQUIRE(flag);
  CHECK(flag->type == rex::cvar::FlagType::String);
  CHECK(flag->category == "Online");
  CHECK(flag->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);
  CHECK(flag->persistence == rex::cvar::Persistence::kNeverPersist);
  CHECK(flag->default_value.empty());
  const auto code = TokenCode();
  f.Set("live_invite", " '" + code + "'\n");
  CHECK(rex::cvar::GetFlagByName("live_invite") == code);
  CHECK_FALSE(rex::cvar::SetFlagByName("live_invite", "rr1.a b"));
  CHECK_FALSE(rex::cvar::SetFlagByName("live_invite", "rr1.a/b"));
  CHECK(rex::cvar::GetFlagByName("live_invite") == code);

  rex::cvar::SaveConfig(f.path);
  std::ifstream stream(f.path);
  const std::string saved{std::istreambuf_iterator<char>(stream), {}};
  CHECK(saved.find("live_invite") == std::string::npos);
  CHECK(saved.find(code) == std::string::npos);
}

void SetEnv(const std::string& value) {
#ifdef _WIN32
  _putenv_s("REX_LIVE_INVITE", value.c_str());
#else
  if (value.empty())
    unsetenv("REX_LIVE_INVITE");
  else
    setenv("REX_LIVE_INVITE", value.c_str(), 1);
#endif
}

TEST_CASE("live_invite values never reach the log", "[system_link][live_invite]") {
  ConfigFixture f;
  rex::InitLoggingEarly();
  auto logger = rex::GetLogger(rex::log::core());
  REQUIRE(logger);
  std::ostringstream captured;
  struct Restore {
    std::shared_ptr<spdlog::logger> logger;
    spdlog::level::level_enum level;
    ~Restore() {
      logger->sinks().pop_back();
      logger->set_level(level);
    }
  } restore{logger, logger->level()};
  logger->set_level(spdlog::level::trace);
  logger->sinks().push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(captured));
  const auto code = TokenCode();
  struct Env {
    ~Env() { SetEnv(""); }
  } env;
  SECTION("accepted from the environment") {
    SetEnv(code);
    rex::cvar::ApplyEnvironment();
    CHECK(rex::cvar::GetFlagByName("live_invite") == code);
    CHECK(captured.str().find("REX_LIVE_INVITE") != std::string::npos);
  }
  SECTION("rejected from the environment") {
    SetEnv(code + "!");
    rex::cvar::ApplyEnvironment();
    CHECK(rex::cvar::GetFlagByName("live_invite").empty());
    CHECK(captured.str().find("REX_LIVE_INVITE") != std::string::npos);
  }
  SECTION("from a configuration file") {
    {
      std::ofstream stream(f.path);
      stream << "live_invite = \"" << code << "\"\n";
    }
    rex::cvar::LoadConfig(f.path);
    CHECK(captured.str().find("live_invite") != std::string::npos);
  }
  CHECK(captured.str().find(code) == std::string::npos);
  CHECK(captured.str().find(code.substr(4, 16)) == std::string::npos);
}

TEST_CASE("live_invite alone joins the token's relay with Live sessions",
          "[system_link][live_invite]") {
  ConfigFixture f;
  f.Set("live_invite", TokenCode());
  f.Freeze();
  const auto config = system_link::CurrentConfig();
  REQUIRE(config.error.empty());
  CHECK(config.mode == system_link::Mode::kRelay);
  CHECK(config.address == "relay.example.net:30100");
  CHECK(config.room.empty());
  CHECK_FALSE(config.invite_secret);
  CHECK(config.live_sessions);
  CHECK(config.startup_invite == kSession);
  REQUIRE(config.join_token);
  relay::TokenId id;
  id.fill(0x31);
  CHECK(config.join_token->id == id);
  CHECK(config.join_token->secret.view() == std::string(relay::TokenSecret::kSize, '\x42'));
  auto copy = system_link::CopyJoinToken(config);
  REQUIRE(copy);
  CHECK(copy->id == id);
  CHECK(copy->secret.view() == config.join_token->secret.view());
  copy->secret.Clear();
  CHECK(config.join_token->secret.view().size() == relay::TokenSecret::kSize);
  CHECK(system_link::LiveSessionsActive());
  CHECK(config.xuid != 0);
  CHECK(system_link::Status() == "Relay: waiting for System Link");
}

TEST_CASE("live_invite takes precedence over saved System Link settings",
          "[system_link][live_invite]") {
  ConfigFixture f;
  rex::InitLoggingEarly();
  auto logger = rex::GetLogger(rex::log::core());
  REQUIRE(logger);
  std::ostringstream captured;
  struct Restore {
    std::shared_ptr<spdlog::logger> logger;
    spdlog::level::level_enum level;
    ~Restore() {
      logger->sinks().pop_back();
      logger->set_level(level);
    }
  } restore{logger, logger->level()};
  logger->set_level(spdlog::level::trace);
  logger->sinks().push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(captured));

  const auto code = TokenCode();
  auto system_link_invite =
      rex::relay::EncodeInvite({"saved.example.net", 1}, "r", "0123456789ABCDEF");
  REQUIRE(system_link_invite);
  // Each saved setting the Live invite overrides, with the value it keeps.
  std::vector<std::pair<std::string, std::string>> saved;
  SECTION("System Link invite") {
    saved = {{"system_link_invite", *system_link_invite}};
  }
  SECTION("explicit relay settings") {
    saved = {{"system_link_relay_address", "saved.example.net:30100"},
             {"system_link_relay_room", "savedroom"},
             {"system_link_relay_secret_file", "saved-secret.txt"}};
  }
  SECTION("LAN") {
    saved = {{"system_link_lan_enabled", "true"}};
  }
  SECTION("all of them") {
    saved = {{"system_link_invite", *system_link_invite},
             {"system_link_relay_address", "saved.example.net:30100"},
             {"system_link_relay_room", "savedroom"},
             {"system_link_relay_secret_file", "saved-secret.txt"},
             {"system_link_lan_enabled", "true"}};
  }
  for (const auto& [name, value] : saved)
    f.Set(name, value);
  // These still apply under a Live invite.
  f.Set("system_link_local_address", "127.0.0.1");
  f.Set("system_link_xuid", "12345");
  f.Set("system_link_profile_name", "Tester");
  f.Set("live_invite", code);
  f.Freeze();

  const auto config = system_link::CurrentConfig();
  REQUIRE(config.error.empty());
  CHECK(config.mode == system_link::Mode::kRelay);
  CHECK_FALSE(config.lan_enabled);
  CHECK(config.live_sessions);
  CHECK(config.address == "relay.example.net:30100");
  CHECK(config.room.empty());
  CHECK(config.secret_file.empty());
  CHECK_FALSE(config.invite_secret);
  REQUIRE(config.join_token);
  CHECK(config.startup_invite == kSession);
  CHECK(config.local_address == "127.0.0.1");
  CHECK(config.xuid == 12345);
  CHECK(config.profile_name == "Tester");
  CHECK(system_link::LiveSessionsActive());
  CHECK(system_link::Status() == "Relay: waiting for System Link");

  // The overridden settings keep their values, and saving persists them but not the invite.
  for (const auto& [name, value] : saved)
    CHECK(rex::cvar::GetFlagByName(name) == value);
  rex::cvar::SaveConfig(f.path);
  std::ifstream stream(f.path);
  const std::string file{std::istreambuf_iterator<char>(stream), {}};
  CHECK(file.find("live_invite") == std::string::npos);
  CHECK(file.find(code) == std::string::npos);
  for (const auto& [name, value] : saved)
    CHECK(file.find(name) != std::string::npos);

  // One info line names the overridden settings and never a value.
  const auto logged = captured.str();
  const auto line = logged.find("live_invite overrides");
  REQUIRE(line != std::string::npos);
  CHECK(logged.find("live_invite overrides", line + 1) == std::string::npos);
  for (const auto& [name, value] : saved) {
    CHECK(logged.find(name, line) != std::string::npos);
    if (name != "system_link_lan_enabled")
      CHECK(logged.find(value) == std::string::npos);
  }
  CHECK(logged.find(code) == std::string::npos);
}

TEST_CASE("live_invite alone logs no overrides", "[system_link][live_invite]") {
  ConfigFixture f;
  rex::InitLoggingEarly();
  auto logger = rex::GetLogger(rex::log::core());
  REQUIRE(logger);
  std::ostringstream captured;
  struct Restore {
    std::shared_ptr<spdlog::logger> logger;
    spdlog::level::level_enum level;
    ~Restore() {
      logger->sinks().pop_back();
      logger->set_level(level);
    }
  } restore{logger, logger->level()};
  logger->set_level(spdlog::level::trace);
  logger->sinks().push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(captured));
  f.Set("live_invite", TokenCode());
  f.Freeze();
  CHECK(system_link::CurrentConfig().error.empty());
  CHECK(captured.str().find("live_invite overrides") == std::string::npos);
}

TEST_CASE("live_invite with a bad code is a configuration error", "[system_link][live_invite]") {
  ConfigFixture f;
  auto invite = rex::relay::EncodeInvite({"a", 1}, "r", "0123456789ABCDEF");
  REQUIRE(invite);
  f.Set("system_link_invite", *invite);
  f.Set("live_invite", "rr1.AAAA");
  f.Freeze();
  const auto config = system_link::CurrentConfig();
  CHECK_FALSE(config.error.empty());
  CHECK_FALSE(config.join_token);
  CHECK_FALSE(config.invite_secret);
  CHECK_FALSE(system_link::LiveSessionsActive());
  CHECK(system_link::Status().starts_with("Relay failed: "));
}

TEST_CASE("an expired invite token asks for a new invite", "[system_link][live_invite][network]") {
  ConfigFixture f;
  auto server = relay::UdpSocket::Open("127.0.0.1", 0);
  if (!server)
    SKIP("Loopback UDP socket unavailable");
  // The relay answers an unknown or expired token with REJECT reason 3.
  std::jthread pump([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      auto packet = (*server)->Receive(10);
      if (packet && *packet) {
        relay::Message reject;
        reject.type = relay::Type::kReject;
        reject.reason = relay::Reason::kAuth;
        if (auto wire = relay::Encode(reject))
          (void)(*server)->Send({(**packet).endpoint, *wire});
      }
    }
  });
  system_link::Config config;
  config.mode = system_link::Mode::kRelay;
  config.address = "127.0.0.1:" + std::to_string((*server)->endpoint().port);
  config.join_token = std::make_shared<const relay_link::JoinToken>(relay_link::JoinToken{
      {}, relay_link::Secret(std::string(relay::TokenSecret::kSize, 'k')), 0});
  config.xuid = kXuid;
  relay_link::Client client;
  lan::LanSystemLinkState lan;
  system_link::RelayStartup startup;
  CHECK(startup.Start(config, client, lan) == 0);
  CHECK(client.stats().last_failure == system_link::kInviteExpired);
  CHECK(system_link::RelayStatus(client.stats()) ==
        "Relay failed: invite expired; ask for a new invite");
  startup.Cleanup(client);
}

}  // namespace
}  // namespace rex::kernel::xam::live

TEST_CASE("Session status reports a hosted or joined session", "[live_invite]") {
  namespace lan = rex::kernel::xam::lan;
  using rex::kernel::xam::live::SessionStatusFrom;
  CHECK_FALSE(SessionStatusFrom(lan::SessionSnapshot{}).active);

  lan::SessionSnapshot hosted;
  hosted.created = true;
  hosted.flags = 0x1;
  hosted.max_public_slots = 3;
  hosted.max_private_slots = 1;
  auto status = SessionStatusFrom(hosted);
  CHECK(status.active);
  CHECK(status.host);
  CHECK(status.members == 1);
  CHECK(status.max_members == 4);

  hosted.joined_public = 2;
  CHECK(SessionStatusFrom(hosted).members == 2);

  lan::SessionSnapshot joined;
  joined.created = true;
  joined.flags = 0x8;
  joined.max_public_slots = 4;
  joined.joined_public = 1;
  joined.filled_public = 1;
  status = SessionStatusFrom(joined);
  CHECK(status.active);
  CHECK_FALSE(status.host);
  CHECK(status.members == 2);
  CHECK(status.max_members == 4);

  joined.filled_public = 9;
  CHECK(SessionStatusFrom(joined).members == 4);
}
