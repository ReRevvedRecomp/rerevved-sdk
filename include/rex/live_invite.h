/**
 * @file        rex/live_invite.h
 * @brief       Invite codes for Live-mode sessions served by the relay directory
 *
 * A Live invite code lets a player join a friend's Live-mode session from
 * outside the guest, for example from a chat client's join button or the
 * `live_invite` startup option. Codes are ASCII, at most 128 characters, and
 * carry a relay-issued join token; see docs/SYSTEM_LINK_RELAY.md, "Live
 * invites". These calls never block and are safe from any host thread.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rex::live_invite {

struct HostInvite {
  std::string code;          // empty when no invite is available
  uint32_t members = 0;      // occupied public and private slots
  uint32_t max_members = 0;  // public plus private slots
};

// Returns the invite for the Live session this instance hosts and has
// published. The first call for a session requests a join token from the
// relay and returns an empty code until it arrives; once asked, the token is
// replaced in the background when less than 300 seconds of its lifetime
// remain, so at most two tokens per session are live. Poll again to pick up a
// replacement. No token is requested before the first call or after the
// session ends. The code is empty when this instance is not hosting a
// published session, the relay association is not live, or the relay address
// cannot be encoded; the party size is reported whenever a session is hosted.
HostInvite CurrentHostInvite();

enum class JoinResult {
  kPending,          // still checking; call PollJoin
  kAccepted,         // the guest has been told an invite was accepted
  kRestartRequired,  // relaunch with live_invite set to the same code
  kInvalid,          // not a valid Live invite code
  kUnavailable,      // the relay could not answer; try again later
};

struct JoinStatus {
  JoinResult result = JoinResult::kUnavailable;
  std::string reason;  // plain text for the player, empty when accepted or pending
};

// Starts joining the session named by `code` in the running game. Accepted
// requires relay mode with Live sessions on, a live relay association, the
// same resolved relay endpoint as the code, and the session in this
// instance's room. Any other System Link state needs a restart. A new call
// replaces an attempt still pending.
JoinStatus BeginJoin(std::string_view code);
// Reports the latest BeginJoin attempt; kUnavailable when there is none.
JoinStatus PollJoin();

struct SessionStatus {
  bool active = false;       // the guest has a System Link or Live session
  bool host = false;         // this instance created it as host
  uint32_t members = 0;      // occupied public and private slots, at least 1
  uint32_t max_members = 0;  // public plus private slots
};

// The guest's current System Link or Live session, in any System Link mode,
// whether hosted or joined. Inactive when System Link is off or no session
// exists. It does not say what kind of game the session is for.
SessionStatus CurrentSession();

}  // namespace rex::live_invite
