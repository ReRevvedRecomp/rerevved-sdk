#pragma once

#include <array>
#include <string>
#include <string_view>

#include "core/relay_transport.h"

namespace rexglue::cli {
namespace detail {

struct RelayCounter {
  std::string_view name;
  uint64_t value;
  bool problem = false;
};

inline auto RelayCounters(const rex::relay::Counters& c, const rex::relay::TransportCounters& t,
                          uint64_t connection_resets) {
  constexpr auto kDropNames = std::to_array<std::string_view>(
      {"version", "malformed", "auth", "room_full", "too_many_rooms", "fingerprint", "rate_limited",
       "destination", "direction", "expired_challenge", "idle", "directory_rate_limited",
       "session_conflict", "session_limit", "token_denied", "token_limit"});
  constexpr auto kConnectDropNames = std::to_array<std::string_view>(
      {"connect_rate_limited", "connect_unavailable", "connect_limit", "connect_auth"});
  static_assert(kDropNames.size() == static_cast<size_t>(rex::relay::Drop::kCount),
                "Update relay status for new drop reasons");
  static_assert(kConnectDropNames.size() == static_cast<size_t>(rex::relay::ConnectDrop::kCount),
                "Update relay status for new connect drop reasons");
  std::array<RelayCounter, 13 + kDropNames.size() + kConnectDropNames.size()> counters{{
      {"rooms", c.rooms},
      {"peers", c.peers},
      {"pending", c.pending},
      {"sessions", c.sessions},
      {"tokens", c.tokens},
      {"attempts", c.attempts},
      {"packets_in", c.packets_in},
      {"bytes_in", c.bytes_in},
      {"packets_out", c.packets_out},
      {"bytes_out", c.bytes_out},
      {"receive_errors", t.receive_errors, true},
      {"send_errors", t.send_errors, true},
      {"connection_resets", connection_resets, true},
  }};
  size_t index = 13;
  for (size_t i = 0; i < kDropNames.size(); ++i)
    counters[index++] = {kDropNames[i], c.drops[i],
                         i != static_cast<size_t>(rex::relay::Drop::kIdle)};
  for (size_t i = 0; i < kConnectDropNames.size(); ++i)
    counters[index++] = {kConnectDropNames[i], c.connect_drops[i], true};
  return counters;
}

inline void AppendRelayCounter(std::string& text, std::string_view name, uint64_t value,
                               bool delta = false) {
  if (!value)
    return;
  if (!text.empty())
    text += ' ';
  text += name;
  text += delta ? "=+" : "=";
  text += std::to_string(value);
}

}  // namespace detail

struct RelayStatusMessages {
  std::string status;
  std::string warning;
  std::string debug;
};

// Pure reporting state; now is monotonic milliseconds, as in the relay core.
class RelayStatus {
 public:
  RelayStatusMessages Update(const rex::relay::Counters& c, const rex::relay::TransportCounters& t,
                             uint64_t connection_resets, uint64_t now) {
    RelayStatusMessages messages;
    if (c.peers != peers_ || c.rooms != rooms_) {
      messages.status = "Relay: " + std::to_string(c.peers) +
                        (c.peers == 1 ? " player in " : " players in ") + std::to_string(c.rooms) +
                        (c.rooms == 1 ? " room" : " rooms");
      peers_ = c.peers;
      rooms_ = c.rooms;
    }
    if (now < next_warning_ && now < next_debug_)
      return messages;
    const auto counters = detail::RelayCounters(c, t, connection_resets);
    if (now >= next_warning_) {
      for (size_t i = 0; i < counters.size(); ++i) {
        const auto& counter = counters[i];
        if (counter.problem && counter.value > warned_[i])
          detail::AppendRelayCounter(messages.warning, counter.name, counter.value - warned_[i],
                                     true);
      }
      if (!messages.warning.empty()) {
        messages.warning.insert(0, "Relay problems: ");
        // Keep the last emitted baseline so suppressed increases are included next time.
        for (size_t i = 0; i < counters.size(); ++i)
          warned_[i] = counters[i].value;
        next_warning_ = now + kLogIntervalMs;
      }
    }
    if (now >= next_debug_) {
      for (const auto& counter : counters)
        detail::AppendRelayCounter(messages.debug, counter.name, counter.value);
      if (!messages.debug.empty())
        messages.debug.insert(0, "Relay counters: ");
      next_debug_ = now + kLogIntervalMs;
    }
    return messages;
  }

  static std::string Summary(const rex::relay::Counters& c, const rex::relay::TransportCounters& t,
                             uint64_t connection_resets) {
    uint64_t rejected = 0;
    for (size_t i = 0; i < c.drops.size(); ++i) {
      if (i != static_cast<size_t>(rex::relay::Drop::kIdle))
        rejected += c.drops[i];
    }
    for (const auto count : c.connect_drops)
      rejected += count;
    return "Relay stopped: packets_in=" + std::to_string(c.packets_in) +
           " bytes_in=" + std::to_string(c.bytes_in) +
           " packets_out=" + std::to_string(c.packets_out) +
           " bytes_out=" + std::to_string(c.bytes_out) + " rejected=" + std::to_string(rejected) +
           " receive_errors=" + std::to_string(t.receive_errors) +
           " send_errors=" + std::to_string(t.send_errors) +
           " connection_resets=" + std::to_string(connection_resets);
  }

 private:
  static constexpr uint64_t kLogIntervalMs = 5000;
  size_t peers_ = 0;
  size_t rooms_ = 0;
  uint64_t next_warning_ = 0;
  uint64_t next_debug_ = 0;
  std::array<uint64_t, 13 + static_cast<size_t>(rex::relay::Drop::kCount) +
                           static_cast<size_t>(rex::relay::ConnectDrop::kCount)>
      warned_{};
};

}  // namespace rexglue::cli
