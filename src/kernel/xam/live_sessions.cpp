#include "live_sessions.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "../../core/live_invite.h"
#include "apps/xlivebase_request.h"
#include "system_link_config.h"

namespace rex::kernel::xam::live {
namespace {

constexpr uint8_t kPropertiesVersion = 1;
constexpr size_t kPropertiesHeaderSize = 7;
constexpr size_t kPropertyHeaderSize = 6;
constexpr uint32_t kMaxPropertyValue = 4096;
constexpr uint32_t kMaxMembers = 64;
constexpr uint32_t kErrorNotEnoughMemory = 8;

void PutBe16(relay::Bytes& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value));
}
void PutBe32(relay::Bytes& bytes, uint32_t value) {
  PutBe16(bytes, static_cast<uint16_t>(value >> 16));
  PutBe16(bytes, static_cast<uint16_t>(value));
}
uint16_t GetBe16(std::span<const uint8_t> bytes, size_t offset) {
  return static_cast<uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}
uint32_t GetBe32(std::span<const uint8_t> bytes, size_t offset) {
  return (uint32_t(GetBe16(bytes, offset)) << 16) | GetBe16(bytes, offset + 2);
}
uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// The whole range lies in one heap and every page is committed and readable,
// and writable when `write` is set.
bool GuestAccess(rex::memory::Memory& memory, uint32_t address, uint64_t size, bool write) {
  if (!address || !size || size > std::numeric_limits<uint32_t>::max() ||
      address > std::numeric_limits<uint32_t>::max() - (size - 1))
    return false;
  const uint32_t last = static_cast<uint32_t>(address + size - 1);
  auto* heap = memory.LookupHeap(address);
  if (!heap || heap != memory.LookupHeap(last) || !heap->page_size())
    return false;
  uint64_t cursor = address & ~uint64_t(heap->page_size() - 1);
  while (cursor <= last) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(static_cast<uint32_t>(cursor), &info) || !info.region_size ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead))
      return false;
    if (write && !(info.protect &
                   (rex::memory::kMemoryProtectWrite | rex::memory::kMemoryProtectWriteCombine)))
      return false;
    cursor += info.region_size;
  }
  return true;
}
bool GuestRange(rex::memory::Memory& memory, uint32_t address, uint64_t size) {
  return GuestAccess(memory, address, size, false);
}
bool GuestWritable(rex::memory::Memory& memory, uint32_t address, uint64_t size) {
  return GuestAccess(memory, address, size, true);
}
uint8_t* Guest(rex::memory::Memory& memory, uint64_t address) {
  return memory.TranslateVirtual<uint8_t*>(static_cast<uint32_t>(address));
}

// XUSER_DATA_TYPE from the property id's top nibble: 1 int32, 2 int64,
// 3 double, 4 unicode, 5 float, 6 binary, 7 datetime, 0xF null.
uint8_t PropertyType(uint32_t id) {
  return static_cast<uint8_t>(id >> 28);
}
bool VariableProperty(uint32_t id) {
  return PropertyType(id) == 4 || PropertyType(id) == 6;
}
// Whether the value can be written as its type's XUSER_DATA. A unicode value
// is cut after its first UTF-16 terminator; one without a terminator is unusable.
bool WritableProperty(Property& property) {
  switch (PropertyType(property.id)) {
    case 1:
    case 5:
      return property.value.size() == 4;
    case 2:
    case 3:
    case 7:
      return property.value.size() == 8;
    case 4:
      for (size_t i = 0; i + 1 < property.value.size(); i += 2) {
        if (!property.value[i] && !property.value[i + 1]) {
          property.value.resize(i + 2);
          return true;
        }
      }
      return false;
    case 6:
      return property.value.size() <= kMaxPropertyValue;
    case 0xF:
      return property.value.empty();
    default:
      return false;
  }
}

std::optional<uint32_t> FindContext(std::span<const Context> contexts, uint32_t id) {
  for (const auto& context : contexts)
    if (context.id == id)
      return context.value;
  return std::nullopt;
}

uint32_t Median(std::vector<uint32_t> values) {
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

}  // namespace

relay::Bytes EncodeContexts(std::span<const Context> contexts, uint64_t* dropped) {
  relay::Bytes bytes;
  for (const auto& context : contexts) {
    if (bytes.size() + kContextEntrySize > relay::kMaxRecordContexts) {
      if (dropped)
        ++*dropped;
      continue;
    }
    PutBe32(bytes, context.id);
    PutBe32(bytes, context.value);
  }
  return bytes;
}

std::optional<std::vector<Context>> DecodeContexts(std::span<const uint8_t> bytes) {
  if (bytes.size() % kContextEntrySize || bytes.size() > relay::kMaxRecordContexts)
    return std::nullopt;
  std::vector<Context> contexts;
  for (size_t offset = 0; offset < bytes.size(); offset += kContextEntrySize)
    contexts.push_back({GetBe32(bytes, offset), GetBe32(bytes, offset + 4)});
  return contexts;
}

relay::Bytes EncodeProperties(const Ethernet& ethernet, std::span<const Property> properties,
                              uint64_t* dropped) {
  relay::Bytes bytes{kPropertiesVersion};
  bytes.insert(bytes.end(), ethernet.begin(), ethernet.end());
  for (const auto& property : properties) {
    if (property.value.size() > 0xFFFF ||
        bytes.size() + kPropertyHeaderSize + property.value.size() > relay::kMaxRecordProperties) {
      if (dropped)
        ++*dropped;
      continue;
    }
    PutBe32(bytes, property.id);
    PutBe16(bytes, static_cast<uint16_t>(property.value.size()));
    bytes.insert(bytes.end(), property.value.begin(), property.value.end());
  }
  return bytes;
}

std::optional<DecodedProperties> DecodeProperties(std::span<const uint8_t> bytes) {
  if (bytes.size() < kPropertiesHeaderSize || bytes.size() > relay::kMaxRecordProperties ||
      bytes[0] != kPropertiesVersion)
    return std::nullopt;
  DecodedProperties decoded;
  std::copy_n(bytes.begin() + 1, decoded.ethernet.size(), decoded.ethernet.begin());
  size_t offset = kPropertiesHeaderSize;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < kPropertyHeaderSize)
      return std::nullopt;
    Property property;
    property.id = GetBe32(bytes, offset);
    const size_t length = GetBe16(bytes, offset + 4);
    offset += kPropertyHeaderSize;
    if (bytes.size() - offset < length)
      return std::nullopt;
    property.value.assign(bytes.begin() + offset, bytes.begin() + offset + length);
    offset += length;
    decoded.properties.push_back(std::move(property));
  }
  return decoded;
}

std::vector<uint8_t> HostNameValue(std::string_view name) {
  std::vector<uint8_t> value;
  for (char c : name) {
    value.push_back(0);
    value.push_back(static_cast<uint8_t>(c));
  }
  value.push_back(0);
  value.push_back(0);
  return value;
}

uint64_t SessionId(std::span<const uint8_t> info) {
  uint64_t id = 0;
  for (size_t i = 0; i < 8 && i < info.size(); ++i)
    id = (id << 8) | info[i];
  return id;
}

SearchFilter ReadSearchFilter(rex::memory::Memory& memory, uint32_t contexts_ptr, uint32_t count) {
  SearchFilter filter;
  if (!count || !GuestRange(memory, contexts_ptr, uint64_t(count) * kContextEntrySize))
    return filter;
  const auto* contexts = Guest(memory, contexts_ptr);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = memory::load_and_swap<uint32_t>(contexts + i * kContextEntrySize);
    const uint32_t value = memory::load_and_swap<uint32_t>(contexts + i * kContextEntrySize + 4);
    if (id == kContextGameType)
      filter.game_type = value;
    else if (id == kContextGameMode)
      filter.game_mode = value;
  }
  return filter;
}

bool Accepts(const SearchFilter& filter, const relay::SessionRecord& record) {
  if (!record.session_id || record.session_id == filter.own_session ||
      !(record.flags & kCreateMatchmaking) || (record.flags & kCreateArbitration) ||
      !relay_link::IsVirtualAddress(record.host_ipv4) || !DecodeProperties(record.properties))
    return false;
  const auto contexts = DecodeContexts(record.contexts);
  if (!contexts)
    return false;
  if (filter.game_type) {
    // Game type 0 is ranked, which this directory never serves.
    const auto value = FindContext(*contexts, kContextGameType);
    if (!*filter.game_type || !value || *value != *filter.game_type)
      return false;
  }
  if (filter.game_mode) {
    const auto value = FindContext(*contexts, kContextGameMode);
    if (!value || *value != *filter.game_mode)
      return false;
  }
  return true;
}

uint32_t SearchBufferSize(uint32_t num_results) {
  const uint64_t size = kSearchHeaderSize + uint64_t(kSearchBytesPerResult) * num_results;
  return static_cast<uint32_t>(std::min<uint64_t>(size, std::numeric_limits<uint32_t>::max()));
}

uint32_t WriteSearchResults(rex::memory::Memory& memory, uint32_t results_ptr, uint32_t buffer_size,
                            std::span<const relay::SessionRecord> records) {
  if (buffer_size < kSearchHeaderSize || !GuestWritable(memory, results_ptr, buffer_size))
    return 0;
  struct Entry {
    const relay::SessionRecord* record;
    DecodedProperties properties;
    std::vector<Context> contexts;
    uint64_t variable = 0;  // property and context arrays plus payloads
  };
  std::vector<Entry> entries;
  for (const auto& record : records) {
    if (entries.size() == kMaxSearchResults)
      break;
    auto properties = DecodeProperties(record.properties);
    auto contexts = DecodeContexts(record.contexts);
    if (!properties || !contexts)
      continue;
    std::erase_if(properties->properties,
                  [](Property& property) { return !WritableProperty(property); });
    Entry entry{&record, std::move(*properties), std::move(*contexts)};
    entry.variable = uint64_t(kPropertyEntrySize) * entry.properties.properties.size() +
                     uint64_t(kContextEntrySize) * entry.contexts.size();
    for (const auto& property : entry.properties.properties)
      if (VariableProperty(property.id))
        entry.variable += AlignUp(property.value.size(), 8);
    entries.push_back(std::move(entry));
  }

  // Keep the longest prefix whose fixed records and variable storage fit.
  const uint64_t base = results_ptr;
  const uint64_t end = base + buffer_size;
  auto fixed_end = [&](size_t count) {
    return AlignUp(base + kSearchHeaderSize + uint64_t(kSearchResultSize) * count, 8);
  };
  size_t count = 0;
  uint64_t variable = 0;
  while (count < entries.size() &&
         fixed_end(count + 1) + variable + entries[count].variable <= end) {
    variable += entries[count].variable;
    ++count;
  }
  const uint64_t used = count ? fixed_end(count) + variable - base : kSearchHeaderSize;
  std::memset(Guest(memory, base), 0, static_cast<size_t>(used));
  auto* header = Guest(memory, base);
  memory::store_and_swap<uint32_t>(header, static_cast<uint32_t>(count));
  memory::store_and_swap<uint32_t>(header + 4,
                                   count ? static_cast<uint32_t>(base + kSearchHeaderSize) : 0);

  uint64_t cursor = fixed_end(count);
  for (size_t i = 0; i < count; ++i) {
    const auto& entry = entries[i];
    const auto& record = *entry.record;
    auto* result = Guest(memory, base + kSearchHeaderSize + uint64_t(kSearchResultSize) * i);
    // XSESSION_INFO order: XNKID, XNADDR {ina, inaOnline, wPortOnline, abEnet,
    // abOnline}, XNKEY.
    memory::store_and_swap<uint64_t>(result, record.session_id);
    std::memcpy(result + 8, &record.host_ipv4, 4);
    std::memcpy(result + 18, entry.properties.ethernet.data(), entry.properties.ethernet.size());
    std::memcpy(result + 44, record.key.data(), record.key.size());
    memory::store_and_swap<uint32_t>(result + 60, record.open_public);
    memory::store_and_swap<uint32_t>(result + 64, record.open_private);
    memory::store_and_swap<uint32_t>(
        result + 68, record.max_public - std::min(record.max_public, record.open_public));
    memory::store_and_swap<uint32_t>(
        result + 72, record.max_private - std::min(record.max_private, record.open_private));
    const auto& properties = entry.properties.properties;
    memory::store_and_swap<uint32_t>(result + 76, static_cast<uint32_t>(properties.size()));
    memory::store_and_swap<uint32_t>(result + 80, static_cast<uint32_t>(entry.contexts.size()));
    const uint64_t properties_ptr = cursor;
    const uint64_t contexts_ptr = properties_ptr + uint64_t(kPropertyEntrySize) * properties.size();
    uint64_t payload = contexts_ptr + uint64_t(kContextEntrySize) * entry.contexts.size();
    if (!properties.empty())
      memory::store_and_swap<uint32_t>(result + 84, static_cast<uint32_t>(properties_ptr));
    if (!entry.contexts.empty())
      memory::store_and_swap<uint32_t>(result + 88, static_cast<uint32_t>(contexts_ptr));
    for (size_t p = 0; p < properties.size(); ++p) {
      const auto& property = properties[p];
      auto* slot = Guest(memory, properties_ptr + uint64_t(kPropertyEntrySize) * p);
      memory::store_and_swap<uint32_t>(slot, property.id);
      const uint8_t type = PropertyType(property.id);
      slot[8] = type == 0xF ? 0xFF : type;
      if (VariableProperty(property.id)) {
        memory::store_and_swap<uint32_t>(slot + 16, static_cast<uint32_t>(property.value.size()));
        if (!property.value.empty()) {
          memory::store_and_swap<uint32_t>(slot + 20, static_cast<uint32_t>(payload));
          std::memcpy(Guest(memory, payload), property.value.data(), property.value.size());
        }
        payload += AlignUp(property.value.size(), 8);
      } else if (!property.value.empty()) {
        std::memcpy(slot + 16, property.value.data(), property.value.size());
      }
    }
    for (size_t c = 0; c < entry.contexts.size(); ++c) {
      auto* slot = Guest(memory, contexts_ptr + uint64_t(kContextEntrySize) * c);
      memory::store_and_swap<uint32_t>(slot, entry.contexts[c].id);
      memory::store_and_swap<uint32_t>(slot + 4, entry.contexts[c].value);
    }
    cursor += entry.variable;
  }
  return static_cast<uint32_t>(count);
}

namespace {

class RelayClientDirectory final : public Directory {
 public:
  explicit RelayClientDirectory(relay_link::Client& client) : client_(client) {}
  bool Live() const override { return client_.stats().state == relay_link::Association::kLive; }
  bool Publish(const relay::SessionRecord& record) override {
    return client_.PublishSession(record);
  }
  bool Remove(uint64_t session_id) override { return client_.RemoveSession(session_id); }
  relay_link::RequestHandle BeginSearch(uint16_t max_results, uint32_t required_flags) override {
    return client_.BeginSearch(max_results, required_flags);
  }
  relay_link::RecordsPoll PollRecords(relay_link::RequestHandle handle) override {
    return client_.PollRecords(handle);
  }
  relay_link::RequestHandle BeginProbe(uint32_t ipv4, unsigned count) override {
    return client_.BeginProbe(ipv4, count);
  }
  relay_link::ProbePoll PollProbe(relay_link::RequestHandle handle) override {
    return client_.PollProbe(handle);
  }
  relay_link::RequestHandle BeginLookup(uint64_t session_id) override {
    return client_.BeginLookup(session_id);
  }
  relay_link::RequestHandle BeginTokenRequest(uint64_t session_id) override {
    return client_.BeginTokenRequest(session_id);
  }
  relay_link::TokenPoll PollToken(relay_link::RequestHandle handle) override {
    return client_.PollToken(handle);
  }

 private:
  relay_link::Client& client_;
};

}  // namespace

std::unique_ptr<Directory> ClientDirectory(relay_link::Client& client) {
  return std::make_unique<RelayClientDirectory>(client);
}

struct Service::Impl {
  rex::memory::Memory& memory;
  Directory& directory;
  GuestHooks hooks;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  bool stop = false;

  std::string host_name;
  std::array<std::vector<Context>, 4> contexts;
  std::array<std::vector<Property>, 4> properties;
  std::vector<uint8_t> qos_listen;
  struct Hosted {
    lan::SessionSnapshot session;
    uint64_t id = 0;
    bool dirty = true;
    bool published = false;
    Clock::time_point next{};
  };
  std::optional<Hosted> hosted;
  uint64_t retired = 0;  // ended session, never republished
  uint64_t current = 0;  // session whose membership is tracked
  std::vector<std::pair<uint64_t, bool>> members;
  std::map<uint64_t, relay::SessionRecord> found;
  // Removals the client could not queue; retried until accepted or the association ends.
  std::vector<uint64_t> removals;

  struct PendingSearch {
    SearchRequest request;
    SearchFilter filter;
    relay_link::RequestHandle handle = 0;
    Clock::time_point start{};
    Clock::time_point deadline{};
    uint64_t by_id = 0;  // XSessionSearchByID session; zero for a filtered search
  };
  std::vector<PendingSearch> searches;
  // Handles the service stopped waiting for; polled until the client consumes them.
  std::vector<relay_link::RequestHandle> abandoned_searches;
  std::vector<relay_link::RequestHandle> abandoned_probes;
  struct QosEntry {
    uint32_t index = 0;
    relay_link::RequestHandle handle = 0;
    uint32_t data_ptr = 0;
    std::vector<uint8_t> data;
  };
  struct PendingQos {
    uint32_t xnqos = 0;
    uint32_t event = 0;
    uint32_t pending = 0;
    std::vector<QosEntry> entries;
  };
  std::vector<PendingQos> lookups;
  Counters counters;

  // The one invite lookup in flight: the startup invite or a running-game accept.
  struct InviteLookup {
    uint64_t session_id = 0;
    relay_link::RequestHandle handle = 0;
    bool startup = false;
    unsigned attempts = 0;
    Clock::time_point next{};
  };
  std::optional<InviteLookup> invite_lookup;
  AcceptState accept = AcceptState::kNone;
  struct PendingInvite {
    relay::SessionRecord record;
    bool accepted = false;  // from a running-game accept rather than startup
    bool delivered = false;
    Clock::time_point since{};
  };
  std::optional<PendingInvite> invite;
  // Join token for the hosted session, present once the title asked for an invite.
  struct InviteToken {
    uint64_t session_id = 0;
    std::optional<relay::TokenId> id;
    relay::TokenSecret secret;
    Clock::time_point expires{};
    relay_link::RequestHandle handle = 0;
    Clock::time_point retry{};
  };
  std::optional<InviteToken> token;
  std::vector<relay_link::RequestHandle> abandoned_tokens;

  Impl(rex::memory::Memory& memory, Directory& directory, GuestHooks hooks)
      : memory(memory), directory(directory), hooks(std::move(hooks)) {}

  bool BusyLocked() const {
    return !searches.empty() || !abandoned_searches.empty() || !abandoned_probes.empty() ||
           !lookups.empty() || !removals.empty() || (hosted && hosted->dirty) ||
           invite_lookup.has_value() || !abandoned_tokens.empty() || (token && token->handle);
  }

  // The next timed invite step while nothing else keeps the worker busy.
  std::optional<Clock::time_point> DeadlineLocked() const {
    std::optional<Clock::time_point> deadline;
    auto consider = [&](Clock::time_point when) {
      if (!deadline || when < *deadline)
        deadline = when;
    };
    if (invite && invite->delivered)
      consider(invite->since + std::chrono::milliseconds(kInviteHoldMs));
    if (token && !token->handle) {
      if (token->id)
        consider(
            std::max(token->retry, token->expires - std::chrono::seconds(kInviteRefreshSeconds)));
      else
        consider(token->retry);
    }
    return deadline;
  }

  void ResetTokenLocked() {
    if (token && token->handle)
      abandoned_tokens.push_back(token->handle);
    token.reset();
  }

  void RequestTokenLocked(Clock::time_point now) {
    if (!token || token->handle || now < token->retry || !directory.Live())
      return;
    if (token->id && token->expires - now > std::chrono::seconds(kInviteRefreshSeconds))
      return;
    token->handle = directory.BeginTokenRequest(token->session_id);
    ++counters.token_requests;
    if (!token->handle)
      token->retry = now + std::chrono::milliseconds(kInviteTokenRetryMs);
    REXKRNL_DEBUG("Live invites: join token requested started={}", token->handle != 0);
    wake.notify_all();
  }

  void PumpTokenLocked(Clock::time_point now) {
    std::erase_if(abandoned_tokens, [&](relay_link::RequestHandle handle) {
      return directory.PollToken(handle).state != relay_link::RequestState::kPending;
    });
    if (!token)
      return;
    if (token->handle) {
      auto poll = directory.PollToken(token->handle);
      if (poll.state == relay_link::RequestState::kPending)
        return;
      token->handle = 0;
      const auto secret = poll.token ? poll.token->secret.view() : std::string_view{};
      if (poll.state == relay_link::RequestState::kDone && poll.token &&
          secret.size() == relay::TokenSecret::kSize && poll.token->expiry_seconds) {
        token->id = poll.token->id;
        token->secret = relay::TokenSecret(std::span<const uint8_t, relay::TokenSecret::kSize>(
            reinterpret_cast<const uint8_t*>(secret.data()), relay::TokenSecret::kSize));
        token->expires = now + std::chrono::seconds(poll.token->expiry_seconds);
        token->retry = {};
        REXKRNL_DEBUG("Live invites: join token granted lifetime={}s", poll.token->expiry_seconds);
      } else {
        token->retry = now + std::chrono::milliseconds(kInviteTokenRetryMs);
        REXKRNL_DEBUG("Live invites: join token request failed");
      }
    }
    RequestTokenLocked(now);
  }

  void FailInviteLookupLocked(Clock::time_point now) {
    auto& lookup = *invite_lookup;
    lookup.handle = 0;
    if (lookup.startup && ++lookup.attempts < kInviteLookupAttempts) {
      lookup.next = now + std::chrono::seconds(1);
      return;
    }
    REXKRNL_DEBUG("Live invites: lookup failed session={:016X}", lookup.session_id);
    if (!lookup.startup)
      accept = AcceptState::kFailed;
    invite_lookup.reset();
  }

  void DeliverInviteLocked(Clock::time_point now) {
    if (!invite || invite->delivered || !directory.Live() || !hooks.listening || !hooks.listening())
      return;
    invite->delivered = true;
    invite->since = now;
    ++counters.invites_delivered;
    REXKRNL_DEBUG("Live invites: XN_LIVE_INVITE_ACCEPTED raised session={:016X}",
                  invite->record.session_id);
    if (hooks.invite)
      hooks.invite();
  }

  void PumpInviteLocked(Clock::time_point now) {
    if (invite_lookup) {
      auto& lookup = *invite_lookup;
      if (!lookup.handle) {
        if (directory.Live() && now >= lookup.next) {
          lookup.handle = directory.BeginLookup(lookup.session_id);
          if (!lookup.handle)
            FailInviteLookupLocked(now);
          else
            REXKRNL_DEBUG("Live invites: looking up session={:016X}", lookup.session_id);
        }
      } else if (auto poll = directory.PollRecords(lookup.handle);
                 poll.state == relay_link::RequestState::kDone) {
        auto record = std::find_if(poll.records.begin(), poll.records.end(), [&](const auto& r) {
          return r.session_id == lookup.session_id;
        });
        if (record != poll.records.end()) {
          REXKRNL_DEBUG("Live invites: session={:016X} found", lookup.session_id);
          found[record->session_id] = *record;
          invite.emplace();
          invite->record = std::move(*record);
          invite->accepted = !lookup.startup;
          if (!lookup.startup)
            accept = AcceptState::kFound;
        } else {
          REXKRNL_DEBUG("Live invites: session={:016X} not in this room", lookup.session_id);
          if (!lookup.startup)
            accept = AcceptState::kMissing;
        }
        invite_lookup.reset();
      } else if (poll.state == relay_link::RequestState::kFailed) {
        FailInviteLookupLocked(now);
      }
    }
    DeliverInviteLocked(now);
    if (invite && invite->delivered &&
        now - invite->since >= std::chrono::milliseconds(kInviteHoldMs)) {
      REXKRNL_DEBUG("Live invites: pending invite expired unsearched");
      invite.reset();
    }
  }

  relay::SessionRecord BuildLocked() {
    const auto& session = hosted->session;
    relay::SessionRecord record;
    record.session_id = hosted->id;
    std::copy_n(session.info.begin() + 44, record.key.size(), record.key.begin());
    record.flags = session.flags;
    record.max_public = static_cast<uint16_t>(std::min<uint32_t>(session.max_public_slots, 0xFFFF));
    record.max_private =
        static_cast<uint16_t>(std::min<uint32_t>(session.max_private_slots, 0xFFFF));
    const uint32_t occupied_public = session.joined_public + session.filled_public;
    const uint32_t occupied_private = session.joined_private + session.filled_private;
    record.open_public = static_cast<uint16_t>(
        record.max_public - std::min<uint32_t>(record.max_public, occupied_public));
    record.open_private = static_cast<uint16_t>(
        record.max_private - std::min<uint32_t>(record.max_private, occupied_private));
    const uint32_t user = session.user_index;
    std::vector<Property> advertised{{kPropertyGamerHostName, HostNameValue(host_name)}};
    if (user < properties.size()) {
      for (const auto& property : properties[user])
        if (property.id != kPropertyGamerHostName)
          advertised.push_back(property);
      record.contexts = EncodeContexts(contexts[user], &counters.dropped_contexts);
    }
    Ethernet ethernet{};
    std::copy_n(session.info.begin() + 18, ethernet.size(), ethernet.begin());
    record.properties = EncodeProperties(ethernet, advertised, &counters.dropped_properties);
    if (qos_listen.size() <= relay::kMaxRecordQos)
      record.qos = qos_listen;
    else
      ++counters.dropped_qos;
    return record;
  }

  void LoseLocked() {
    ++counters.association_losses;
    hosted.reset();
    ResetTokenLocked();
    REXKRNL_DEBUG("Live sessions: association lost; the relay removed the hosted session");
  }

  void PublishDueLocked(Clock::time_point now) {
    if (!hosted || !hosted->dirty || now < hosted->next)
      return;
    if (!directory.Live()) {
      LoseLocked();
      return;
    }
    const auto record = BuildLocked();
    if (directory.Publish(record)) {
      hosted->published = true;
      hosted->dirty = false;
      ++counters.publishes;
      REXKRNL_DEBUG(
          "Live sessions: published open={}/{} private={}/{} properties={}B contexts={}B qos={}B",
          record.open_public, record.max_public, record.open_private, record.max_private,
          record.properties.size(), record.contexts.size(), record.qos.size());
    } else {
      ++counters.publish_failures;
    }
    hosted->next = now + std::chrono::milliseconds(kRepublishIntervalMs);
  }

  void RemoveHostedLocked() {
    if (hosted->published && directory.Live()) {
      if (directory.Remove(hosted->id))
        ++counters.removes;
      else
        removals.push_back(hosted->id);
    }
    hosted.reset();
    ResetTokenLocked();
    wake.notify_all();
  }

  void RetryRemovalsLocked() {
    if (removals.empty())
      return;
    // Without a live association the relay has already dropped this peer's sessions.
    if (!directory.Live()) {
      removals.clear();
      return;
    }
    std::erase_if(removals, [&](uint64_t id) {
      if (!directory.Remove(id))
        return false;
      ++counters.removes;
      return true;
    });
  }

  void MarkDirtyLocked(uint32_t user, Clock::time_point now) {
    if (hosted && hosted->session.user_index == user) {
      hosted->dirty = true;
      PublishDueLocked(now);
      wake.notify_all();
    }
  }

  // Returns the number of records written to the guest buffer.
  uint32_t CompleteSearchLocked(const PendingSearch& search,
                                std::span<const relay::SessionRecord> records,
                                Clock::time_point now) {
    const auto& request = search.request;
    const uint32_t written =
        WriteSearchResults(memory, request.results_ptr, request.buffer_size, records);
    counters.results_written += written;
    counters.results_dropped += records.size() - written;
    REXKRNL_DEBUG(
        "Live sessions: search listed={} written={} buffer={} elapsed={}ms", records.size(),
        written, request.buffer_size,
        std::chrono::duration_cast<std::chrono::milliseconds>(now - search.start).count());
    std::atomic_thread_fence(std::memory_order_release);
    if (hooks.complete)
      hooks.complete(request.overlapped, 0);
    return written;
  }

  void FinishQosLocked(PendingQos& lookup, const QosEntry& entry,
                       const relay_link::ProbePoll& poll) {
    auto* info = Guest(
        memory, uint64_t(lookup.xnqos) + kQosHeaderSize + uint64_t(kQosInfoSize) * entry.index);
    memory::store_and_swap<uint16_t>(info + 2, static_cast<uint16_t>(poll.sent));
    uint8_t flags = kQosComplete;
    if (poll.state == relay_link::RequestState::kDone && !poll.rtt_ms.empty()) {
      const uint32_t minimum = *std::min_element(poll.rtt_ms.begin(), poll.rtt_ms.end());
      memory::store_and_swap<uint16_t>(info + 4, static_cast<uint16_t>(poll.rtt_ms.size()));
      memory::store_and_swap<uint16_t>(info + 12,
                                       static_cast<uint16_t>(std::min<uint32_t>(minimum, 0xFFFF)));
      memory::store_and_swap<uint16_t>(
          info + 14, static_cast<uint16_t>(std::min<uint32_t>(Median(poll.rtt_ms), 0xFFFF)));
      if (entry.data_ptr) {
        std::memcpy(Guest(memory, entry.data_ptr), entry.data.data(), entry.data.size());
        memory::store_and_swap<uint16_t>(info + 6, static_cast<uint16_t>(entry.data.size()));
        memory::store_and_swap<uint32_t>(info + 8, entry.data_ptr);
        flags |= kQosDataReceived;
      }
      flags |= kQosTargetContacted;
      ++counters.qos_reachable;
    } else {
      ++counters.qos_unreachable;
    }
    std::atomic_thread_fence(std::memory_order_release);
    info[0] = flags;
    --lookup.pending;
    std::atomic_thread_fence(std::memory_order_release);
    memory::store_and_swap<uint32_t>(Guest(memory, uint64_t(lookup.xnqos) + 4), lookup.pending);
  }

  void PumpLocked(Clock::time_point now) {
    for (auto it = searches.begin(); it != searches.end();) {
      auto poll = directory.PollRecords(it->handle);
      if (poll.state == relay_link::RequestState::kPending && now < it->deadline) {
        ++it;
        continue;
      }
      std::vector<relay::SessionRecord> accepted;
      if (poll.state == relay_link::RequestState::kPending) {
        abandoned_searches.push_back(it->handle);
        ++counters.search_timeouts;
      } else if (poll.state == relay_link::RequestState::kFailed) {
        ++counters.search_failures;
      } else if (it->by_id) {
        // LOOKUP returns the session whatever its slots and flags; the guest joins it directly.
        for (auto& record : poll.records) {
          if (record.session_id == it->by_id && accepted.empty())
            accepted.push_back(std::move(record));
          else
            ++counters.results_dropped;
        }
        for (const auto& record : accepted)
          found[record.session_id] = record;
      } else {
        for (auto& record : poll.records) {
          if (Accepts(it->filter, record))
            accepted.push_back(std::move(record));
          else
            ++counters.results_dropped;
        }
        if (accepted.size() > it->request.num_results) {
          counters.results_dropped += accepted.size() - it->request.num_results;
          accepted.resize(it->request.num_results);
        }
        found.clear();
        for (const auto& record : accepted)
          found.emplace(record.session_id, record);
      }
      const uint32_t written = CompleteSearchLocked(*it, accepted, now);
      // A buffer too small for the record leaves the invite for a retry.
      if (it->by_id && written && invite && invite->record.session_id == it->by_id) {
        REXKRNL_DEBUG("Live invites: search by ID returned session={:016X}; invite cleared",
                      it->by_id);
        invite.reset();
      }
      it = searches.erase(it);
    }
    std::erase_if(abandoned_searches, [&](relay_link::RequestHandle handle) {
      return directory.PollRecords(handle).state != relay_link::RequestState::kPending;
    });
    if (hosted && hosted->published && !directory.Live())
      LoseLocked();
    RetryRemovalsLocked();
    PublishDueLocked(now);
    for (auto lookup = lookups.begin(); lookup != lookups.end();) {
      for (auto entry = lookup->entries.begin(); entry != lookup->entries.end();) {
        const auto poll = directory.PollProbe(entry->handle);
        if (poll.state == relay_link::RequestState::kPending) {
          ++entry;
          continue;
        }
        FinishQosLocked(*lookup, *entry, poll);
        entry = lookup->entries.erase(entry);
      }
      if (lookup->pending) {
        ++lookup;
        continue;
      }
      if (lookup->event && hooks.signal)
        hooks.signal(lookup->event);
      lookup = lookups.erase(lookup);
    }
    std::erase_if(abandoned_probes, [&](relay_link::RequestHandle handle) {
      return directory.PollProbe(handle).state != relay_link::RequestState::kPending;
    });
    PumpInviteLocked(now);
    PumpTokenLocked(now);
  }

  void Run() {
    std::unique_lock lock(mutex);
    while (!stop) {
      PumpLocked(Clock::now());
      if (stop)
        break;
      if (BusyLocked())
        wake.wait_for(lock, std::chrono::milliseconds(10));
      else if (const auto deadline = DeadlineLocked())
        wake.wait_until(lock, *deadline);
      else
        wake.wait(lock);
    }
  }
};

Service::Service(rex::memory::Memory& memory, Directory& directory, GuestHooks hooks, bool threaded)
    : impl_(std::make_unique<Impl>(memory, directory, std::move(hooks))) {
  if (threaded)
    impl_->worker = std::thread([this] { impl_->Run(); });
}

Service::~Service() {
  Stop();
}

void Service::Stop() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stop = true;
    impl_->wake.notify_all();
  }
  if (impl_->worker.joinable())
    impl_->worker.join();
}

void Service::SetHostName(std::string name) {
  std::lock_guard lock(impl_->mutex);
  impl_->host_name = std::move(name);
}

void Service::SetContext(uint32_t user, uint32_t id, uint32_t value, Clock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  if (user >= impl_->contexts.size())
    return;
  auto& contexts = impl_->contexts[user];
  auto found = std::find_if(contexts.begin(), contexts.end(),
                            [&](const Context& context) { return context.id == id; });
  if (found != contexts.end()) {
    if (found->value == value)
      return;
    found->value = value;
  } else {
    contexts.push_back({id, value});
  }
  impl_->MarkDirtyLocked(user, now);
}

void Service::SetProperty(uint32_t user, uint32_t id, std::span<const uint8_t> value,
                          Clock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  if (user >= impl_->properties.size())
    return;
  auto& properties = impl_->properties[user];
  auto found = std::find_if(properties.begin(), properties.end(),
                            [&](const Property& property) { return property.id == id; });
  std::vector<uint8_t> bytes(value.begin(), value.end());
  if (found != properties.end()) {
    if (found->value == bytes)
      return;
    found->value = std::move(bytes);
  } else {
    properties.push_back({id, std::move(bytes)});
  }
  impl_->MarkDirtyLocked(user, now);
}

void Service::SetQosListen(std::span<const uint8_t> payload, Clock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  if (std::equal(payload.begin(), payload.end(), impl_->qos_listen.begin(),
                 impl_->qos_listen.end()))
    return;
  impl_->qos_listen.assign(payload.begin(), payload.end());
  if (impl_->hosted)
    impl_->MarkDirtyLocked(impl_->hosted->session.user_index, now);
}

void Service::SessionChanged(const lan::SessionSnapshot& session, Clock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  const uint64_t id = session.created ? SessionId(session.info) : 0;
  if (id != impl_->current) {
    impl_->current = id;
    impl_->members.clear();
  }
  const bool host = session.created && (session.flags & kCreateHost) &&
                    (session.flags & kCreateMatchmaking) && !(session.flags & kCreateArbitration) &&
                    id && id != impl_->retired;
  if (impl_->hosted && (!host || impl_->hosted->id != id))
    impl_->RemoveHostedLocked();
  if (!host)
    return;
  if (!impl_->hosted)
    impl_->hosted = Impl::Hosted{session, id, true, false, now};
  impl_->hosted->session = session;
  impl_->hosted->dirty = true;
  impl_->PublishDueLocked(now);
  impl_->wake.notify_all();
}

void Service::Joined(std::span<const uint64_t> members, const std::vector<bool>& private_slots) {
  std::lock_guard lock(impl_->mutex);
  for (size_t i = 0; i < members.size(); ++i) {
    const bool is_private = i < private_slots.size() && private_slots[i];
    auto found = std::find_if(impl_->members.begin(), impl_->members.end(),
                              [&](const auto& member) { return member.first == members[i]; });
    if (found != impl_->members.end())
      found->second = is_private;
    else if (impl_->members.size() < kMaxMembers)
      impl_->members.emplace_back(members[i], is_private);
  }
}

Service::Departures Service::Left(std::span<const uint64_t> members) {
  std::lock_guard lock(impl_->mutex);
  Departures removed;
  for (const auto member : members) {
    auto found = std::find_if(impl_->members.begin(), impl_->members.end(),
                              [&](const auto& entry) { return entry.first == member; });
    if (found == impl_->members.end()) {
      ++removed.unknown;
      continue;
    }
    ++(found->second ? removed.private_members : removed.public_members);
    impl_->members.erase(found);
  }
  return removed;
}

void Service::EndSession(Clock::time_point) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->hosted)
    return;
  impl_->retired = impl_->hosted->id;
  impl_->RemoveHostedLocked();
  REXKRNL_DEBUG("Live sessions: hosted session removed");
}

void Service::Cleanup() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->hosted)
    impl_->RemoveHostedLocked();
  const auto now = Clock::now();
  for (const auto& search : impl_->searches) {
    impl_->abandoned_searches.push_back(search.handle);
    impl_->CompleteSearchLocked(search, {}, now);
  }
  impl_->searches.clear();
  for (auto& lookup : impl_->lookups) {
    for (const auto& entry : lookup.entries) {
      impl_->abandoned_probes.push_back(entry.handle);
      impl_->FinishQosLocked(lookup, entry, {});
    }
    if (lookup.event && impl_->hooks.signal)
      impl_->hooks.signal(lookup.event);
  }
  impl_->lookups.clear();
  impl_->found.clear();
  impl_->members.clear();
  impl_->current = 0;
  impl_->wake.notify_all();
}

std::optional<relay::SessionRecord> Service::FoundRecord(uint64_t session_id) const {
  std::lock_guard lock(impl_->mutex);
  auto found = impl_->found.find(session_id);
  if (found == impl_->found.end())
    return std::nullopt;
  return found->second;
}

uint32_t Service::Search(const SearchRequest& request, Clock::time_point now) {
  auto filter = ReadSearchFilter(impl_->memory, request.contexts_ptr, request.num_contexts);
  std::lock_guard lock(impl_->mutex);
  ++impl_->counters.searches;
  filter.own_session = impl_->hosted ? impl_->hosted->id : 0;
  // Without an overlapped the guest would wait on this thread, so it gets no results.
  if (!request.overlapped || !request.num_results || (filter.game_type && !*filter.game_type)) {
    WriteSearchResults(impl_->memory, request.results_ptr, request.buffer_size, {});
    return 0;
  }
  const auto handle = impl_->directory.BeginSearch(
      static_cast<uint16_t>(std::min(request.num_results, kMaxSearchResults)), kCreateMatchmaking);
  if (!handle) {
    ++impl_->counters.search_failures;
    WriteSearchResults(impl_->memory, request.results_ptr, request.buffer_size, {});
    return 0;
  }
  if (impl_->hooks.prepare)
    impl_->hooks.prepare(request.overlapped);
  impl_->searches.push_back(
      {request, filter, handle, now, now + std::chrono::milliseconds(kSearchTimeoutMs)});
  impl_->wake.notify_all();
  return kErrorIoPending;
}

uint32_t Service::QosLookup(std::span<const QosTarget> targets, uint32_t unsupported,
                            unsigned probes, uint32_t event, uint32_t result_ptr,
                            Clock::time_point) {
  std::lock_guard lock(impl_->mutex);
  const uint64_t count = uint64_t(targets.size()) + unsupported;
  if (count > kMaxQosTargets || !GuestWritable(impl_->memory, result_ptr, 4))
    return kErrorInvalidParameter;
  // One allocation holds the header, every XNQOSINFO and the listen data,
  // so XNetQosRelease's single SystemHeapFree releases all of it.
  std::vector<Impl::QosEntry> entries(targets.size());
  uint64_t size = kQosHeaderSize + uint64_t(kQosInfoSize) * std::max<uint64_t>(count, 1);
  for (size_t i = 0; i < targets.size(); ++i) {
    entries[i].index = static_cast<uint32_t>(i);
    auto found = impl_->found.find(targets[i].session_id);
    if (found != impl_->found.end() && found->second.host_ipv4 == targets[i].ipv4)
      entries[i].data = found->second.qos;
    size += AlignUp(entries[i].data.size(), 8);
  }
  const uint32_t xnqos = impl_->memory.SystemHeapAlloc(static_cast<uint32_t>(size));
  if (!xnqos)
    return kErrorNotEnoughMemory;
  auto* header = Guest(impl_->memory, xnqos);
  memory::store_and_swap<uint32_t>(header, static_cast<uint32_t>(count));
  memory::store_and_swap<uint32_t>(header + 4, static_cast<uint32_t>(count));
  Impl::PendingQos lookup{xnqos, event, static_cast<uint32_t>(count), {}};
  uint64_t data =
      uint64_t(xnqos) + kQosHeaderSize + uint64_t(kQosInfoSize) * std::max<uint64_t>(count, 1);
  for (size_t i = 0; i < targets.size(); ++i) {
    auto& entry = entries[i];
    if (!entry.data.empty()) {
      entry.data_ptr = static_cast<uint32_t>(data);
      data += AlignUp(entry.data.size(), 8);
    }
    if (relay_link::IsVirtualAddress(targets[i].ipv4))
      entry.handle = impl_->directory.BeginProbe(targets[i].ipv4, probes);
    if (entry.handle)
      lookup.entries.push_back(std::move(entry));
    else
      impl_->FinishQosLocked(lookup, entry, {});
  }
  for (uint32_t i = 0; i < unsupported; ++i) {
    Impl::QosEntry entry;
    entry.index = static_cast<uint32_t>(targets.size() + i);
    impl_->FinishQosLocked(lookup, entry, {});
  }
  memory::store_and_swap<uint32_t>(Guest(impl_->memory, result_ptr), xnqos);
  ++impl_->counters.qos_lookups;
  REXKRNL_DEBUG("Live sessions: QoS lookup targets={} probing={} bytes={}", count,
                lookup.entries.size(), size);
  if (!lookup.pending) {
    if (event && impl_->hooks.signal)
      impl_->hooks.signal(event);
  } else {
    impl_->lookups.push_back(std::move(lookup));
    impl_->wake.notify_all();
  }
  return 0;
}

bool Service::Cancel(uint32_t overlapped) {
  std::lock_guard lock(impl_->mutex);
  auto found =
      std::find_if(impl_->searches.begin(), impl_->searches.end(),
                   [&](const auto& search) { return search.request.overlapped == overlapped; });
  if (!overlapped || found == impl_->searches.end())
    return false;
  impl_->abandoned_searches.push_back(found->handle);
  impl_->searches.erase(found);
  if (impl_->hooks.complete)
    impl_->hooks.complete(overlapped, kErrorCancelled);
  impl_->wake.notify_all();
  return true;
}

void Service::ReleaseQos(uint32_t xnqos) {
  std::lock_guard lock(impl_->mutex);
  auto found = std::find_if(impl_->lookups.begin(), impl_->lookups.end(),
                            [&](const auto& lookup) { return lookup.xnqos == xnqos; });
  if (found == impl_->lookups.end())
    return;
  for (const auto& entry : found->entries)
    impl_->abandoned_probes.push_back(entry.handle);
  impl_->lookups.erase(found);
  impl_->wake.notify_all();
}

void Service::Pump(Clock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  impl_->PumpLocked(now);
}

bool Service::Busy() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->BusyLocked();
}

Counters Service::counters() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->counters;
}

uint32_t Service::SearchById(const SearchByIdRequest& request, Clock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->counters.searches;
  REXKRNL_DEBUG("Live invites: search by ID session={:016X} overlapped={:08X}", request.session_id,
                request.overlapped);
  // Without an overlapped only the pending invite's record can answer at once.
  if (!request.overlapped || !request.session_id) {
    std::vector<relay::SessionRecord> records;
    if (request.session_id && impl_->invite &&
        impl_->invite->record.session_id == request.session_id)
      records.push_back(impl_->invite->record);
    const uint32_t written =
        WriteSearchResults(impl_->memory, request.results_ptr, request.buffer_size, records);
    if (written) {
      impl_->found[request.session_id] = records.front();
      impl_->invite.reset();
    }
    return 0;
  }
  const auto handle = impl_->directory.BeginLookup(request.session_id);
  if (!handle) {
    ++impl_->counters.search_failures;
    WriteSearchResults(impl_->memory, request.results_ptr, request.buffer_size, {});
    return 0;
  }
  if (impl_->hooks.prepare)
    impl_->hooks.prepare(request.overlapped);
  SearchRequest search;
  search.num_results = 1;
  search.buffer_size = request.buffer_size;
  search.results_ptr = request.results_ptr;
  search.overlapped = request.overlapped;
  impl_->searches.push_back({search,
                             {},
                             handle,
                             now,
                             now + std::chrono::milliseconds(kSearchTimeoutMs),
                             request.session_id});
  impl_->wake.notify_all();
  return kErrorIoPending;
}

void Service::SetStartupInvite(uint64_t session_id) {
  std::lock_guard lock(impl_->mutex);
  if (!session_id || impl_->invite_lookup)
    return;
  REXKRNL_DEBUG("Live invites: startup invite session={:016X} waits for the relay", session_id);
  impl_->invite_lookup.emplace();
  impl_->invite_lookup->session_id = session_id;
  impl_->invite_lookup->startup = true;
  impl_->wake.notify_all();
}

bool Service::AcceptInvite(uint64_t session_id) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->invite_lookup && impl_->invite_lookup->handle)
    impl_->abandoned_searches.push_back(impl_->invite_lookup->handle);
  impl_->invite_lookup.reset();
  impl_->accept = AcceptState::kFailed;
  if (!session_id || !impl_->directory.Live())
    return false;
  const auto handle = impl_->directory.BeginLookup(session_id);
  if (!handle)
    return false;
  REXKRNL_DEBUG("Live invites: accepting session={:016X}", session_id);
  impl_->invite_lookup.emplace();
  impl_->invite_lookup->session_id = session_id;
  impl_->invite_lookup->handle = handle;
  impl_->accept = AcceptState::kPending;
  impl_->wake.notify_all();
  return true;
}

void Service::CancelAccept() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->invite_lookup && !impl_->invite_lookup->startup) {
    if (impl_->invite_lookup->handle)
      impl_->abandoned_searches.push_back(impl_->invite_lookup->handle);
    impl_->invite_lookup.reset();
    REXKRNL_DEBUG("Live invites: running-game accept withdrawn");
  }
  if (impl_->invite && impl_->invite->accepted && !impl_->invite->delivered)
    impl_->invite.reset();
  impl_->accept = AcceptState::kNone;
}

AcceptState Service::PollAccept() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->accept;
}

void Service::ListenerCreated(uint64_t mask, Clock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  if (!(mask & kNotifyLiveArea))
    return;
  impl_->DeliverInviteLocked(now);
  impl_->wake.notify_all();
}

std::optional<relay::SessionRecord> Service::AcceptedInvite(uint32_t user_index) const {
  std::lock_guard lock(impl_->mutex);
  if (user_index || !impl_->invite)
    return std::nullopt;
  return impl_->invite->record;
}

HostInviteState Service::HostInvite(Clock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  HostInviteState state;
  if (!impl_->hosted || !impl_->hosted->published || !impl_->directory.Live()) {
    impl_->ResetTokenLocked();
    return state;
  }
  const auto record = impl_->BuildLocked();
  state.session_id = record.session_id;
  state.max_members = uint32_t(record.max_public) + record.max_private;
  state.members = state.max_members - record.open_public - record.open_private;
  if (!impl_->token || impl_->token->session_id != record.session_id) {
    impl_->ResetTokenLocked();
    impl_->token.emplace();
    impl_->token->session_id = record.session_id;
  }
  impl_->RequestTokenLocked(now);
  if (impl_->token->id && now < impl_->token->expires) {
    state.token_id = impl_->token->id;
    state.token_secret = impl_->token->secret;
  }
  return state;
}

std::optional<uint64_t> Service::hosted_session() const {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->hosted || !impl_->hosted->published)
    return std::nullopt;
  return impl_->hosted->id;
}

namespace {
thread_local uint32_t dispatch_overlapped = 0;
thread_local bool dispatch_deferred = false;
std::mutex service_mutex;
// Opened by each kernel at construction and closed by its teardown.
bool armed = false;
std::shared_ptr<Service> test_service;
// The process-wide service and the directory it uses. Callers share ownership,
// so the instance outlives every call admitted before Shutdown.
struct Instance {
  std::unique_ptr<Directory> directory;
  std::unique_ptr<Service> service;
  ~Instance() { service.reset(); }
};
std::shared_ptr<Instance> owned;
// The live_invite session is handed to the first service only.
bool startup_invite_taken = false;

GuestHooks KernelHooks() {
  GuestHooks hooks;
  hooks.prepare = [](uint32_t overlapped) {
    auto* state = system::kernel_state();
    auto* ptr = state->memory()->TranslateVirtual(overlapped);
    system::XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
    system::XOverlappedSetContext(ptr, system::XThread::GetCurrentThreadHandle());
  };
  hooks.complete = [](uint32_t overlapped, uint32_t result) {
    if (auto* state = system::kernel_state())
      state->CompleteOverlappedEx(overlapped, result, result, 0);
  };
  hooks.signal = [](uint32_t event) {
    auto* state = system::kernel_state();
    if (!state)
      return;
    if (auto object = state->object_table()->LookupObject<system::XEvent>(event))
      object->Set(0, false);
  };
  hooks.invite = [] {
    if (auto* state = system::kernel_state())
      state->BroadcastNotification(kNotifyLiveInviteAccepted, 0);
  };
  hooks.listening = [] {
    auto* state = system::kernel_state();
    return state && state->HasOpenNotifyListener(kNotifyLiveArea);
  };
  return hooks;
}

std::vector<uint64_t> ReadMembers(rex::memory::Memory& memory, uint32_t count, uint32_t xuids_ptr,
                                  uint32_t user_indices_ptr) {
  std::vector<uint64_t> members;
  if (!count || count > kMaxMembers)
    return members;
  if (xuids_ptr && GuestRange(memory, xuids_ptr, uint64_t(count) * 8)) {
    const auto* xuids = Guest(memory, xuids_ptr);
    for (uint32_t i = 0; i < count; ++i)
      members.push_back(memory::load_and_swap<uint64_t>(xuids + i * 8));
  } else if (!xuids_ptr && GuestRange(memory, user_indices_ptr, uint64_t(count) * 4)) {
    const auto* indices = Guest(memory, user_indices_ptr);
    for (uint32_t i = 0; i < count; ++i)
      members.push_back(Service::LocalMember(memory::load_and_swap<uint32_t>(indices + i * 4)));
  }
  return members;
}

// XSessionJoin and XSessionLeave requests: session, count, XUID array (remote)
// or null, user index array (local), private-slot array (join only).
struct MembershipRequest {
  std::vector<uint64_t> members;
  std::vector<bool> private_slots;
  bool remote = false;  // members are XUIDs rather than local user indices
};
MembershipRequest ReadMembership(rex::memory::Memory& memory, uint32_t request_ptr) {
  MembershipRequest request;
  if (!GuestRange(memory, request_ptr, 20))
    return request;
  const auto* bytes = Guest(memory, request_ptr);
  const uint32_t count = memory::load_and_swap<uint32_t>(bytes + 4);
  const uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(bytes + 8);
  const uint32_t indices_ptr = memory::load_and_swap<uint32_t>(bytes + 12);
  const uint32_t private_ptr = memory::load_and_swap<uint32_t>(bytes + 16);
  request.members = ReadMembers(memory, count, xuids_ptr, indices_ptr);
  request.remote = xuids_ptr != 0;
  if (!request.members.empty() && private_ptr &&
      GuestRange(memory, private_ptr, uint64_t(count) * 4)) {
    const auto* slots = Guest(memory, private_ptr);
    for (uint32_t i = 0; i < count; ++i)
      request.private_slots.push_back(memory::load_and_swap<uint32_t>(slots + i * 4) != 0);
  }
  return request;
}
}  // namespace

DispatchScope::DispatchScope(uint32_t overlapped)
    : previous_overlapped_(dispatch_overlapped), previous_deferred_(dispatch_deferred) {
  dispatch_overlapped = overlapped;
  dispatch_deferred = false;
}
DispatchScope::~DispatchScope() {
  dispatch_overlapped = previous_overlapped_;
  dispatch_deferred = previous_deferred_;
}
bool DispatchScope::deferred() const {
  return dispatch_deferred;
}
bool OverlappedWritable(rex::memory::Memory& memory, uint32_t overlapped) {
  return GuestWritable(memory, overlapped, kOverlappedSize);
}
uint32_t DispatchOverlapped() {
  return dispatch_overlapped;
}
void Defer() {
  dispatch_deferred = dispatch_overlapped != 0;
}

std::shared_ptr<Service> ActiveService() {
  if (!system_link::LiveSessionsActive())
    return nullptr;
  std::lock_guard lock(service_mutex);
  if (!armed)
    return nullptr;
  if (test_service)
    return test_service;
  if (!owned) {
    auto* state = system::kernel_state();
    if (!state)
      return nullptr;
    auto instance = std::make_shared<Instance>();
    instance->directory = ClientDirectory(relay_link::SystemLinkClient());
    instance->service =
        std::make_unique<Service>(*state->memory(), *instance->directory, KernelHooks(), true);
    const auto config = system_link::CurrentConfig();
    instance->service->SetHostName(config.profile_name);
    if (config.startup_invite && !startup_invite_taken) {
      startup_invite_taken = true;
      instance->service->SetStartupInvite(config.startup_invite);
    }
    owned = std::move(instance);
  }
  return std::shared_ptr<Service>(owned, owned->service.get());
}

void Arm() {
  std::lock_guard lock(service_mutex);
  armed = true;
}

void NotifyListenerCreated(uint64_t mask) {
  if (auto service = ActiveService())
    service->ListenerCreated(mask, Clock::now());
}

void Shutdown() {
  std::shared_ptr<Instance> stopping;
  {
    std::lock_guard lock(service_mutex);
    armed = false;
    stopping = std::move(owned);
  }
  // The worker stops now, dropping pending requests without guest writes. A call
  // admitted earlier keeps the instance alive and destroys it when it returns.
  if (stopping)
    stopping->service->Stop();
}

uint32_t HandleSearch(Service& service, rex::memory::Memory& memory, uint32_t request_ptr,
                      uint32_t request_length) {
  if ((request_length && request_length < 32) || !GuestWritable(memory, request_ptr, 32))
    return kErrorInvalidParameter;
  auto* bytes = Guest(memory, request_ptr);
  SearchRequest request;
  request.num_results = memory::load_and_swap<uint32_t>(bytes + 8);
  request.num_contexts = memory::load_and_swap<uint16_t>(bytes + 14);
  request.contexts_ptr = memory::load_and_swap<uint32_t>(bytes + 20);
  request.buffer_size = memory::load_and_swap<uint32_t>(bytes + 24);
  request.results_ptr = memory::load_and_swap<uint32_t>(bytes + 28);
  request.overlapped = DispatchOverlapped();
  if (request.overlapped && !GuestWritable(memory, request.overlapped, kOverlappedSize))
    return kErrorInvalidParameter;
  if (!request.results_ptr || request.buffer_size < kSearchHeaderSize) {
    memory::store_and_swap<uint32_t>(bytes + 24, SearchBufferSize(request.num_results));
    return kErrorInsufficientBuffer;
  }
  const uint32_t status = service.Search(request, Clock::now());
  if (status == kErrorIoPending)
    Defer();
  return status;
}

uint32_t HandleSearchById(Service& service, rex::memory::Memory& memory, uint32_t request_ptr,
                          uint64_t session_id, uint32_t buffer_size, uint32_t results_ptr) {
  const uint32_t overlapped = DispatchOverlapped();
  if (overlapped && !GuestWritable(memory, overlapped, kOverlappedSize))
    return kErrorInvalidParameter;
  if (!results_ptr || buffer_size < kSearchHeaderSize) {
    if (!GuestWritable(memory, request_ptr + 12, 4))
      return kErrorInvalidParameter;
    memory::store_and_swap<uint32_t>(Guest(memory, uint64_t(request_ptr) + 12),
                                     SearchBufferSize(1));
    return kErrorInsufficientBuffer;
  }
  SearchByIdRequest request{session_id, buffer_size, results_ptr, overlapped};
  const uint32_t status = service.SearchById(request, Clock::now());
  if (status == kErrorIoPending)
    Defer();
  return status;
}

bool WriteInviteInfo(rex::memory::Memory& memory, uint32_t info_ptr, uint64_t invitee_xuid,
                     uint32_t title_id, const relay::SessionRecord& record) {
  if (!GuestWritable(memory, info_ptr, kInviteInfoSize))
    return false;
  Ethernet ethernet{};
  if (auto properties = DecodeProperties(record.properties))
    ethernet = properties->ethernet;
  auto* info = Guest(memory, info_ptr);
  std::memset(info, 0, kInviteInfoSize);
  memory::store_and_swap<uint64_t>(info, invitee_xuid);
  // The inviter XUID at +0x08 and fFromGameInvite at +0x50 stay zero.
  memory::store_and_swap<uint32_t>(info + 0x10, title_id);
  // XSESSION_INFO exactly as the search writer lays it out.
  auto* session = info + kInviteSessionInfoOffset;
  memory::store_and_swap<uint64_t>(session, record.session_id);
  std::memcpy(session + 8, &record.host_ipv4, 4);
  std::memcpy(session + 18, ethernet.data(), ethernet.size());
  std::memcpy(session + 44, record.key.data(), record.key.size());
  return true;
}

bool HandleInviteInfo(Service& service, rex::memory::Memory& memory, uint32_t arguments_ptr,
                      uint32_t title_id) {
  if (!GuestRange(memory, arguments_ptr, apps::kArgumentListSize))
    return false;
  const std::span<const uint8_t> arguments(Guest(memory, arguments_ptr), apps::kArgumentListSize);
  if (apps::LoadBe32(arguments, apps::kArgumentCountOffset) != kInviteInfoArgumentCount)
    return false;
  const uint32_t user_ptr = apps::ArgumentPointer(arguments, 0);
  const uint32_t info_ptr = apps::ArgumentPointer(arguments, 1);
  if (!GuestRange(memory, user_ptr, 4))
    return false;
  const uint32_t user_index = memory::load_and_swap<uint32_t>(Guest(memory, user_ptr));
  const auto record = service.AcceptedInvite(user_index);
  if (!record)
    return false;
  const bool written =
      WriteInviteInfo(memory, info_ptr, system_link::CurrentConfig().xuid, title_id, *record);
  REXKRNL_DEBUG("Live invites: XInviteGetAcceptedInfo user={} session={:016X} written={}",
                user_index, record->session_id, written);
  return written;
}

rex::live_invite::SessionStatus SessionStatusFrom(const lan::SessionSnapshot& session) {
  rex::live_invite::SessionStatus status;
  if (!session.created)
    return status;
  status.active = true;
  status.host = (session.flags & kCreateHost) != 0;
  status.max_members = session.max_public_slots + session.max_private_slots;
  const uint64_t members = uint64_t(session.joined_public) + session.joined_private +
                           session.filled_public + session.filled_private;
  status.members = uint32_t(std::clamp<uint64_t>(members, 1, std::max(status.max_members, 1u)));
  return status;
}

rex::live_invite::HostInvite EncodeHostInvite(const HostInviteState& state,
                                              std::string_view relay_address) {
  rex::live_invite::HostInvite result;
  if (!state.session_id)
    return result;
  result.members = state.members;
  result.max_members = state.max_members;
  auto address = relay_link::ParseAddress(relay_address);
  if (!state.token_id || !address)
    return result;
  relay::LiveInvite invite;
  invite.session_id = state.session_id;
  invite.token_id = *state.token_id;
  invite.token_secret = state.token_secret;
  invite.address = std::move(*address);
  if (auto code = relay::EncodeLiveInvite(invite))
    result.code = std::move(*code);
  return result;
}

namespace {
bool SameHost(std::string_view left, std::string_view right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) ==
                  std::tolower(static_cast<unsigned char>(b));
         });
}
std::string_view TrimCode(std::string_view code) {
  const auto first = code.find_first_not_of(" \t\r\n\f\v");
  if (first == std::string_view::npos)
    return {};
  return code.substr(first, code.find_last_not_of(" \t\r\n\f\v") - first + 1);
}
}  // namespace

rex::live_invite::JoinStatus InviteJoin::Finish(rex::live_invite::JoinResult result,
                                                std::string reason) {
  status_ = {result, std::move(reason)};
  service_.reset();
  accepting_ = false;
  REXKRNL_DEBUG("Live invites: join finished result={} reason={}", static_cast<int>(result),
                status_.reason);
  return status_;
}

rex::live_invite::JoinStatus InviteJoin::Accept() {
  if (!service_->AcceptInvite(session_id_))
    return Finish(rex::live_invite::JoinResult::kUnavailable,
                  "The relay could not look up the session");
  accepting_ = true;
  status_ = {rex::live_invite::JoinResult::kPending, {}};
  return status_;
}

rex::live_invite::JoinStatus InviteJoin::Begin(std::string_view code, Environment environment) {
  using rex::live_invite::JoinResult;
  // Only the latest attempt may set the pending invite: withdraw the previous
  // accept, and drop a previous resolution without waiting for it.
  resolving_.reset();
  if (service_)
    service_->CancelAccept();
  if (environment.service && environment.service != service_)
    environment.service->CancelAccept();
  service_.reset();
  accepting_ = false;
  auto invite = relay::DecodeLiveInvite(TrimCode(code));
  if (!invite)
    return Finish(JoinResult::kInvalid, invite.error().message);
  const auto& config = environment.config;
  if (!config.error.empty() || config.mode != system_link::Mode::kRelay)
    return Finish(JoinResult::kRestartRequired, config.mode == system_link::Mode::kLan
                                                    ? "LAN System Link is on"
                                                    : "System Link is not using a relay");
  if (!config.live_sessions)
    return Finish(JoinResult::kRestartRequired, "Live sessions are off");
  if (!environment.live || !environment.service)
    return Finish(JoinResult::kRestartRequired, "Not connected to the relay");
  session_id_ = invite->session_id;
  server_ = environment.server;
  service_ = std::move(environment.service);
  const auto configured = relay_link::ParseAddress(config.address);
  if (configured && configured->port == invite->address.port &&
      SameHost(configured->host, invite->address.host))
    return Accept();
  if (!environment.resolve)
    return Finish(JoinResult::kUnavailable, "Cannot resolve the invite's relay");
  const std::string address = invite->address.host + ":" + std::to_string(invite->address.port);
  resolving_ = std::make_shared<Resolution>();
  std::thread([resolution = resolving_, resolve = std::move(environment.resolve), address] {
    auto result = resolve(address);
    std::lock_guard lock(resolution->mutex);
    resolution->result = std::move(result);
  }).detach();
  status_ = {JoinResult::kPending, {}};
  return status_;
}

rex::live_invite::JoinStatus InviteJoin::Poll() {
  using rex::live_invite::JoinResult;
  if (status_.result != JoinResult::kPending)
    return status_;
  if (resolving_) {
    std::optional<Result<relay::Endpoint>> endpoint;
    {
      std::lock_guard lock(resolving_->mutex);
      endpoint = std::move(resolving_->result);
    }
    if (!endpoint)
      return status_;
    resolving_.reset();
    if (!*endpoint)
      return Finish(JoinResult::kUnavailable, "Cannot resolve the invite's relay");
    if (**endpoint != server_)
      return Finish(JoinResult::kRestartRequired, "The invite is for another relay");
    return Accept();
  }
  if (!accepting_ || !service_)
    return Finish(JoinResult::kUnavailable, "No invite is being joined");
  switch (service_->PollAccept()) {
    case AcceptState::kFound:
      return Finish(JoinResult::kAccepted, {});
    case AcceptState::kMissing:
      return Finish(JoinResult::kRestartRequired, "The session is not in this relay room");
    case AcceptState::kPending:
      return status_;
    default:
      return Finish(JoinResult::kUnavailable, "The relay could not look up the session");
  }
}

void HandleProperty(Service& service, rex::memory::Memory& memory, uint32_t user, uint32_t id,
                    uint32_t size, uint32_t value_ptr) {
  if (size > kMaxPropertyValue || (size && !GuestRange(memory, value_ptr, size)))
    return;
  const auto* value = size ? Guest(memory, value_ptr) : nullptr;
  service.SetProperty(user, id, std::span<const uint8_t>(value, size), Clock::now());
}

void AfterCreate(Service& service, lan::LanSystemLinkState& state, rex::memory::Memory&, uint32_t) {
  auto session = state.session_snapshot();
  if (session.created && !(session.flags & kCreateHost)) {
    // A joiner reports the host's flags and slots, so details show true open slots.
    if (auto record = service.FoundRecord(SessionId(session.info))) {
      session.flags = record->flags & ~kCreateHost;
      session.max_public_slots = record->max_public;
      session.max_private_slots = record->max_private;
      session.filled_public =
          record->max_public - std::min(record->max_public, record->open_public);
      session.filled_private =
          record->max_private - std::min(record->max_private, record->open_private);
      state.CreateSession(session);
    }
  }
  service.SessionChanged(state.session_snapshot(), Clock::now());
}

void AfterJoin(Service& service, lan::LanSystemLinkState& state, rex::memory::Memory& memory,
               uint32_t request_ptr) {
  const auto request = ReadMembership(memory, request_ptr);
  service.Joined(request.members, request.private_slots);
  // Remote joins name host-counted members by slot kind. Without a roster, an
  // early new member also consumes a count until all counted members are named;
  // titles name the roster after create.
  if (request.remote) {
    const auto private_members = static_cast<uint32_t>(
        std::count(request.private_slots.begin(), request.private_slots.end(), true));
    state.ConsumeFilled(static_cast<uint32_t>(request.members.size()) - private_members,
                        private_members);
  }
  service.SessionChanged(state.session_snapshot(), Clock::now());
}

void HandleLeave(Service& service, lan::LanSystemLinkState& state, rex::memory::Memory& memory,
                 uint32_t request_ptr) {
  const auto request = ReadMembership(memory, request_ptr);
  const auto removed = service.Left(request.members);
  // Unknown members were counted by the host's record before this client joined;
  // their slot kind is unknown, so public counts go first.
  const auto uncounted = state.ConsumeFilled(0, 0, removed.unknown);
  REXKRNL_DEBUG("XGISessionLeave: public={} private={} unknown={}", removed.public_members,
                removed.private_members, removed.unknown);
  state.LeaveSession(removed.public_members + uncounted.first,
                     removed.private_members + uncounted.second);
  service.SessionChanged(state.session_snapshot(), Clock::now());
}

void HandleModify(Service& service, lan::LanSystemLinkState& state, rex::memory::Memory& memory,
                  uint32_t request_ptr) {
  if (!GuestRange(memory, request_ptr, 16))
    return;
  const auto* bytes = Guest(memory, request_ptr);
  const uint32_t flags = memory::load_and_swap<uint32_t>(bytes + 4);
  const uint32_t max_public = memory::load_and_swap<uint32_t>(bytes + 8);
  const uint32_t max_private = memory::load_and_swap<uint32_t>(bytes + 12);
  const auto session = state.session_snapshot();
  if (!session.created)
    return;
  state.ModifySession((session.flags & ~kModifiableFlags) | (flags & kModifiableFlags), max_public,
                      max_private);
  service.SessionChanged(state.session_snapshot(), Clock::now());
}

uint32_t HandleQosLookup(Service& service, rex::memory::Memory& memory, uint32_t count,
                         uint32_t addresses_ptr, uint32_t ids_ptr, uint32_t unsupported,
                         uint32_t probes, uint32_t event, uint32_t result_ptr) {
  if (uint64_t(count) + unsupported > kMaxQosTargets ||
      (count && !GuestRange(memory, addresses_ptr, uint64_t(count) * 4)) ||
      (count && ids_ptr && !GuestRange(memory, ids_ptr, uint64_t(count) * 4)))
    return kErrorInvalidParameter;
  std::vector<QosTarget> targets(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t address = memory::load_and_swap<uint32_t>(Guest(memory, addresses_ptr + i * 4));
    if (GuestRange(memory, address, 36))
      std::memcpy(&targets[i].ipv4, Guest(memory, address), 4);
    if (!ids_ptr)
      continue;
    const uint32_t id = memory::load_and_swap<uint32_t>(Guest(memory, ids_ptr + i * 4));
    if (GuestRange(memory, id, 8))
      targets[i].session_id = memory::load_and_swap<uint64_t>(Guest(memory, id));
  }
  return service.QosLookup(targets, unsupported, probes, event, result_ptr, Clock::now());
}

namespace testing {
void SetService(Service* service) {
  std::lock_guard lock(service_mutex);
  test_service = service ? std::shared_ptr<Service>(service, [](Service*) {}) : nullptr;
}
void Adopt(std::unique_ptr<Directory> directory, std::unique_ptr<Service> service) {
  auto instance = std::make_shared<Instance>();
  instance->directory = std::move(directory);
  instance->service = std::move(service);
  std::shared_ptr<Instance> previous;
  {
    std::lock_guard lock(service_mutex);
    previous = std::exchange(owned, std::move(instance));
  }
}
bool Owned() {
  std::lock_guard lock(service_mutex);
  return owned != nullptr;
}
}  // namespace testing

}  // namespace rex::kernel::xam::live

namespace rex::live_invite {
namespace {
std::mutex join_mutex;
kernel::xam::live::InviteJoin& JoinAttempt() {
  static kernel::xam::live::InviteJoin attempt;
  return attempt;
}
}  // namespace

HostInvite CurrentHostInvite() {
  namespace live = kernel::xam::live;
  auto service = live::ActiveService();
  if (!service)
    return {};
  return live::EncodeHostInvite(
      service->HostInvite(live::Clock::now()),
      kernel::xam::system_link::InviteRelayAddress(kernel::xam::system_link::CurrentConfig()));
}

JoinStatus BeginJoin(std::string_view code) {
  namespace xam = kernel::xam;
  auto& client = xam::relay_link::SystemLinkClient();
  xam::live::InviteJoin::Environment environment{
      xam::system_link::CurrentConfig(),
      client.stats().state == xam::relay_link::Association::kLive, client.server(),
      xam::live::ActiveService(), xam::relay_link::Resolve};
  std::lock_guard lock(join_mutex);
  return JoinAttempt().Begin(code, std::move(environment));
}

JoinStatus PollJoin() {
  std::lock_guard lock(join_mutex);
  return JoinAttempt().Poll();
}

SessionStatus CurrentSession() {
  namespace xam = kernel::xam;
  if (xam::system_link::CurrentMode() == xam::system_link::Mode::kOff)
    return {};
  return xam::live::SessionStatusFrom(xam::lan::SystemLinkState().session_snapshot());
}

}  // namespace rex::live_invite
