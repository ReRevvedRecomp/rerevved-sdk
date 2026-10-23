/**
 * @file        rex/discord_rpc.h
 * @brief       Lightweight Discord Rich Presence client
 *
 * Communicates with the local Discord client over IPC using JSON payloads,
 * without third-party dependencies.
 *
 * To enable Rich Presence, create an Application at
 * https://discord.com/developers/applications and pass its Application ID to
 * Start(). Upload artwork there and reference asset names via SetLargeImage /
 * SetSmallImage.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32 || REX_PLATFORM_LINUX || REX_PLATFORM_MAC

namespace rex::discord_rpc {

struct Presence {
  std::string details_;           // first line under the title, e.g. "In Main Menu"
  std::string state_;             // second line, e.g. "Level 196"
  std::string large_image_key_;   // asset name from the Developer Portal
  std::string large_image_text_;  // tooltip shown on hover
  std::string small_image_key_;
  std::string small_image_text_;
};

// Spawns a background worker thread (idempotent). Sets the initial presence.
void Start(const char* client_id, const Presence& initial = {});

// Replaces the entire presence
void SetPresence(const Presence& p);

// Per-field setters for incremental updates (avoids constructing a full Presence).
void SetDetails(const std::string& details);
void SetState(const std::string& state);
void SetLargeImage(const std::string& key, const std::string& text = "");
void SetSmallImage(const std::string& key, const std::string& text = "");

// Party and join secret, kept apart from Presence and merged into every
// activity update as party.id, party.size [size, max] and secrets.join. An
// empty id or secret removes its keys; max 0 omits the size. Discord shows a
// join button only when the party id, size and join secret are all set.
void SetParty(const std::string& id, int size, int max);
void ClearParty();
void SetJoinSecret(const std::string& secret);
void ClearJoinSecret();

// Receives the join secret of an ACTIVITY_JOIN event on the worker thread.
// While a handler is set, the worker subscribes to ACTIVITY_JOIN and
// ACTIVITY_JOIN_REQUEST after READY; requests are only logged, because the
// player accepts them in Discord. An empty handler stops delivery.
// Join events do not depend on discord_activity: with it false no activity,
// party or secret is published, so friends see no join button, but join
// events from invites the player accepts in Discord still arrive.
using JoinHandler = std::function<void(const std::string& secret)>;
void SetJoinHandler(JoinHandler handler);

// Cold-start helper: waits up to `timeout` for an ACTIVITY_JOIN and returns its
// secret, or empty. Before Start it connects with `client_id` on the calling
// thread and closes that connection cleanly before returning, so a later
// Start reconnects; do not call Start concurrently with it. After Start it
// reuses the worker's connection. A waiting caller takes the event instead of
// the join handler. Only one caller may wait at a time; others return empty.
std::string WaitForJoin(const char* client_id, std::chrono::milliseconds timeout);

// Stops the worker thread and clears the presence.
void Stop();

}  // namespace rex::discord_rpc

#endif  // REX_PLATFORM_WIN32 || REX_PLATFORM_LINUX || REX_PLATFORM_MAC
