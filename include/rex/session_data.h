/**
 * @file        rex/session_data.h
 * @brief       Title data the host of a System Link or Live session serves to joiners
 *
 * A title registers a provider whose bytes describe its host-side settings.
 * When this instance creates a host session, the SDK calls the provider once,
 * freezes the bytes for that session and serves them to every joiner over an
 * SDK side channel that the guest never sees. A joiner's create starts a
 * request to the host addressed from the host's XSESSION_INFO; the title reads
 * the result with Current() or waits for it with WaitForHostData(). Messages
 * are authenticated with the session's key-exchange key. See
 * docs/MAINTAINING.md, "Host session data", for transport and port details.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace rex::session_data {

inline constexpr std::size_t kMaxBytes = 1024;

// Called (any thread, must be cheap) when this instance creates a host session;
// the returned bytes (<= kMaxBytes, larger is truncated with a warning) are
// frozen for that session and served to joiners. A null provider freezes no
// bytes, and an exception from it is logged as an error and freezes no bytes
// (the host still reports kHost). The provider is copied and may be replaced
// at any time; a session already created keeps the bytes it froze.
void SetHostDataProvider(std::function<std::vector<std::uint8_t>()> provider);

enum class State : std::uint8_t {
  kNone,         // no session
  kHost,         // this instance hosts the session; bytes are the frozen data
  kPending,      // joined; the host's data has not arrived yet
  kReceived,     // joined; bytes are the host's data
  kUnavailable,  // joined; the host did not answer within about 15 seconds
};

struct Status {
  State state = State::kNone;
  std::vector<std::uint8_t> bytes;
};

// kNone without a session; kHost with the frozen bytes on the host. Thread-safe
// and never blocks.
Status Current();

// Returns at once unless kPending; then waits up to `timeout` for host data or
// for the request to time out. If the session ends during the wait (delete, a
// new create or XNetCleanup), returns kNone without bytes even if a newer
// session exists; call Current() for that session.
Status WaitForHostData(std::chrono::milliseconds timeout);

}  // namespace rex::session_data
