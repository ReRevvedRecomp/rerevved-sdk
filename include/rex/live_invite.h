/**
 * @file        rex/live_invite.h
 * @brief       Invite codes for Live-mode sessions served by the relay directory
 *
 * Never blocks; safe from any host thread.
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
  std::string code;
  uint32_t members = 0;
  uint32_t max_members = 0;
};

// Empty until the relay has issued a join token, so poll. Tokens are
// requested only once this is called and renewed before they expire.
HostInvite CurrentHostInvite();

enum class JoinResult {
  kPending,
  kAccepted,
  kRestartRequired,  // relaunch with live_invite set to the same code
  kInvalid,
  kUnavailable,
};

struct JoinStatus {
  JoinResult result = JoinResult::kUnavailable;
  std::string reason;  // text for the player
};

// A new call replaces an attempt still pending.
JoinStatus BeginJoin(std::string_view code);
JoinStatus PollJoin();

struct SessionStatus {
  bool active = false;
  bool host = false;
  uint32_t members = 0;
  uint32_t max_members = 0;
};

// Covers every System Link mode, hosted or joined.
SessionStatus CurrentSession();

}  // namespace rex::live_invite
