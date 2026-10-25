/**
 * @file        rex/session_data.h
 * @brief       Title data the host of a System Link or Live session serves to joiners
 *
 * A title registers a provider whose bytes describe its host-side settings.
 * When this instance creates a host session, the SDK captures lobby bytes,
 * then refreshes them once at PrepareForGame and serves that frozen game over an
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

// Called at host session creation and game preparation on any thread. Must be cheap.
// Returned bytes are frozen until the next game and served to joiners. Output above
// kMaxBytes is truncated with a warning. Null or throwing providers freeze no bytes
// but still report kHost. Exceptions are logged as errors. The provider is copied and
// replaceable at any time. A prepared game keeps its frozen bytes until XSessionEnd.
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
  bool game_ready = false;  // refreshed for this game, never merely lobby data
};

// Before title gameplay freezes, capture the host's provider or start a joiner's fresh
// authenticated game-only request. Idempotent until XSessionEnd, also called by XSessionStart.
// Game policy requires game_ready after WaitForHostData, never Current's lobby bytes.
void PrepareForGame();

// kNone without a session; kHost with the frozen bytes on the host. Thread-safe
// and never blocks.
Status Current();

// Returns at once unless kPending; then waits up to `timeout` for host data or
// for the request to time out. If the session ends during the wait (delete, a
// new create or XNetCleanup), returns kNone without bytes even if a newer
// session exists; call Current() for that session.
Status WaitForHostData(std::chrono::milliseconds timeout);

}  // namespace rex::session_data
