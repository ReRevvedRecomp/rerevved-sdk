#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>
#include <rex/system/xsocket.h>

#include "kernel/xam/lan_system_link.h"
#include "kernel/xam/live_sessions.h"
#include "kernel/xam/relay_system_link.h"
#include "kernel/xam/system_link_config.h"
#include "test_memory.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#endif

extern "C" REX_FUNC(__imp__NetDll_XNetQosLookup);

namespace rex::kernel::xam::live {
namespace {
using namespace std::chrono_literals;
using relay_link::ProbePoll;
using relay_link::RecordsPoll;
using relay_link::RequestFailure;
using relay_link::RequestHandle;
using relay_link::RequestState;

uint32_t Address(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const uint8_t bytes[4]{a, b, c, d};
  uint32_t value = 0;
  std::memcpy(&value, bytes, 4);
  return value;
}
std::string Dotted(uint32_t ipv4) {
  uint8_t b[4];
  std::memcpy(b, &ipv4, 4);
  return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
         std::to_string(b[3]);
}
const uint32_t kHostA = Address(198, 18, 0, 10);
const uint32_t kHostB = Address(198, 18, 0, 11);
const uint32_t kNative = Address(192, 168, 1, 10);

uint32_t Load32(const uint8_t* p) {
  return memory::load_and_swap<uint32_t>(p);
}
uint16_t Load16(const uint8_t* p) {
  return memory::load_and_swap<uint16_t>(p);
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

class FakeDirectory final : public Directory {
 public:
  bool live = true;
  bool publish_ok = true;
  bool remove_ok = true;
  bool probe_ok = true;
  std::vector<relay::SessionRecord> published;
  std::vector<uint64_t> removed;
  std::vector<std::pair<uint16_t, uint32_t>> searches;
  std::vector<std::pair<uint32_t, unsigned>> probe_targets;
  std::map<RequestHandle, RecordsPoll> search_polls;
  std::map<RequestHandle, ProbePoll> probe_polls;

  bool Live() const override { return live; }
  bool Publish(const relay::SessionRecord& record) override {
    published.push_back(record);
    return publish_ok;
  }
  bool Remove(uint64_t id) override {
    removed.push_back(id);
    return remove_ok;
  }
  RequestHandle BeginSearch(uint16_t max_results, uint32_t flags) override {
    searches.emplace_back(max_results, flags);
    const auto handle = ++next_;
    search_polls[handle].state = RequestState::kPending;
    return handle;
  }
  RecordsPoll PollRecords(RequestHandle handle) override {
    auto found = search_polls.find(handle);
    if (found == search_polls.end())
      return {};
    auto poll = found->second;
    if (poll.state != RequestState::kPending)
      search_polls.erase(found);
    return poll;
  }
  RequestHandle BeginProbe(uint32_t ipv4, unsigned count) override {
    if (!probe_ok)
      return 0;
    probe_targets.emplace_back(ipv4, count);
    const auto handle = ++next_;
    probe_polls[handle].state = RequestState::kPending;
    return handle;
  }
  ProbePoll PollProbe(RequestHandle handle) override {
    auto found = probe_polls.find(handle);
    if (found == probe_polls.end())
      return {};
    auto poll = found->second;
    if (poll.state != RequestState::kPending)
      probe_polls.erase(found);
    return poll;
  }
  RequestHandle last() const { return next_; }
  void FinishSearch(RequestHandle handle, std::vector<relay::SessionRecord> records) {
    auto& poll = search_polls[handle];
    poll.state = RequestState::kDone;
    poll.failure = RequestFailure::kNone;
    poll.records = std::move(records);
  }
  void FailSearch(RequestHandle handle, RequestFailure failure) {
    auto& poll = search_polls[handle];
    poll.state = RequestState::kFailed;
    poll.failure = failure;
  }
  void FinishProbe(RequestHandle handle, unsigned sent, std::vector<uint32_t> rtt) {
    auto& poll = probe_polls[handle];
    poll.state = RequestState::kDone;
    poll.failure = RequestFailure::kNone;
    poll.sent = sent;
    poll.rtt_ms = std::move(rtt);
  }

 private:
  RequestHandle next_ = 0;
};

struct Recorder {
  std::mutex mutex;
  std::vector<uint32_t> prepared;
  std::vector<std::pair<uint32_t, uint32_t>> completed;
  std::vector<uint32_t> signaled;
  GuestHooks hooks() {
    GuestHooks result;
    result.prepare = [this](uint32_t ov) {
      std::lock_guard lock(mutex);
      prepared.push_back(ov);
    };
    result.complete = [this](uint32_t ov, uint32_t status) {
      std::lock_guard lock(mutex);
      completed.emplace_back(ov, status);
    };
    result.signal = [this](uint32_t event) {
      std::lock_guard lock(mutex);
      signaled.push_back(event);
    };
    return result;
  }
  size_t completions() {
    std::lock_guard lock(mutex);
    return completed.size();
  }
  size_t signals() {
    std::lock_guard lock(mutex);
    return signaled.size();
  }
};

const Ethernet kEthernet{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

relay::SessionRecord MakeRecord(uint64_t id, uint32_t ipv4, std::vector<Context> contexts,
                                std::string_view name = "Host-1") {
  relay::SessionRecord record;
  record.session_id = id;
  record.host_ipv4 = ipv4;
  record.key.fill(0x6B);
  record.flags = kCreateHost | kCreateMatchmaking;
  record.max_public = 4;
  record.open_public = 3;
  record.max_private = 2;
  record.open_private = 1;
  const std::vector<Property> properties{{kPropertyGamerHostName, HostNameValue(name)},
                                         {0x10000001, {0, 0, 0, 7}}};
  record.properties = EncodeProperties(kEthernet, properties, nullptr);
  record.contexts = EncodeContexts(contexts, nullptr);
  record.qos = {9, 8, 7};
  return record;
}

lan::SessionSnapshot HostSession(uint64_t id, uint32_t flags = kCreateHost | kCreateMatchmaking) {
  lan::SessionSnapshot session;
  session.created = true;
  session.flags = flags;
  session.max_public_slots = 4;
  session.max_private_slots = 0;
  session.joined_public = 1;
  for (int i = 0; i < 8; ++i)
    session.info[i] = static_cast<uint8_t>(id >> (56 - 8 * i));
  std::memcpy(session.info.data() + 8, &kHostA, 4);
  std::copy(kEthernet.begin(), kEthernet.end(), session.info.begin() + 18);
  std::fill(session.info.begin() + 44, session.info.end(), uint8_t{0x5A});
  return session;
}

// Every count and pointer in the written results stays inside the buffer.
bool InBounds(Scratch& scratch, uint32_t offset, uint32_t size) {
  const uint64_t base = scratch.addr(offset);
  const uint64_t end = base + size;
  const auto* header = scratch.at(offset);
  const uint32_t count = Load32(header);
  if (!count)
    return Load32(header + 4) == 0;
  const uint32_t results = Load32(header + 4);
  auto inside = [&](uint64_t p, uint64_t n) { return p >= base && p + n <= end; };
  if (results != base + kSearchHeaderSize || !inside(results, uint64_t(count) * kSearchResultSize))
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    const auto* result = scratch.memory.TranslateVirtual<uint8_t*>(results + kSearchResultSize * i);
    const uint32_t properties = Load32(result + 76);
    const uint32_t contexts = Load32(result + 80);
    const uint32_t properties_ptr = Load32(result + 84);
    const uint32_t contexts_ptr = Load32(result + 88);
    if (properties && !inside(properties_ptr, uint64_t(properties) * kPropertyEntrySize))
      return false;
    if (contexts && !inside(contexts_ptr, uint64_t(contexts) * kContextEntrySize))
      return false;
    for (uint32_t p = 0; p < properties; ++p) {
      const auto* slot =
          scratch.memory.TranslateVirtual<uint8_t*>(properties_ptr + kPropertyEntrySize * p);
      if (slot[8] == 4 || slot[8] == 6) {
        const uint32_t length = Load32(slot + 16);
        if (length && !inside(Load32(slot + 20), length))
          return false;
      }
    }
  }
  return true;
}

void PutSearch(uint8_t* request, uint32_t num_results, uint16_t num_contexts,
               uint32_t contexts_ptr, uint32_t buffer_size, uint32_t results_ptr) {
  std::memset(request, 0, 32);
  Store32(request + 8, num_results);
  memory::store_and_swap<uint16_t>(request + 14, num_contexts);
  Store32(request + 20, contexts_ptr);
  Store32(request + 24, buffer_size);
  Store32(request + 28, results_ptr);
}

// Mirrors system_link_config_test's fixture: a fresh snapshot and default flags.
struct ConfigFixture {
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("live-sessions-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::path path = directory / "fixture";
  ConfigFixture() {
    REQUIRE(std::filesystem::create_directory(directory));
    system_link::testing::ResetSnapshot();
    rex::cvar::testing::ResetAllForTesting();
    Arm();
  }
  ~ConfigFixture() {
    testing::SetService(nullptr);
    Shutdown();
    system_link::testing::ResetSnapshot();
    rex::cvar::testing::ResetAllForTesting();
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(directory, error);
  }
  void Set(std::string_view key, std::string_view value) {
    REQUIRE(rex::cvar::SetFlagByName(key, value));
  }
  void Relay() {
    Set("system_link_relay_address", "127.0.0.1:30100");
    Set("system_link_relay_room", "room");
    Set("system_link_relay_secret_file", "secret_file");
  }
  void Freeze() {
    system_link::FreezeConfig(path, [] { return rex::relay::Digest{0xCD, 0xAB, 2, 3, 4, 5}; });
  }
};

TEST_CASE("live_sessions option is a player Online setting off by default", "[live_sessions]") {
  ConfigFixture f;
  const auto* flag = rex::cvar::GetFlagInfo("live_sessions");
  REQUIRE(flag);
  CHECK(flag->type == rex::cvar::FlagType::Boolean);
  CHECK(flag->category == "Online");
  CHECK(flag->default_value == "false");
  CHECK(flag->audience == rex::cvar::Audience::kPlayer);
  CHECK(flag->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);
  CHECK(flag->persistence == rex::cvar::Persistence::kConfig);
  CHECK(rex::cvar::GetFlagByName("live_sessions") == "false");
  CHECK_FALSE(rex::cvar::SetFlagByName("live_sessions", "maybe"));
  f.Freeze();
  CHECK_FALSE(system_link::CurrentConfig().live_sessions);
  CHECK_FALSE(system_link::LiveSessionsActive());
  CHECK(system_link::Status() == "Off");
}

TEST_CASE("live_sessions is active only in a valid relay configuration", "[live_sessions]") {
  ConfigFixture f;
  auto& memory = rex::testing::GetTestMemory();
  FakeDirectory directory;
  Service service(memory, directory, {}, false);
  testing::SetService(&service);

  SECTION("off mode reports inactive") {
    f.Set("live_sessions", "true");
    f.Freeze();
    CHECK(system_link::Status() == "Off (Live sessions inactive)");
    CHECK_FALSE(system_link::LiveSessionsActive());
    CHECK(ActiveService() == nullptr);
  }
  SECTION("LAN mode reports inactive") {
    f.Set("live_sessions", "true");
    f.Set("system_link_lan_enabled", "true");
    f.Set("system_link_local_address", "127.0.0.1");
    f.Freeze();
    CHECK(system_link::Status() == "LAN (Live sessions inactive)");
    CHECK_FALSE(system_link::LiveSessionsActive());
    CHECK(ActiveService() == nullptr);
  }
  SECTION("failed relay settings stay inactive") {
    f.Set("live_sessions", "true");
    f.Set("system_link_relay_address", "127.0.0.1:30100");
    f.Freeze();
    CHECK(system_link::Status().starts_with("Relay failed: "));
    CHECK(ActiveService() == nullptr);
  }
  SECTION("relay without the option stays inactive") {
    f.Relay();
    f.Freeze();
    CHECK(system_link::Status().find("Live") == std::string::npos);
    CHECK(ActiveService() == nullptr);
  }
  SECTION("relay with the option is active and frozen") {
    f.Relay();
    f.Set("live_sessions", "true");
    f.Freeze();
    f.Set("live_sessions", "false");
    CHECK(system_link::CurrentConfig().live_sessions);
    CHECK(system_link::LiveSessionsActive());
    CHECK(ActiveService().get() == &service);
    CHECK(system_link::Status().find("Live") == std::string::npos);
  }
}

TEST_CASE("Inactive Live sessions keep the XNetQosLookup stub", "[live_sessions]") {
  ConfigFixture f;
  Scratch scratch(4096);
  Store32(scratch.at(0), 0xA5A5A5A5);
  for (bool lan : {false, true}) {
    system_link::testing::ResetSnapshot();
    f.Set("live_sessions", "true");
    f.Set("system_link_lan_enabled", lan ? "true" : "false");
    f.Set("system_link_local_address", "127.0.0.1");
    f.Freeze();
    // The stub leaves every register byte, including r3's upper word, unchanged.
    PPCContext ctx;
    std::memset(&ctx, 0x5C, sizeof(ctx));
    ctx.r3.u64 = 0xFFFFFFFF00001234ull;
    ctx.r4.u64 = 1;
    ctx.r5.u64 = scratch.addr(64);
    PPCContext before;
    std::memcpy(&before, &ctx, sizeof(ctx));
    __imp__NetDll_XNetQosLookup(ctx, scratch.memory.virtual_membase());
    CHECK(std::memcmp(&before, &ctx, sizeof(ctx)) == 0);
    CHECK(Load32(scratch.at(0)) == 0xA5A5A5A5);
  }
}

// Requests never complete, so a service using it always has pending work.
class StalledDirectory final : public Directory {
 public:
  explicit StalledDirectory(std::atomic<bool>& destroyed) : destroyed_(destroyed) {}
  ~StalledDirectory() override { destroyed_ = true; }
  bool Live() const override { return true; }
  bool Publish(const relay::SessionRecord&) override { return true; }
  bool Remove(uint64_t) override { return true; }
  RequestHandle BeginSearch(uint16_t, uint32_t) override { return 1; }
  RecordsPoll PollRecords(RequestHandle) override {
    RecordsPoll poll;
    poll.state = RequestState::kPending;
    return poll;
  }
  RequestHandle BeginProbe(uint32_t, unsigned) override { return 2; }
  ProbePoll PollProbe(RequestHandle) override {
    ProbePoll poll;
    poll.state = RequestState::kPending;
    return poll;
  }

 private:
  std::atomic<bool>& destroyed_;
};

TEST_CASE("Live shutdown stops the service with work pending", "[live_sessions]") {
  ConfigFixture f;
  f.Relay();
  f.Set("live_sessions", "true");
  f.Freeze();
  Scratch scratch(SearchBufferSize(1) + 4096);
  Recorder recorder;
  std::atomic<bool> destroyed = false;
  auto directory = std::make_unique<StalledDirectory>(destroyed);
  auto service = std::make_unique<Service>(scratch.memory, *directory, recorder.hooks(), true);
  Service* raw = service.get();
  testing::Adopt(std::move(directory), std::move(service));
  REQUIRE(ActiveService().get() == raw);

  PutSearch(scratch.at(0), 1, 0, 0, SearchBufferSize(1), scratch.addr(2048));
  Store32(scratch.at(2048), 0xA5A5A5A5);
  {
    DispatchScope dispatch(scratch.addr(64));
    REQUIRE(HandleSearch(*raw, scratch.memory, scratch.addr(0), 32) == kErrorIoPending);
  }
  std::memcpy(scratch.at(512), &kHostA, 4);
  Store32(scratch.at(256), scratch.addr(512));
  REQUIRE(HandleQosLookup(*raw, scratch.memory, 1, scratch.addr(256), 0, 0, 2, 0x77,
                          scratch.addr(320)) == 0);
  const uint32_t xnqos = Load32(scratch.at(320));
  REQUIRE(raw->Busy());
  std::this_thread::sleep_for(30ms);

  // A call admitted before shutdown keeps the instance alive through it.
  auto admitted = ActiveService();
  REQUIRE(admitted.get() == raw);
  Shutdown();
  CHECK_FALSE(testing::Owned());
  CHECK_FALSE(destroyed);
  CHECK(ActiveService() == nullptr);
  admitted->SetContext(0, kContextGameType, 3, Clock::now());
  CHECK(admitted->Busy());
  std::this_thread::sleep_for(30ms);
  CHECK(recorder.completions() == 0);
  CHECK(recorder.signals() == 0);
  CHECK(Load32(scratch.at(2048)) == 0xA5A5A5A5);
  CHECK(Load32(scratch.memory.TranslateVirtual<uint8_t*>(xnqos + 4)) == 1);
  admitted.reset();
  CHECK(destroyed);

  // After shutdown nothing is admitted until the next kernel arms the service.
  std::atomic<bool> next_destroyed = false;
  auto next_directory = std::make_unique<StalledDirectory>(next_destroyed);
  auto next = std::make_unique<Service>(scratch.memory, *next_directory, recorder.hooks(), false);
  Service* next_raw = next.get();
  testing::Adopt(std::move(next_directory), std::move(next));
  CHECK(ActiveService() == nullptr);
  Arm();
  CHECK(ActiveService().get() == next_raw);
  Shutdown();
  CHECK(next_destroyed);
  CHECK(ActiveService() == nullptr);
  Shutdown();
  scratch.memory.SystemHeapFree(xnqos);
}

TEST_CASE("Live guest ranges must be committed and writable where written", "[live_sessions]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = memory.LookupHeap(0x10000000);
  REQUIRE(heap != nullptr);
  uint32_t reserved = 0;
  REQUIRE(heap->Alloc(8192, 4096, rex::memory::kMemoryAllocationReserve,
                      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                      &reserved));
  const std::vector<relay::SessionRecord> records{MakeRecord(1, kHostA, {})};
  CHECK(WriteSearchResults(memory, reserved, 4096, records) == 0);
  CHECK_FALSE(ReadSearchFilter(memory, reserved, 1).game_type);

  // The second page becomes read-only after its contents are written.
  Scratch scratch(8192);
  Store32(scratch.at(4096), kContextGameType);
  Store32(scratch.at(4100), 3);
  PutSearch(scratch.at(4096 + 64), 1, 0, 0, SearchBufferSize(1), scratch.addr(0));
  Store32(scratch.at(0), 0xA5A5A5A5);
  REQUIRE(heap->Protect(scratch.addr(4096), 4096, rex::memory::kMemoryProtectRead));
  CHECK(WriteSearchResults(memory, scratch.addr(0), 8192, records) == 0);
  CHECK(Load32(scratch.at(0)) == 0xA5A5A5A5);
  CHECK(WriteSearchResults(memory, scratch.addr(0), 4096, records) == 1);
  CHECK(ReadSearchFilter(memory, scratch.addr(4096), 1).game_type == 3u);
  FakeDirectory directory;
  Service service(memory, directory, {}, false);
  CHECK(HandleSearch(service, memory, scratch.addr(4096 + 64), 32) == 87);
  CHECK(directory.searches.empty());
}

TEST_CASE("Live search results keep only terminated unicode properties", "[live_sessions]") {
  Scratch scratch(SearchBufferSize(1) + 4096);
  auto record = MakeRecord(1, kHostA, {});
  const std::vector<Property> properties{
      // Fills its storage without a terminator.
      {kPropertyGamerHostName, {0, 'A', 0, 'b'}},
      {0x40000002, {0, 'A', 0}},
      {0x40000003, {0, 'A', 0, 0, 0, 'B', 0, 0}},
      {0x40000004, {}},
      {0x10000001, {0, 0, 0, 7}}};
  record.properties = EncodeProperties(kEthernet, properties, nullptr);
  REQUIRE(WriteSearchResults(scratch.memory, scratch.addr(0), SearchBufferSize(1), {&record, 1}) ==
          1);
  CHECK(InBounds(scratch, 0, SearchBufferSize(1)));
  const auto* result = scratch.at(8);
  REQUIRE(Load32(result + 76) == 2);
  const auto* unicode = scratch.memory.TranslateVirtual<uint8_t*>(Load32(result + 84));
  CHECK(Load32(unicode) == 0x40000003);
  REQUIRE(Load32(unicode + 16) == 4);
  const auto* text = scratch.memory.TranslateVirtual<uint8_t*>(Load32(unicode + 20));
  CHECK(text[1] == 'A');
  CHECK(text[2] == 0);
  CHECK(text[3] == 0);
  CHECK(Load32(unicode + kPropertyEntrySize) == 0x10000001);
}

TEST_CASE("Live removal is retried until the client accepts it", "[live_sessions]") {
  auto& memory = rex::testing::GetTestMemory();
  FakeDirectory directory;
  Service service(memory, directory, {}, false);
  const auto t0 = Clock::now();
  const uint64_t id = 0x0012345678ABCDEF;
  service.SessionChanged(HostSession(id), t0);
  REQUIRE(directory.published.size() == 1);
  directory.remove_ok = false;
  service.EndSession(t0);
  CHECK(directory.removed == std::vector<uint64_t>{id});
  CHECK_FALSE(service.hosted_session());
  CHECK(service.Busy());
  service.Pump(t0 + 10ms);
  CHECK(directory.removed.size() == 2);

  SECTION("accepted later") {
    directory.remove_ok = true;
    service.Pump(t0 + 20ms);
    CHECK(directory.removed.size() == 3);
    CHECK(service.counters().removes == 1);
    CHECK_FALSE(service.Busy());
    service.Pump(t0 + 30ms);
    CHECK(directory.removed.size() == 3);
  }
  SECTION("dropped with the association") {
    directory.live = false;
    service.Pump(t0 + 20ms);
    CHECK(directory.removed.size() == 2);
    CHECK_FALSE(service.Busy());
  }
}

TEST_CASE("Live joiner occupancy keeps the joined member's slot kind", "[live_sessions]") {
  Scratch scratch(SearchBufferSize(1) + 4096);
  FakeDirectory directory;
  Service service(scratch.memory, directory, {}, false);
  PutSearch(scratch.at(0), 1, 0, 0, SearchBufferSize(1), scratch.addr(2048));
  {
    DispatchScope dispatch(scratch.addr(64));
    REQUIRE(HandleSearch(service, scratch.memory, scratch.addr(0), 32) == kErrorIoPending);
  }
  // One unnamed public and one unnamed private member.
  directory.FinishSearch(directory.last(), {MakeRecord(0x0042, kHostA, {})});
  service.Pump(Clock::now());
  lan::LanSystemLinkState state;
  REQUIRE(state.Configure("198.18.0.11", 0xB13E000000000002));
  lan::CreateSession(state, scratch.memory, kCreateMatchmaking, 1, 0, 0, scratch.addr(2048 + 8), 0);
  AfterCreate(service, state, scratch.memory, scratch.addr(2048 + 8));
  CHECK(state.session_snapshot().filled_public == 1);
  CHECK(state.session_snapshot().filled_private == 1);

  // Naming the private member consumes the private count only.
  auto* request = scratch.at(256);
  memory::store_and_swap<uint64_t>(scratch.at(320), 0x0009000000000003);
  Store32(scratch.at(352), 1);
  Store32(request + 4, 1);
  Store32(request + 8, scratch.addr(320));
  Store32(request + 12, 0);
  Store32(request + 16, scratch.addr(352));
  lan::JoinSession(state, scratch.memory, 1, scratch.addr(352));
  AfterJoin(service, state, scratch.memory, scratch.addr(256));
  const auto session = state.session_snapshot();
  CHECK(session.filled_public == 1);
  CHECK(session.filled_private == 0);
  CHECK(session.joined_public + session.filled_public == 1);
  CHECK(session.joined_private + session.filled_private == 1);
}

TEST_CASE("Live joiner occupancy follows membership updates", "[live_sessions]") {
  Scratch scratch(SearchBufferSize(1) + 4096);
  FakeDirectory directory;
  Recorder recorder;
  Service service(scratch.memory, directory, recorder.hooks(), false);
  // The host's record: 4 public with 3 open, 2 private with 1 open.
  PutSearch(scratch.at(0), 1, 0, 0, SearchBufferSize(1), scratch.addr(2048));
  {
    DispatchScope dispatch(scratch.addr(64));
    REQUIRE(HandleSearch(service, scratch.memory, scratch.addr(0), 32) == kErrorIoPending);
  }
  directory.FinishSearch(directory.last(), {MakeRecord(0x0042, kHostA, {})});
  service.Pump(Clock::now());
  lan::LanSystemLinkState state;
  REQUIRE(state.Configure("198.18.0.11", 0xB13E000000000002));
  lan::CreateSession(state, scratch.memory, kCreateMatchmaking, 1, 0, 0, scratch.addr(2048 + 8), 0);
  AfterCreate(service, state, scratch.memory, scratch.addr(2048 + 8));
  auto open = [&]() -> std::pair<uint32_t, uint32_t> {
    Store32(scratch.at(512), lan::kSessionDetailsSize);
    REQUIRE(lan::FillSessionDetails(state, scratch.memory, scratch.addr(512), scratch.addr(1024)) ==
            0);
    return {Load32(scratch.at(1024 + 24)), Load32(scratch.at(1024 + 28))};
  };
  CHECK(open() == std::pair<uint32_t, uint32_t>{3, 1});

  auto* request = scratch.at(256);
  auto membership = [&](uint32_t xuids_ptr, uint32_t indices_ptr) {
    std::memset(request, 0, 20);
    Store32(request + 4, 1);
    Store32(request + 8, xuids_ptr);
    Store32(request + 12, indices_ptr);
  };
  // The local player joins: one more slot is taken.
  Store32(scratch.at(352), 0);
  membership(0, scratch.addr(352));
  lan::JoinSession(state, scratch.memory, 1, 0);
  AfterJoin(service, state, scratch.memory, scratch.addr(256));
  CHECK(open() == std::pair<uint32_t, uint32_t>{2, 1});
  // The host joins remotely: it was already counted by the record.
  memory::store_and_swap<uint64_t>(scratch.at(320), 0x0009000000000001);
  membership(scratch.addr(320), 0);
  lan::JoinSession(state, scratch.memory, 1, 0);
  AfterJoin(service, state, scratch.memory, scratch.addr(256));
  CHECK(open() == std::pair<uint32_t, uint32_t>{2, 1});
  // The host leaves, then a member this client never saw leaves.
  HandleLeave(service, state, scratch.memory, scratch.addr(256));
  CHECK(open() == std::pair<uint32_t, uint32_t>{3, 1});
  memory::store_and_swap<uint64_t>(scratch.at(320), 0x0009000000000002);
  HandleLeave(service, state, scratch.memory, scratch.addr(256));
  CHECK(open() == std::pair<uint32_t, uint32_t>{3, 2});
  // Further unknown leaves fall back to joined members and clamp at zero.
  HandleLeave(service, state, scratch.memory, scratch.addr(256));
  HandleLeave(service, state, scratch.memory, scratch.addr(256));
  CHECK(open() == std::pair<uint32_t, uint32_t>{4, 2});
}

TEST_CASE("Live record blobs round-trip and respect relay caps", "[live_sessions]") {
  const std::vector<Context> contexts{{kContextGameType, 3}, {kContextGameMode, 1}};
  uint64_t dropped = 0;
  auto bytes = EncodeContexts(contexts, &dropped);
  CHECK(dropped == 0);
  CHECK(DecodeContexts(bytes) == contexts);
  CHECK_FALSE(DecodeContexts(relay::Bytes(7)));
  std::vector<Context> many(20, Context{1, 2});
  CHECK(EncodeContexts(many, &dropped).size() == relay::kMaxRecordContexts);
  CHECK(dropped == 4);

  dropped = 0;
  const std::vector<Property> properties{{kPropertyGamerHostName, HostNameValue("Ab")},
                                         {0x10000001, {0, 0, 0, 5}},
                                         {0x60000002, std::vector<uint8_t>(500, 1)}};
  auto encoded = EncodeProperties(kEthernet, properties, &dropped);
  CHECK(dropped == 1);
  auto decoded = DecodeProperties(encoded);
  REQUIRE(decoded);
  CHECK(decoded->ethernet == kEthernet);
  REQUIRE(decoded->properties.size() == 2);
  CHECK(decoded->properties[0].value == std::vector<uint8_t>{0, 'A', 0, 'b', 0, 0});
  encoded.pop_back();
  CHECK_FALSE(DecodeProperties(encoded));
  CHECK(SearchBufferSize(200) == 265208);
  CHECK(SearchBufferSize(0xFFFFFFFF) == 0xFFFFFFFF);
}

TEST_CASE("Live search filters fail closed on game type and mode", "[live_sessions]") {
  const auto record = MakeRecord(0x1111, kHostA, {{kContextGameType, 3}, {kContextGameMode, 1}});
  CHECK(Accepts({}, record));
  CHECK(Accepts({3, std::nullopt, 0}, record));
  CHECK(Accepts({3, 1, 0}, record));
  CHECK_FALSE(Accepts({4, std::nullopt, 0}, record));
  CHECK_FALSE(Accepts({0, std::nullopt, 0}, record));
  CHECK_FALSE(Accepts({3, 2, 0}, record));
  CHECK_FALSE(Accepts({std::nullopt, std::nullopt, 0x1111}, record));
  const auto no_mode = MakeRecord(0x2222, kHostA, {{kContextGameType, 3}});
  CHECK_FALSE(Accepts({std::nullopt, 1, 0}, no_mode));
  CHECK_FALSE(Accepts({3, std::nullopt, 0}, MakeRecord(0x3333, kHostA, {})));
  auto ranked = record;
  ranked.flags |= kCreateArbitration;
  CHECK_FALSE(Accepts({}, ranked));
  auto private_only = record;
  private_only.flags = kCreateHost;
  CHECK_FALSE(Accepts({}, private_only));
  CHECK_FALSE(Accepts({}, MakeRecord(0x4444, kNative, {})));
  auto corrupt = record;
  corrupt.properties = {2, 0, 0};
  CHECK_FALSE(Accepts({}, corrupt));

  Scratch scratch(4096);
  Store32(scratch.at(0), 0x12345678);
  Store32(scratch.at(4), 9);
  Store32(scratch.at(8), kContextGameMode);
  Store32(scratch.at(12), 2);
  Store32(scratch.at(16), kContextGameType);
  Store32(scratch.at(20), 0);
  const auto filter = ReadSearchFilter(scratch.memory, scratch.addr(0), 3);
  CHECK(filter.game_type == 0u);
  CHECK(filter.game_mode == 2u);
  CHECK_FALSE(ReadSearchFilter(scratch.memory, 0, 3).game_type);
}

TEST_CASE("Live search writer lays out 92-byte results inside the buffer", "[live_sessions]") {
  Scratch scratch(8192);
  const std::vector<relay::SessionRecord> records{
      MakeRecord(0x0123456789ABCDEF, kHostA, {{kContextGameType, 3}, {kContextGameMode, 1}}),
      MakeRecord(0x2222, kHostB, {})};
  const uint32_t size = SearchBufferSize(2);
  std::memset(scratch.at(0), 0xA5, scratch.size);
  REQUIRE(WriteSearchResults(scratch.memory, scratch.addr(0), size, records) == 2);
  CHECK(InBounds(scratch, 0, size));
  CHECK(std::all_of(scratch.at(size), scratch.at(scratch.size), [](uint8_t b) { return b == 0xA5; }));
  const auto* header = scratch.at(0);
  CHECK(Load32(header) == 2);
  const auto* result = scratch.at(8);
  CHECK(memory::load_and_swap<uint64_t>(result) == 0x0123456789ABCDEF);
  // XNADDR ina is the host's relay virtual address, stored as the relay reports it.
  CHECK(std::memcmp(result + 8, &kHostA, 4) == 0);
  CHECK(std::equal(kEthernet.begin(), kEthernet.end(), result + 18));
  CHECK(std::all_of(result + 44, result + 60, [](uint8_t b) { return b == 0x6B; }));
  CHECK(Load32(result + 60) == 3);
  CHECK(Load32(result + 64) == 1);
  CHECK(Load32(result + 68) == 1);
  CHECK(Load32(result + 72) == 1);
  REQUIRE(Load32(result + 76) == 2);
  REQUIRE(Load32(result + 80) == 2);
  const auto* host_name = scratch.memory.TranslateVirtual<uint8_t*>(Load32(result + 84));
  CHECK(Load32(host_name) == kPropertyGamerHostName);
  CHECK(host_name[8] == 4);
  const auto expected = HostNameValue("Host-1");
  REQUIRE(Load32(host_name + 16) == expected.size());
  CHECK(std::equal(expected.begin(), expected.end(),
                   scratch.memory.TranslateVirtual<uint8_t*>(Load32(host_name + 20))));
  const auto* number = host_name + kPropertyEntrySize;
  CHECK(Load32(number) == 0x10000001);
  CHECK(number[8] == 1);
  CHECK(Load32(number + 16) == 7);
  const auto* contexts = scratch.memory.TranslateVirtual<uint8_t*>(Load32(result + 88));
  CHECK(Load32(contexts) == kContextGameType);
  CHECK(Load32(contexts + 4) == 3);
  CHECK(Load32(contexts + 8) == kContextGameMode);
  CHECK(Load32(contexts + 12) == 1);
  const auto* second = result + kSearchResultSize;
  CHECK(std::memcmp(second + 8, &kHostB, 4) == 0);
  CHECK(Load32(second + 80) == 0);
  CHECK(Load32(second + 88) == 0);
}

TEST_CASE("Live search writer fills 200 results and drops what does not fit", "[live_sessions]") {
  Scratch scratch(SearchBufferSize(200) + 4096);
  std::vector<relay::SessionRecord> records;
  for (uint64_t i = 1; i <= 201; ++i)
    records.push_back(MakeRecord(i, kHostA, {{kContextGameType, 3}, {kContextGameMode, 1}},
                                 "Player-" + std::to_string(i)));
  std::memset(scratch.at(0), 0xA5, scratch.size);
  REQUIRE(SearchBufferSize(200) == 265208);
  CHECK(WriteSearchResults(scratch.memory, scratch.addr(0), 265208, records) == 200);
  CHECK(InBounds(scratch, 0, 265208));
  CHECK(std::all_of(scratch.at(265208), scratch.at(scratch.size),
                    [](uint8_t b) { return b == 0xA5; }));

  // Records with the most guest storage per blob byte overflow 1326 bytes each.
  std::vector<Property> nulls;
  for (uint32_t i = 0; i < 62; ++i)
    nulls.push_back({0xF0000000 | i, {}});
  for (auto& record : records)
    record.properties = EncodeProperties(kEthernet, nulls, nullptr);
  std::memset(scratch.at(0), 0xA5, scratch.size);
  const uint32_t written = WriteSearchResults(scratch.memory, scratch.addr(0), 265208, records);
  CHECK(written > 0);
  CHECK(written < 200);
  CHECK(InBounds(scratch, 0, 265208));
  CHECK(std::all_of(scratch.at(265208), scratch.at(scratch.size),
                    [](uint8_t b) { return b == 0xA5; }));

  // One record fits; the trailing one is dropped whole.
  std::memset(scratch.at(0), 0xA5, scratch.size);
  records = {MakeRecord(1, kHostA, {}), MakeRecord(2, kHostB, {})};
  CHECK(WriteSearchResults(scratch.memory, scratch.addr(0), 8 + 92 + 120, records) == 1);
  CHECK(InBounds(scratch, 0, 8 + 92 + 120));
  CHECK(std::all_of(scratch.at(8 + 92 + 120), scratch.at(4096),
                    [](uint8_t b) { return b == 0xA5; }));
  CHECK(WriteSearchResults(scratch.memory, scratch.addr(0), 8, records) == 0);
  CHECK(Load32(scratch.at(0)) == 0);
  CHECK(Load32(scratch.at(4)) == 0);
  std::memset(scratch.at(0), 0xA5, 16);
  CHECK(WriteSearchResults(scratch.memory, scratch.addr(0), 7, records) == 0);
  CHECK(Load32(scratch.at(0)) == 0xA5A5A5A5);
}

TEST_CASE("Live search size probe reports 8 plus 1326 per result", "[live_sessions]") {
  Scratch scratch(4096);
  FakeDirectory directory;
  Recorder recorder;
  Service service(scratch.memory, directory, recorder.hooks(), false);
  DispatchScope dispatch(scratch.addr(512));
  PutSearch(scratch.at(0), 10, 0, 0, 0, 0);
  CHECK(HandleSearch(service, scratch.memory, scratch.addr(0), 32) == 122);
  CHECK(Load32(scratch.at(24)) == 8 + 1326 * 10);
  PutSearch(scratch.at(0), 3, 0, 0, 7, scratch.addr(1024));
  Store32(scratch.at(1024), 0xA5A5A5A5);
  CHECK(HandleSearch(service, scratch.memory, scratch.addr(0), 32) == 122);
  CHECK(Load32(scratch.at(24)) == 8 + 1326 * 3);
  CHECK(Load32(scratch.at(1024)) == 0xA5A5A5A5);
  CHECK(HandleSearch(service, scratch.memory, scratch.addr(0), 16) == 87);
  CHECK_FALSE(dispatch.deferred());
  CHECK(directory.searches.empty());
  CHECK(recorder.prepared.empty());
}

struct SearchHarness {
  Scratch scratch{SearchBufferSize(4) + 4096};
  FakeDirectory directory;
  Recorder recorder;
  Service service{scratch.memory, directory, recorder.hooks(), false};
  static constexpr uint32_t kOverlapped = 64;
  static constexpr uint32_t kContexts = 128;
  static constexpr uint32_t kResults = 256;
  uint32_t buffer = SearchBufferSize(4);

  uint32_t Begin(uint32_t game_type, bool overlapped = true) {
    Store32(scratch.at(kContexts), kContextGameType);
    Store32(scratch.at(kContexts + 4), game_type);
    PutSearch(scratch.at(0), 4, 1, scratch.addr(kContexts), buffer, scratch.addr(kResults));
    Store32(scratch.at(kResults), 0xA5A5A5A5);
    DispatchScope dispatch(overlapped ? scratch.addr(kOverlapped) : 0);
    const uint32_t status = HandleSearch(service, scratch.memory, scratch.addr(0), 32);
    CHECK(dispatch.deferred() == (status == kErrorIoPending));
    return status;
  }
  uint32_t count() { return Load32(scratch.at(kResults)); }
};

TEST_CASE("Live search completes its deferred overlapped", "[live_sessions]") {
  SearchHarness h;
  REQUIRE(h.Begin(3) == kErrorIoPending);
  CHECK(h.directory.searches == std::vector<std::pair<uint16_t, uint32_t>>{{4, kCreateMatchmaking}});
  CHECK(h.recorder.prepared == std::vector<uint32_t>{h.scratch.addr(SearchHarness::kOverlapped)});
  h.service.Pump(Clock::now());
  CHECK(h.recorder.completions() == 0);
  CHECK(h.count() == 0xA5A5A5A5);

  SECTION("results are filtered and written") {
    h.directory.FinishSearch(
        h.directory.last(),
        {MakeRecord(1, kHostA, {{kContextGameType, 3}}), MakeRecord(2, kHostA, {{kContextGameType, 4}}),
         MakeRecord(3, kHostB, {{kContextGameType, 3}})});
    h.service.Pump(Clock::now());
    REQUIRE(h.recorder.completed.size() == 1);
    CHECK(h.recorder.completed[0] ==
          std::pair<uint32_t, uint32_t>{h.scratch.addr(SearchHarness::kOverlapped), 0});
    CHECK(h.count() == 2);
    CHECK(InBounds(h.scratch, SearchHarness::kResults, h.buffer));
    CHECK(h.service.FoundRecord(1));
    CHECK_FALSE(h.service.FoundRecord(2));
    CHECK(h.service.counters().results_dropped == 1);
  }
  SECTION("timeout completes with no results and keeps polling the request") {
    h.service.Pump(Clock::now() + std::chrono::milliseconds(kSearchTimeoutMs - 500));
    CHECK(h.recorder.completions() == 0);
    h.service.Pump(Clock::now() + std::chrono::milliseconds(kSearchTimeoutMs + 100));
    REQUIRE(h.recorder.completions() == 1);
    CHECK(h.recorder.completed[0].second == 0);
    CHECK(h.count() == 0);
    CHECK(h.service.counters().search_timeouts == 1);
    CHECK(h.service.Busy());
    h.directory.FinishSearch(h.directory.last(), {MakeRecord(1, kHostA, {})});
    h.service.Pump(Clock::now());
    CHECK_FALSE(h.service.Busy());
    CHECK(h.recorder.completions() == 1);
    CHECK_FALSE(h.service.FoundRecord(1));
  }
  SECTION("association loss completes with no results") {
    h.directory.FailSearch(h.directory.last(), RequestFailure::kAssociation);
    h.service.Pump(Clock::now());
    REQUIRE(h.recorder.completions() == 1);
    CHECK(h.recorder.completed[0].second == 0);
    CHECK(h.count() == 0);
    CHECK(h.service.counters().search_failures == 1);
  }
  SECTION("XNetCleanup completes with no results") {
    h.service.Cleanup();
    REQUIRE(h.recorder.completions() == 1);
    CHECK(h.count() == 0);
  }
  SECTION("cancel completes as cancelled once") {
    CHECK(h.service.Cancel(h.scratch.addr(SearchHarness::kOverlapped)));
    CHECK_FALSE(h.service.Cancel(h.scratch.addr(SearchHarness::kOverlapped)));
    h.directory.FinishSearch(h.directory.last(), {MakeRecord(1, kHostA, {})});
    h.service.Pump(Clock::now());
    REQUIRE(h.recorder.completions() == 1);
    CHECK(h.recorder.completed[0].second == kErrorCancelled);
    CHECK(h.count() == 0xA5A5A5A5);
  }
}

TEST_CASE("Live search returns nothing for ranked or synchronous requests", "[live_sessions]") {
  SearchHarness h;
  CHECK(h.Begin(0) == 0);
  CHECK(h.count() == 0);
  CHECK(h.Begin(3, false) == 0);
  CHECK(h.count() == 0);
  CHECK(h.directory.searches.empty());
  CHECK(h.recorder.prepared.empty());
}

TEST_CASE("Live host publication follows the hosted session", "[live_sessions]") {
  auto& memory = rex::testing::GetTestMemory();
  FakeDirectory directory;
  Service service(memory, directory, {}, false);
  const auto t0 = Clock::now();
  service.SetHostName("Host-1");
  service.SetContext(0, kContextGameType, 3, t0);
  service.SetProperty(0, 0x10000001, std::vector<uint8_t>{0, 0, 0, 5}, t0);
  service.SetProperty(0, kPropertyGamerHostName, HostNameValue("Spoofed"), t0);
  service.SetQosListen(std::vector<uint8_t>{1, 2, 3}, t0);
  CHECK(directory.published.empty());

  const uint64_t id = 0x0012345678ABCDEF;
  auto session = HostSession(id);
  service.SessionChanged(session, t0);
  REQUIRE(directory.published.size() == 1);
  CHECK(service.hosted_session() == id);
  {
    const auto& record = directory.published[0];
    CHECK(record.session_id == id);
    CHECK(std::all_of(record.key.begin(), record.key.end(), [](uint8_t b) { return b == 0x5A; }));
    CHECK(record.flags == (kCreateHost | kCreateMatchmaking));
    CHECK(record.max_public == 4);
    CHECK(record.open_public == 3);
    CHECK(record.qos == relay::Bytes{1, 2, 3});
    CHECK(DecodeContexts(record.contexts) == std::vector<Context>{{kContextGameType, 3}});
    auto properties = DecodeProperties(record.properties);
    REQUIRE(properties);
    CHECK(properties->ethernet == kEthernet);
    REQUIRE(properties->properties.size() == 2);
    CHECK(properties->properties[0] == Property{kPropertyGamerHostName, HostNameValue("Host-1")});
    CHECK(properties->properties[1].id == 0x10000001);
  }

  // Changes inside the republish interval coalesce into one update.
  service.SetContext(0, kContextGameMode, 1, t0 + 10ms);
  service.SetContext(1, kContextGameMode, 9, t0 + 10ms);
  session.joined_public = 2;
  service.SessionChanged(session, t0 + 20ms);
  service.Pump(t0 + std::chrono::milliseconds(kRepublishIntervalMs - 1));
  CHECK(directory.published.size() == 1);
  service.Pump(t0 + std::chrono::milliseconds(kRepublishIntervalMs));
  REQUIRE(directory.published.size() == 2);
  CHECK(directory.published[1].open_public == 2);
  CHECK(DecodeContexts(directory.published[1].contexts) ==
        std::vector<Context>{{kContextGameType, 3}, {kContextGameMode, 1}});
  service.Pump(t0 + 1s);
  CHECK(directory.published.size() == 2);

  SECTION("XSessionEnd and delete remove it for good") {
    service.EndSession(t0 + 1s);
    CHECK(directory.removed == std::vector<uint64_t>{id});
    CHECK_FALSE(service.hosted_session());
    service.SessionChanged(session, t0 + 2s);
    service.Pump(t0 + 3s);
    CHECK(directory.published.size() == 2);
    service.EndSession(t0 + 3s);
    CHECK(directory.removed.size() == 1);
  }
  SECTION("XNetCleanup removes it") {
    service.Cleanup();
    CHECK(directory.removed == std::vector<uint64_t>{id});
    CHECK_FALSE(service.hosted_session());
  }
  SECTION("a different or non-host session replaces it") {
    service.SessionChanged(HostSession(id + 1), t0 + 2s);
    CHECK(directory.removed == std::vector<uint64_t>{id});
    CHECK(service.hosted_session() == id + 1);
    service.SessionChanged({}, t0 + 3s);
    CHECK(directory.removed == std::vector<uint64_t>{id, id + 1});
  }
  SECTION("association loss drops it without a remove") {
    directory.live = false;
    service.Pump(t0 + 2s);
    CHECK(directory.removed.empty());
    CHECK_FALSE(service.hosted_session());
    CHECK(service.counters().association_losses == 1);
    service.Cleanup();
    CHECK(directory.removed.empty());
  }
}

TEST_CASE("Live publication skips ranked and non-matchmaking sessions", "[live_sessions]") {
  auto& memory = rex::testing::GetTestMemory();
  FakeDirectory directory;
  Service service(memory, directory, {}, false);
  const auto now = Clock::now();
  service.SessionChanged(HostSession(1, kCreateHost | kCreateMatchmaking | kCreateArbitration), now);
  service.SessionChanged(HostSession(2, kCreateHost), now);
  service.SessionChanged(HostSession(3, kCreateMatchmaking), now);
  service.Pump(now + 1s);
  CHECK(directory.published.empty());
  CHECK_FALSE(service.hosted_session());
}

TEST_CASE("Live join leave and modify republish the host record", "[live_sessions]") {
  Scratch scratch(4096);
  FakeDirectory directory;
  Service service(scratch.memory, directory, {}, false);
  lan::LanSystemLinkState state;
  REQUIRE(state.Configure("198.18.0.10", 0xB13E000000000001));
  lan::CreateSession(state, scratch.memory, kCreateHost | kCreateMatchmaking, 4, 2, 0,
                     scratch.addr(0), scratch.addr(64));
  AfterCreate(service, state, scratch.memory, scratch.addr(0));
  REQUIRE(directory.published.size() == 1);
  CHECK(directory.published[0].session_id == SessionId(state.session_snapshot().info));
  CHECK(directory.published[0].open_public == 4);
  CHECK(directory.published[0].open_private == 2);

  // Remote join of two XUIDs, the second into a private slot.
  auto* request = scratch.at(256);
  Store32(request + 4, 2);
  Store32(request + 8, scratch.addr(320));
  Store32(request + 12, 0);
  Store32(request + 16, scratch.addr(352));
  memory::store_and_swap<uint64_t>(scratch.at(320), 0x0009000000000001);
  memory::store_and_swap<uint64_t>(scratch.at(328), 0x0009000000000002);
  Store32(scratch.at(352), 0);
  Store32(scratch.at(356), 1);
  lan::JoinSession(state, scratch.memory, 2, scratch.addr(352));
  AfterJoin(service, state, scratch.memory, scratch.addr(256));
  service.Pump(Clock::now() + 1s);
  REQUIRE(directory.published.size() == 2);
  CHECK(directory.published[1].open_public == 3);
  CHECK(directory.published[1].open_private == 1);

  Store32(request + 4, 1);
  Store32(request + 8, scratch.addr(328));
  Store32(request + 16, 0);
  HandleLeave(service, state, scratch.memory, scratch.addr(256));
  CHECK(state.session_snapshot().joined_private == 0);
  CHECK(state.session_snapshot().joined_public == 1);
  service.Pump(Clock::now() + 2s);
  REQUIRE(directory.published.size() == 3);
  CHECK(directory.published[2].open_private == 2);

  // Modify changes join policy and slots but never the host or matchmaking bits.
  auto* modify = scratch.at(384);
  Store32(modify + 4, 0x100 | kCreateArbitration);
  Store32(modify + 8, 6);
  Store32(modify + 12, 0);
  HandleModify(service, state, scratch.memory, scratch.addr(384));
  service.Pump(Clock::now() + 3s);
  REQUIRE(directory.published.size() == 4);
  CHECK(directory.published[3].flags == (kCreateHost | kCreateMatchmaking | 0x100));
  CHECK(directory.published[3].max_public == 6);
  CHECK(directory.published[3].open_public == 5);
  CHECK(directory.published[3].max_private == 0);
}

TEST_CASE("Live joiner details report the host record's flags and open slots", "[live_sessions]") {
  Scratch scratch(SearchBufferSize(1) + 4096);
  FakeDirectory directory;
  Recorder recorder;
  Service service(scratch.memory, directory, recorder.hooks(), false);
  for (uint32_t host_flags : {kCreateHost | kCreateMatchmaking, kCreateHost | kCreateMatchmaking | 0x100u}) {
    auto record = MakeRecord(0x0042, kHostA, {{kContextGameType, 3}});
    record.flags = host_flags;
    PutSearch(scratch.at(0), 1, 0, 0, SearchBufferSize(1), scratch.addr(2048));
    {
      DispatchScope dispatch(scratch.addr(64));
      REQUIRE(HandleSearch(service, scratch.memory, scratch.addr(0), 32) == kErrorIoPending);
    }
    directory.FinishSearch(directory.last(), {record});
    service.Pump(Clock::now());
    REQUIRE(Load32(scratch.at(2048)) == 1);

    lan::LanSystemLinkState state;
    REQUIRE(state.Configure("198.18.0.11", 0xB13E000000000002));
    // The joiner creates from the search result's XSESSION_INFO.
    lan::CreateSession(state, scratch.memory, kCreateMatchmaking, 1, 0, 0, scratch.addr(2048 + 8),
                       0);
    AfterCreate(service, state, scratch.memory, scratch.addr(2048 + 8));
    Store32(scratch.at(512), lan::kSessionDetailsSize);
    REQUIRE(lan::FillSessionDetails(state, scratch.memory, scratch.addr(512), scratch.addr(1024)) == 0);
    const auto* details = scratch.at(1024);
    CHECK(Load32(details + 12) == (host_flags & ~kCreateHost));
    CHECK(Load32(details + 16) == 4);
    CHECK(Load32(details + 20) == 2);
    CHECK(Load32(details + 24) == 3);
    CHECK(Load32(details + 28) == 1);
    CHECK(std::equal(details + 52, details + 112, scratch.at(2048 + 8)));
    CHECK(directory.published.empty());
  }
}

struct QosHarness {
  Scratch scratch{8192};
  FakeDirectory directory;
  Recorder recorder;
  Service service{scratch.memory, directory, recorder.hooks(), false};
  static constexpr uint32_t kEvent = 0xF00D;

  // Caches a found record for kHostA so its listen data can be returned.
  void Find() {
    PutSearch(scratch.at(0), 1, 0, 0, SearchBufferSize(1), scratch.addr(4096));
    DispatchScope dispatch(scratch.addr(64));
    REQUIRE(HandleSearch(service, scratch.memory, scratch.addr(0), 32) == kErrorIoPending);
    directory.FinishSearch(directory.last(), {MakeRecord(0x0042, kHostA, {})});
    service.Pump(Clock::now());
    REQUIRE(service.FoundRecord(0x0042));
  }
  // XNADDR and XNKID pointer arrays at 256 and 288, the XNQOS pointer at 320.
  uint32_t Lookup(std::vector<uint32_t> addresses, uint32_t unsupported) {
    for (size_t i = 0; i < addresses.size(); ++i) {
      const uint32_t xnaddr = 512 + 64 * static_cast<uint32_t>(i);
      std::memcpy(scratch.at(xnaddr), &addresses[i], 4);
      Store32(scratch.at(256 + 4 * static_cast<uint32_t>(i)), scratch.addr(xnaddr));
      memory::store_and_swap<uint64_t>(scratch.at(xnaddr + 40), 0x0042);
      Store32(scratch.at(288 + 4 * static_cast<uint32_t>(i)), scratch.addr(xnaddr + 40));
    }
    REQUIRE(HandleQosLookup(service, scratch.memory, static_cast<uint32_t>(addresses.size()),
                            scratch.addr(256), scratch.addr(288), unsupported, 4, kEvent,
                            scratch.addr(320)) == 0);
    const uint32_t xnqos = Load32(scratch.at(320));
    REQUIRE(xnqos != 0);
    return xnqos;
  }
  uint8_t* info(uint32_t xnqos, uint32_t index) {
    return scratch.memory.TranslateVirtual<uint8_t*>(xnqos + kQosHeaderSize + kQosInfoSize * index);
  }
  uint32_t pending(uint32_t xnqos) {
    return Load32(scratch.memory.TranslateVirtual<uint8_t*>(xnqos + 4));
  }
};

TEST_CASE("Live QoS lookup measures reachable and unreachable hosts", "[live_sessions]") {
  QosHarness h;
  h.Find();
  const uint32_t xnqos = h.Lookup({kHostA, kHostB, kNative}, 1);
  CHECK(Load32(h.scratch.memory.TranslateVirtual<uint8_t*>(xnqos)) == 4);
  // Non-virtual and server targets complete at once.
  CHECK(h.pending(xnqos) == 2);
  CHECK(h.info(xnqos, 2)[0] == kQosComplete);
  CHECK(h.info(xnqos, 3)[0] == kQosComplete);
  CHECK(h.info(xnqos, 0)[0] == 0);
  REQUIRE(h.directory.probe_targets.size() == 2);
  CHECK(h.directory.probe_targets[0] == std::pair<uint32_t, unsigned>{kHostA, 4});
  CHECK(h.directory.probe_targets[1].first == kHostB);
  CHECK(h.recorder.signals() == 0);

  const RequestHandle first = h.directory.last() - 1;
  h.directory.FinishProbe(first, 3, {30, 10, 20});
  h.service.Pump(Clock::now());
  auto* reachable = h.info(xnqos, 0);
  CHECK(reachable[0] == (kQosComplete | kQosTargetContacted | kQosDataReceived));
  CHECK(Load16(reachable + 2) == 3);
  CHECK(Load16(reachable + 4) == 3);
  CHECK(Load16(reachable + 12) == 10);
  CHECK(Load16(reachable + 14) == 20);
  REQUIRE(Load16(reachable + 6) == 3);
  const uint32_t data = Load32(reachable + 8);
  CHECK(data >= xnqos + kQosHeaderSize + 4 * kQosInfoSize);
  CHECK(std::equal(h.scratch.memory.TranslateVirtual<uint8_t*>(data),
                   h.scratch.memory.TranslateVirtual<uint8_t*>(data) + 3,
                   std::vector<uint8_t>{9, 8, 7}.begin()));
  CHECK(h.pending(xnqos) == 1);
  CHECK(h.recorder.signals() == 0);

  h.directory.FinishProbe(first + 1, 4, {});
  h.service.Pump(Clock::now());
  auto* unreachable = h.info(xnqos, 1);
  CHECK(unreachable[0] == kQosComplete);
  CHECK(Load16(unreachable + 2) == 4);
  CHECK(Load16(unreachable + 4) == 0);
  CHECK(Load16(unreachable + 6) == 0);
  CHECK(Load32(unreachable + 8) == 0);
  CHECK(h.pending(xnqos) == 0);
  CHECK(h.recorder.signaled == std::vector<uint32_t>{QosHarness::kEvent});
  CHECK_FALSE(h.service.Busy());
  CHECK(h.service.counters().qos_reachable == 1);
  CHECK(h.service.counters().qos_unreachable == 3);
  h.service.ReleaseQos(xnqos);
  h.scratch.memory.SystemHeapFree(xnqos);
}

TEST_CASE("Live QoS lookup of only unreachable hosts completes at once", "[live_sessions]") {
  QosHarness h;
  h.directory.probe_ok = false;
  const uint32_t xnqos = h.Lookup({kHostA, kNative}, 0);
  CHECK(h.pending(xnqos) == 0);
  CHECK(h.info(xnqos, 0)[0] == kQosComplete);
  CHECK(h.info(xnqos, 1)[0] == kQosComplete);
  CHECK(h.recorder.signaled == std::vector<uint32_t>{QosHarness::kEvent});
  CHECK_FALSE(h.service.Busy());
  h.scratch.memory.SystemHeapFree(xnqos);
}

TEST_CASE("Live QoS release and cleanup stop pending lookups", "[live_sessions]") {
  QosHarness h;
  const uint32_t xnqos = h.Lookup({kHostA}, 0);
  CHECK(h.pending(xnqos) == 1);
  SECTION("release before completion") {
    h.service.ReleaseQos(xnqos);
    h.scratch.memory.SystemHeapFree(xnqos);
    CHECK(h.service.Busy());
    h.directory.FinishProbe(h.directory.last(), 4, {5});
    h.service.Pump(Clock::now());
    CHECK_FALSE(h.service.Busy());
    CHECK(h.recorder.signals() == 0);
  }
  SECTION("XNetCleanup completes it unreachable") {
    h.service.Cleanup();
    CHECK(h.pending(xnqos) == 0);
    CHECK(h.info(xnqos, 0)[0] == kQosComplete);
    CHECK(h.recorder.signals() == 1);
    h.scratch.memory.SystemHeapFree(xnqos);
  }
}

TEST_CASE("relay probe budget refills at its rate", "[live_sessions][relay_client]") {
  relay_link::TokenBucket bucket(relay_link::kProbesPerSecond);
  const auto t0 = Clock::now();
  int taken = 0;
  while (bucket.Take(t0))
    ++taken;
  CHECK(taken == 40);
  CHECK_FALSE(bucket.Take(t0 + 10ms));
  CHECK(bucket.Take(t0 + 30ms));
  CHECK_FALSE(bucket.Take(t0 + 30ms));
  taken = 0;
  while (bucket.Take(t0 + 10s))
    ++taken;
  CHECK(taken == 40);
}

bool Readable(system::XSocket& socket, std::chrono::milliseconds wait) {
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(socket.native_handle(), &readable);
  timeval timeout{0, static_cast<long>(wait.count() * 1000)};
  return select(static_cast<int>(socket.native_handle() + 1), &readable, nullptr, nullptr,
                &timeout) == 1;
}

struct LoopbackRelay {
  std::unique_ptr<relay::UdpSocket> socket;
  relay::Core core{{}, "secret", relay::Digest{42}};
  relay::TransportCounters counters;
  std::jthread pump;
  bool Open() {
    auto opened = relay::UdpSocket::Open("127.0.0.1", 0);
    if (!opened)
      return false;
    socket = std::move(*opened);
    pump = std::jthread([this](std::stop_token stop) {
      const auto start = std::chrono::steady_clock::now();
      while (!stop.stop_requested()) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        relay::Pump(*socket, core, now.count(), counters, 10);
      }
    });
    return true;
  }
  relay::Endpoint endpoint() const { return socket->endpoint(); }
};

ProbePoll WaitProbe(relay_link::Client& client, RequestHandle handle) {
  const auto deadline = Clock::now() + 5s;
  while (Clock::now() < deadline) {
    auto poll = client.PollProbe(handle);
    if (poll.state != RequestState::kPending)
      return poll;
    std::this_thread::sleep_for(1ms);
  }
  FAIL("Probe set did not complete");
  return {};
}

TEST_CASE("relay probes measure round trips without reaching guest sockets",
          "[live_sessions][relay_client][network]") {
  LoopbackRelay relay;
  if (!relay.Open())
    SKIP("Loopback UDP socket unavailable");
  system::XSocket guest(nullptr);
  if (guest.Initialize(system::XSocket::X_AF_INET, system::XSocket::X_SOCK_DGRAM,
                       system::XSocket::X_IPPROTO_UDP) != X_STATUS_SUCCESS)
    SKIP("Guest UDP socket unavailable");
  relay_link::Client prober, target;
  REQUIRE(prober.Start(relay.endpoint(), "live", "secret", {}));
  REQUIRE(target.Start(relay.endpoint(), "live", "secret", {}));
  REQUIRE(target.Bind(guest.native_handle(), 1000));

  // Replies are off by default, so the target looks unreachable.
  auto poll = WaitProbe(prober, prober.BeginProbe(target.ipv4(), 2));
  CHECK(poll.state == RequestState::kDone);
  CHECK(poll.sent == 2);
  CHECK(poll.rtt_ms.empty());
  CHECK(target.stats().probe_replies_dropped == 2);

  target.SetProbeReplies(true);
  const auto start = Clock::now();
  poll = WaitProbe(prober, prober.BeginProbe(target.ipv4(), 4));
  CHECK(poll.state == RequestState::kDone);
  CHECK(poll.sent == 4);
  CHECK(poll.rtt_ms.size() == 4);
  CHECK(Clock::now() - start < 1s);
  CHECK(target.stats().probe_replies_sent == 4);
  CHECK(prober.stats().probe_replies_received == 4);

  // An unassigned address and a non-virtual address are unreachable.
  poll = WaitProbe(prober, prober.BeginProbe(Address(198, 19, 255, 254), 1));
  CHECK(poll.state == RequestState::kDone);
  CHECK(poll.rtt_ms.empty());
  poll = prober.PollProbe(prober.BeginProbe(kNative, 1));
  CHECK(poll.state == RequestState::kFailed);
  CHECK(poll.failure == RequestFailure::kInvalid);

  // A guest datagram to port 0 is consumed as an invalid probe, never delivered.
  system::XSocket sender(nullptr);
  REQUIRE(sender.Initialize(system::XSocket::X_AF_INET, system::XSocket::X_SOCK_DGRAM,
                            system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
  const relay::Bytes junk{'R', 'X', 'Q', 'P', 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
  REQUIRE(prober.Send(sender.native_handle(), {target.ipv4(), 0}, junk));
  const auto invalid_deadline = Clock::now() + 2s;
  while (target.stats().invalid_probes == 0 && Clock::now() < invalid_deadline)
    std::this_thread::sleep_for(1ms);
  CHECK(target.stats().invalid_probes == 1);
  CHECK_FALSE(Readable(guest, 100ms));
  CHECK(target.stats().unknown_port == 0);

  // 64 probes exceed the 40-probe burst, so the last ones wait for budget.
  const auto burst_start = Clock::now();
  std::vector<RequestHandle> sets;
  for (int i = 0; i < 8; ++i)
    sets.push_back(prober.BeginProbe(target.ipv4(), relay_link::kMaxProbesPerSet));
  size_t answered = 0;
  for (auto handle : sets)
    answered += WaitProbe(prober, handle).rtt_ms.size();
  CHECK(answered == 64);
  CHECK(Clock::now() - burst_start >= 400ms);
  CHECK(prober.stats().probes_sent == 2 + 4 + 1 + 64);
  CHECK_FALSE(Readable(guest, 0ms));

  // Association loss fails pending sets.
  const auto pending = prober.BeginProbe(Address(198, 19, 255, 254), 1);
  prober.Stop();
  CHECK(prober.PollProbe(pending).failure == RequestFailure::kAssociation);
  target.Stop();
}

TEST_CASE("Two relay clients publish search measure join and talk",
          "[live_sessions][relay_client][network]") {
  LoopbackRelay relay;
  if (!relay.Open())
    SKIP("Loopback UDP socket unavailable");
  system::XSocket host_socket(nullptr), joiner_socket(nullptr);
  for (auto* socket : {&host_socket, &joiner_socket})
    if (socket->Initialize(system::XSocket::X_AF_INET, system::XSocket::X_SOCK_DGRAM,
                           system::XSocket::X_IPPROTO_UDP) != X_STATUS_SUCCESS)
      SKIP("Guest UDP socket unavailable");
  Scratch scratch(SearchBufferSize(4) + 8192);
  relay_link::Client host, joiner;
  REQUIRE(host.Start(relay.endpoint(), "live", "secret", {}));
  REQUIRE(joiner.Start(relay.endpoint(), "live", "secret", {}));
  host.SetProbeReplies(true);
  joiner.SetProbeReplies(true);
  REQUIRE(host.Bind(host_socket.native_handle(), 1000));

  auto host_directory = ClientDirectory(host);
  auto joiner_directory = ClientDirectory(joiner);
  Recorder host_hooks, joiner_hooks;
  Service host_service(scratch.memory, *host_directory, host_hooks.hooks(), true);
  Service joiner_service(scratch.memory, *joiner_directory, joiner_hooks.hooks(), true);
  host_service.SetHostName("Host-1");
  joiner_service.SetHostName("Joiner-1");

  lan::LanSystemLinkState host_state, joiner_state;
  REQUIRE(host_state.Configure(Dotted(host.ipv4()), 0xB13E000000000001));
  REQUIRE(joiner_state.Configure(Dotted(joiner.ipv4()), 0xB13E000000000002));
  host_service.SetContext(0, kContextGameType, 3, Clock::now());
  host_service.SetQosListen(std::vector<uint8_t>{0x51, 0x52, 0x53, 0x54}, Clock::now());
  lan::CreateSession(host_state, scratch.memory, kCreateHost | kCreateMatchmaking, 4, 0, 0,
                     scratch.addr(0), scratch.addr(64));
  lan::JoinSession(host_state, scratch.memory, 1, 0);
  AfterCreate(host_service, host_state, scratch.memory, scratch.addr(0));
  REQUIRE(host_service.hosted_session());
  const uint64_t session_id = *host_service.hosted_session();

  // XSessionSearch with game type 3 until the published record is listed.
  constexpr uint32_t kResults = 4096;
  auto search = [&]() -> uint32_t {
    Store32(scratch.at(128), kContextGameType);
    Store32(scratch.at(132), 3);
    PutSearch(scratch.at(96), 4, 1, scratch.addr(128), SearchBufferSize(4), scratch.addr(kResults));
    const size_t before = joiner_hooks.completions();
    {
      DispatchScope dispatch(scratch.addr(192));
      REQUIRE(HandleSearch(joiner_service, scratch.memory, scratch.addr(96), 32) == kErrorIoPending);
    }
    const auto deadline = Clock::now() + 5s;
    while (joiner_hooks.completions() == before && Clock::now() < deadline)
      std::this_thread::sleep_for(1ms);
    REQUIRE(joiner_hooks.completions() == before + 1);
    return Load32(scratch.at(kResults));
  };
  uint32_t found = 0;
  for (auto deadline = Clock::now() + 5s; !found && Clock::now() < deadline;) {
    found = search();
    if (!found)
      std::this_thread::sleep_for(100ms);
  }
  REQUIRE(found == 1);
  CHECK(InBounds(scratch, kResults, SearchBufferSize(4)));
  const auto* result = scratch.at(kResults + 8);
  CHECK(memory::load_and_swap<uint64_t>(result) == session_id);
  const uint32_t host_ina = host.ipv4();
  CHECK(std::memcmp(result + 8, &host_ina, 4) == 0);
  CHECK(std::equal(result + 18, result + 24, scratch.at(18)));
  CHECK(std::equal(result + 44, result + 60, scratch.at(44)));
  CHECK(Load32(result + 60) == 3);

  // QoS through the relay returns round trips and the host's listen data.
  Store32(scratch.at(256), scratch.addr(kResults + 8 + 8));
  Store32(scratch.at(260), scratch.addr(kResults + 8));
  REQUIRE(HandleQosLookup(joiner_service, scratch.memory, 1, scratch.addr(256), scratch.addr(260), 0,
                          2, 0x77, scratch.addr(264)) == 0);
  const uint32_t xnqos = Load32(scratch.at(264));
  const auto qos_deadline = Clock::now() + 5s;
  while (joiner_hooks.signals() == 0 && Clock::now() < qos_deadline)
    std::this_thread::sleep_for(1ms);
  REQUIRE(joiner_hooks.signals() == 1);
  const auto* info = scratch.memory.TranslateVirtual<uint8_t*>(xnqos + kQosHeaderSize);
  CHECK(info[0] == (kQosComplete | kQosTargetContacted | kQosDataReceived));
  CHECK(Load16(info + 4) == 2);
  REQUIRE(Load16(info + 6) == 4);
  CHECK(Load32(scratch.memory.TranslateVirtual<uint8_t*>(Load32(info + 8))) == 0x51525354);
  joiner_service.ReleaseQos(xnqos);
  scratch.memory.SystemHeapFree(xnqos);
  CHECK(host.stats().probe_replies_sent == 2);
  CHECK_FALSE(Readable(host_socket, 0ms));

  // The joiner's session reports the host's flags and true open slots.
  lan::CreateSession(joiner_state, scratch.memory, kCreateMatchmaking, 1, 0, 0,
                     scratch.addr(kResults + 8), 0);
  AfterCreate(joiner_service, joiner_state, scratch.memory, scratch.addr(kResults + 8));
  Store32(scratch.at(512), lan::kSessionDetailsSize);
  REQUIRE(lan::FillSessionDetails(joiner_state, scratch.memory, scratch.addr(512),
                                  scratch.addr(1024)) == 0);
  CHECK(Load32(scratch.at(1024 + 12)) == kCreateMatchmaking);
  CHECK(Load32(scratch.at(1024 + 16)) == 4);
  CHECK(Load32(scratch.at(1024 + 24)) == 3);

  // The listed XNADDR maps to the host's relay address and carries game traffic.
  lan::XnAddrSnapshot peer;
  std::memcpy(&peer.ipv4, result + 8, 4);
  uint32_t in_addr = 0;
  REQUIRE(joiner_state.XnAddrToInAddr(peer, &in_addr));
  CHECK(in_addr == host.ipv4());
  const relay::Bytes payload{1, 2, 3, 4};
  REQUIRE(joiner.Send(joiner_socket.native_handle(), {in_addr, 1000}, payload));
  REQUIRE(Readable(host_socket, 2000ms));
  std::array<uint8_t, relay::kMaxPayload> buffer{};
  relay::Endpoint source;
  auto received = host.Receive(host_socket.native_handle(), buffer, 0, source);
  REQUIRE(received);
  CHECK(relay::Bytes(buffer.begin(), buffer.begin() + *received) == payload);
  CHECK(source.ipv4 == joiner.ipv4());

  // Deleting the session removes it from the directory.
  host_service.EndSession(Clock::now());
  CHECK_FALSE(host_service.hosted_session());
  for (auto deadline = Clock::now() + 5s; found && Clock::now() < deadline;) {
    found = search();
    if (found)
      std::this_thread::sleep_for(100ms);
  }
  CHECK(found == 0);
  joiner.Stop();
  host.Stop();
}

}  // namespace
}  // namespace rex::kernel::xam::live
