#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/live_invite.h>

#include "lan_system_link.h"
#include "relay_system_link.h"
#include "system_link_config.h"

namespace rex::memory {
class Memory;
}  // namespace rex::memory

// Live-mode sessions served by the relay session directory. A host's
// matchmaking session is published, XSessionSearch lists other hosts in the
// room, and XNetQosLookup measures round trips through the relay.
namespace rex::kernel::xam::live {

using Clock = std::chrono::steady_clock;

// XSession create flags.
constexpr uint32_t kCreateHost = 0x00000001;
constexpr uint32_t kCreateMatchmaking = 0x00000008;
constexpr uint32_t kCreateArbitration = 0x00000010;
// Flags XSessionModify may change: invites and join policy.
constexpr uint32_t kModifiableFlags = 0x00000F00;
constexpr uint32_t kPropertyGamerHostName = 0x40008109;  // X_PROPERTY_GAMER_HOSTNAME
constexpr uint32_t kContextGameType = 0x0000800A;        // X_CONTEXT_GAME_TYPE, 0 is ranked
constexpr uint32_t kContextGameMode = 0x0000800B;        // X_CONTEXT_GAME_MODE

// Guest layouts, big-endian. XSESSION_SEARCHRESULT_HEADER is {count, results
// pointer}. XSESSION_SEARCHRESULT is XSESSION_INFO (XNKID[8], XNADDR[36],
// XNKEY[16]), open public, open private, filled public and filled private
// slots, property count, context count, property pointer, context pointer.
// XUSER_PROPERTY is {id, pad, XUSER_DATA}, where XUSER_DATA is {type:u8,
// pad[7], union[8]}; strings and binaries hold {byte count, pointer}.
// XUSER_CONTEXT is {id, value}. These are the public Xbox 360 shapes; the
// title's reads at these offsets are recorded in RVA-F-0243:
// https://github.com/ReRevvedRecomp/rerevved-research/blob/345d1d61120e702a43ed0bbdb1a8530dab6723e1/manifests/xbox-live-session-search-contract.json
constexpr uint32_t kSearchHeaderSize = 8;
constexpr uint32_t kSearchResultSize = 92;
constexpr uint32_t kSearchBytesPerResult = 1326;  // guest wrapper requirement per result
constexpr uint32_t kSessionInfoSize = 60;
constexpr uint32_t kPropertyEntrySize = 24;
constexpr uint32_t kContextEntrySize = 8;
constexpr uint32_t kMaxSearchResults = 200;
// XNQOS is {count, pending, XNQOSINFO[count]}; see xam_net.cpp.
constexpr uint32_t kQosHeaderSize = 8;
constexpr uint32_t kQosInfoSize = 24;
constexpr uint8_t kQosComplete = 0x01;
constexpr uint8_t kQosTargetContacted = 0x02;
constexpr uint8_t kQosDataReceived = 0x08;
constexpr uint32_t kMaxQosTargets = 1024;

// The relay client retransmits SEARCH every second and fails it after three
// seconds; 2.5 s covers three attempts and completes the guest first.
constexpr uint32_t kSearchTimeoutMs = 2500;
// Republishing at most four times a second stays inside the relay's ten
// updates per second, leaving room for REMOVE and token requests.
constexpr uint32_t kRepublishIntervalMs = 250;

// Accepted XN_LIVE_INVITE_ACCEPTED invites carry user index; mask bit 1 is
// XNOTIFY_LIVE. Big-endian XINVITE_INFO holds invitee/inviter XUID, title id,
// XSESSION_INFO at+0x14, and fFromGameInvite at+0x50.
constexpr uint32_t kNotifyLiveInviteAccepted = 0x02000002;
constexpr uint64_t kNotifyLiveArea = 0x2;
constexpr uint32_t kInviteInfoSize = 0x54;
constexpr uint32_t kInviteSessionInfoOffset = 0x14;
// A delivered invite the guest never searches for is dropped after this long.
constexpr uint32_t kInviteHoldMs = 60000;
// A startup lookup that fails is retried this many times, a second apart.
constexpr unsigned kInviteLookupAttempts = 5;
// Join tokens are replaced when less than this much lifetime remains.
constexpr uint32_t kInviteRefreshSeconds = 300;
constexpr uint32_t kInviteTokenRetryMs = 5000;

// XOVERLAPPED is seven big-endian words; the kernel writes all of them on completion.
constexpr uint32_t kOverlappedSize = 0x1C;

constexpr uint32_t kErrorInvalidParameter = 87;
constexpr uint32_t kErrorInsufficientBuffer = 122;
constexpr uint32_t kErrorIoPending = 997;
constexpr uint32_t kErrorCancelled = 1223;

struct Context {
  uint32_t id = 0;
  uint32_t value = 0;
  bool operator==(const Context&) const = default;
};
// Value bytes as the guest supplied them; the type comes from the id's top nibble.
struct Property {
  uint32_t id = 0;
  std::vector<uint8_t> value;
  bool operator==(const Property&) const = default;
};
using Ethernet = std::array<uint8_t, 6>;

// Relay treats SDK blobs as opaque; integers are big-endian. Contexts:
// {id:u32,value:u32}. Properties: version 1, host XNADDR Ethernet bytes, then
// {id:u32,length:u16,value}. Entries exceeding the cap are skipped and counted.
relay::Bytes EncodeContexts(std::span<const Context> contexts, uint64_t* dropped);
std::optional<std::vector<Context>> DecodeContexts(std::span<const uint8_t> bytes);
relay::Bytes EncodeProperties(const Ethernet& ethernet, std::span<const Property> properties,
                              uint64_t* dropped);
struct DecodedProperties {
  Ethernet ethernet{};
  std::vector<Property> properties;
};
std::optional<DecodedProperties> DecodeProperties(std::span<const uint8_t> bytes);
// UTF-16BE with a terminator, as X_PROPERTY_GAMER_HOSTNAME carries it.
std::vector<uint8_t> HostNameValue(std::string_view name);
uint64_t SessionId(std::span<const uint8_t> info);

// Contexts supplied by the guest for a search. Requested game type and mode
// must match the record; other contexts and properties are advisory.
struct SearchFilter {
  std::optional<uint32_t> game_type;
  std::optional<uint32_t> game_mode;
  uint64_t own_session = 0;
};
SearchFilter ReadSearchFilter(rex::memory::Memory& memory, uint32_t contexts_ptr, uint32_t count);
bool Accepts(const SearchFilter& filter, const relay::SessionRecord& record);

// 8 + 1326 bytes per result, saturating.
uint32_t SearchBufferSize(uint32_t num_results);
// Writes the header, as many whole records as fit and their property, context
// and payload storage, all inside [results_ptr, results_ptr + buffer_size).
// Returns the record count written; an unusable buffer is left untouched.
uint32_t WriteSearchResults(rex::memory::Memory& memory, uint32_t results_ptr, uint32_t buffer_size,
                            std::span<const relay::SessionRecord> records);

class Directory {
 public:
  virtual ~Directory() = default;
  virtual bool Live() const = 0;
  virtual bool Publish(const relay::SessionRecord& record) = 0;
  virtual bool Remove(uint64_t session_id) = 0;
  virtual relay_link::RequestHandle BeginSearch(uint16_t max_results, uint32_t required_flags) = 0;
  virtual relay_link::RecordsPoll PollRecords(relay_link::RequestHandle handle) = 0;
  virtual relay_link::RequestHandle BeginProbe(uint32_t ipv4, unsigned count) = 0;
  virtual relay_link::ProbePoll PollProbe(relay_link::RequestHandle handle) = 0;
  // Lookups poll through PollRecords. Directories without them start nothing.
  virtual relay_link::RequestHandle BeginLookup(uint64_t) { return 0; }
  virtual relay_link::RequestHandle BeginTokenRequest(uint64_t) { return 0; }
  virtual relay_link::TokenPoll PollToken(relay_link::RequestHandle) { return {}; }
};
std::unique_ptr<Directory> ClientDirectory(relay_link::Client& client);

// Guest-side effects. `prepare` marks an overlapped pending before the
// service owns it; `complete` finishes it; `signal` sets a guest event;
// `invite` broadcasts XN_LIVE_INVITE_ACCEPTED for user 0.
struct GuestHooks {
  std::function<void(uint32_t overlapped)> prepare;
  std::function<void(uint32_t overlapped, uint32_t result)> complete;
  std::function<void(uint32_t event)> signal;
  std::function<void()> invite;
  // True while the guest has an open listener covering kNotifyLiveArea; an
  // invite is announced and marked delivered only then.
  std::function<bool()> listening;
};

struct SearchRequest {
  uint32_t num_results = 0;
  uint32_t num_contexts = 0;
  uint32_t contexts_ptr = 0;
  uint32_t buffer_size = 0;
  uint32_t results_ptr = 0;
  uint32_t overlapped = 0;
};
// XSessionSearchByID after decoding: one session, guest result buffer.
struct SearchByIdRequest {
  uint64_t session_id = 0;
  uint32_t buffer_size = 0;
  uint32_t results_ptr = 0;
  uint32_t overlapped = 0;
};
// Progress of a running-game invite lookup.
enum class AcceptState { kNone, kPending, kFound, kMissing, kFailed };
// The hosted session's invite state for CurrentHostInvite.
struct HostInviteState {
  uint64_t session_id = 0;  // zero when not hosting a published session
  std::optional<relay::TokenId> token_id;
  relay::TokenSecret token_secret;
  uint32_t members = 0;
  uint32_t max_members = 0;
};
struct QosTarget {
  uint32_t ipv4 = 0;  // XNADDR ina, network order
  uint64_t session_id = 0;
};
struct Counters {
  uint64_t publishes = 0;
  uint64_t publish_failures = 0;
  uint64_t removes = 0;
  uint64_t association_losses = 0;
  uint64_t searches = 0;
  uint64_t search_timeouts = 0;
  uint64_t search_failures = 0;
  uint64_t results_written = 0;
  uint64_t results_dropped = 0;  // filtered, undecodable or beyond the buffer
  uint64_t qos_lookups = 0;
  uint64_t qos_reachable = 0;
  uint64_t qos_unreachable = 0;
  uint64_t dropped_contexts = 0;
  uint64_t dropped_properties = 0;
  uint64_t dropped_qos = 0;
  uint64_t invites_delivered = 0;
  uint64_t token_requests = 0;
};

// Thread-safe. Guest entry points take the current time; Pump advances
// directory requests, publication and probes. With `threaded`, a worker pumps
// while work is pending; otherwise the owner calls Pump.
class Service {
 public:
  Service(rex::memory::Memory& memory, Directory& directory, GuestHooks hooks, bool threaded);
  ~Service();
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;
  // Joins the worker; later calls still work but nothing pumps them.
  void Stop();

  void SetHostName(std::string name);
  void SetContext(uint32_t user, uint32_t id, uint32_t value, Clock::time_point now);
  void SetProperty(uint32_t user, uint32_t id, std::span<const uint8_t> value,
                   Clock::time_point now);
  void SetQosListen(std::span<const uint8_t> payload, Clock::time_point now);
  // Mirrors the local session after create, join, leave and modify. Only a
  // host matchmaking session without arbitration is published.
  void SessionChanged(const lan::SessionSnapshot& session, Clock::time_point now);
  // Membership keys: a remote XUID, or LocalMember(user index).
  void Joined(std::span<const uint64_t> members, const std::vector<bool>& private_slots);
  // Members removed by slot kind, and members this client never saw join.
  struct Departures {
    uint32_t public_members = 0;
    uint32_t private_members = 0;
    uint32_t unknown = 0;
  };
  Departures Left(std::span<const uint64_t> members);
  // Delete and end remove a published session.
  void EndSession(Clock::time_point now);
  // XNetCleanup: removes, then completes every pending search and lookup.
  void Cleanup();
  std::optional<relay::SessionRecord> FoundRecord(uint64_t session_id) const;
  // Returns kErrorIoPending when the overlapped completes later; otherwise the
  // result buffer has been written and the request is complete.
  uint32_t Search(const SearchRequest& request, Clock::time_point now);
  // Allocates the XNQOS block with SystemHeapAlloc, as XNetQosRelease frees
  // it, and stores its address at `result_ptr`. Returns a Win32 status.
  uint32_t QosLookup(std::span<const QosTarget> targets, uint32_t unsupported, unsigned probes,
                     uint32_t event, uint32_t result_ptr, Clock::time_point now);
  // XSessionSearchByID: one deferred LOOKUP written by the search writer.
  uint32_t SearchById(const SearchByIdRequest& request, Clock::time_point now);
  bool Cancel(uint32_t overlapped);
  // One pending invite per process, user 0. Announce after association, lookup,
  // and kNotifyLiveArea listener; clear on ID search or kInviteHoldMs after
  // announcement. ListenerCreated retries waiting announcements.
  void SetStartupInvite(uint64_t session_id);
  // Startup invites wait for association; a running-game accept looks up at
  // once and replaces the pending invite only when found.
  bool AcceptInvite(uint64_t session_id);
  // Withdraws a running-game accept: its lookup, and its invite unless the
  // guest was already notified. The startup invite is untouched.
  void CancelAccept();
  AcceptState PollAccept() const;
  void ListenerCreated(uint64_t mask, Clock::time_point now);
  std::optional<relay::SessionRecord> AcceptedInvite(uint32_t user_index) const;
  // Host side: the first call for a hosted session requests a join token;
  // the service replaces it when less than kInviteRefreshSeconds remain.
  HostInviteState HostInvite(Clock::time_point now);
  void ReleaseQos(uint32_t xnqos);
  void Pump(Clock::time_point now);
  bool Busy() const;
  Counters counters() const;
  std::optional<uint64_t> hosted_session() const;

  static uint64_t LocalMember(uint32_t user_index) { return 0xFFFFFFFF00000000ull | user_index; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The overlapped of the XMsgStartIORequest dispatched on this thread. A
// handler that calls Defer owns the overlapped's completion.
class DispatchScope {
 public:
  explicit DispatchScope(uint32_t overlapped);
  ~DispatchScope();
  DispatchScope(const DispatchScope&) = delete;
  DispatchScope& operator=(const DispatchScope&) = delete;
  bool deferred() const;

 private:
  uint32_t previous_overlapped_;
  bool previous_deferred_;
};
uint32_t DispatchOverlapped();
// The whole XOVERLAPPED lies in committed, writable guest memory. XMsg calls
// refuse any other overlapped before a handler or completion can write through it.
bool OverlappedWritable(rex::memory::Memory& memory, uint32_t overlapped);
void Defer();

// The process-wide service, or null when Live sessions are inactive or no
// kernel has armed it. The first active call creates it on the relay client and
// the running kernel's memory. The returned reference keeps it alive.
std::shared_ptr<Service> ActiveService();
// Called by each kernel at construction; ActiveService may then create a service.
void Arm();
// Called at kernel teardown: closes ActiveService until the next Arm, stops
// the worker and drops pending requests. Calls already admitted finish safely.
void Shutdown();

// Writes XINVITE_INFO for `record`; false when the range is not writable.
bool WriteInviteInfo(rex::memory::Memory& memory, uint32_t info_ptr, uint64_t invitee_xuid,
                     uint32_t title_id, const relay::SessionRecord& record);
// The public session summary for a local session snapshot.
rex::live_invite::SessionStatus SessionStatusFrom(const lan::SessionSnapshot& session);
// Encodes the host invite for `relay_address`; empty without a token or address.
rex::live_invite::HostInvite EncodeHostInvite(const HostInviteState& state,
                                              std::string_view relay_address);

// A running-game BeginJoin attempt. Resolution of a relay host that differs
// from the configured text runs on a background task; Poll advances it.
class InviteJoin {
 public:
  using Resolver = std::function<Result<relay::Endpoint>(std::string_view)>;
  struct Environment {
    system_link::Config config;
    bool live = false;
    relay::Endpoint server;  // resolved configured relay
    std::shared_ptr<Service> service;
    Resolver resolve;
  };
  rex::live_invite::JoinStatus Begin(std::string_view code, Environment environment);
  rex::live_invite::JoinStatus Poll();

 private:
  // Filled by a detached resolver thread. A replaced attempt drops its
  // reference, so a stalled resolver never blocks Begin and its result is ignored.
  struct Resolution {
    std::mutex mutex;
    std::optional<Result<relay::Endpoint>> result;
  };
  rex::live_invite::JoinStatus Finish(rex::live_invite::JoinResult result, std::string reason);
  rex::live_invite::JoinStatus Accept();
  rex::live_invite::JoinStatus status_;
  uint64_t session_id_ = 0;
  relay::Endpoint server_;
  std::shared_ptr<Service> service_;
  std::shared_ptr<Resolution> resolving_;
  bool accepting_ = false;
};

// Called after the guest creates an XNotify listener, so a waiting invite is announced.
void NotifyListenerCreated(uint64_t mask);

// XGI and XNet message bridges. Each runs only against an active service.
uint32_t HandleSearch(Service& service, rex::memory::Memory& memory, uint32_t request_ptr,
                      uint32_t request_length);
void HandleProperty(Service& service, rex::memory::Memory& memory, uint32_t user, uint32_t id,
                    uint32_t size, uint32_t value_ptr);
// After lan::CreateSession: a join copies the found record's flags and slots.
void AfterCreate(Service& service, lan::LanSystemLinkState& state, rex::memory::Memory& memory,
                 uint32_t session_info_ptr);
void AfterJoin(Service& service, lan::LanSystemLinkState& state, rex::memory::Memory& memory,
               uint32_t request_ptr);
void HandleLeave(Service& service, lan::LanSystemLinkState& state, rex::memory::Memory& memory,
                 uint32_t request_ptr);
void HandleModify(Service& service, lan::LanSystemLinkState& state, rex::memory::Memory& memory,
                  uint32_t request_ptr);
// XSessionSearchByID: a missing or short result buffer reports its size at +0x0C.
uint32_t HandleSearchById(Service& service, rex::memory::Memory& memory, uint32_t request_ptr,
                          uint64_t session_id, uint32_t buffer_size, uint32_t results_ptr);
// XInviteGetAcceptedInfo. The guest passes an XLiveBase argument list of two
// entries: a pointer to the user index and the XINVITE_INFO pointer. False
// leaves the guest buffer untouched.
constexpr uint32_t kInviteInfoArgumentCount = 2;
bool HandleInviteInfo(Service& service, rex::memory::Memory& memory, uint32_t arguments_ptr,
                      uint32_t title_id);
uint32_t HandleQosLookup(Service& service, rex::memory::Memory& memory, uint32_t count,
                         uint32_t addresses_ptr, uint32_t ids_ptr, uint32_t unsupported,
                         uint32_t probes, uint32_t event, uint32_t result_ptr);

namespace testing {
// Replaces the process-wide service; null restores lazy creation.
void SetService(Service* service);
// Installs `service` as the owned process-wide service, as ActiveService creates it.
void Adopt(std::unique_ptr<Directory> directory, std::unique_ptr<Service> service);
bool Owned();
}  // namespace testing

}  // namespace rex::kernel::xam::live
