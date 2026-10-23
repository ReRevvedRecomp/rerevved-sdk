#pragma once

// Discord IPC framing, JSON payload builders and the minimal event reader,
// separated from the connection code so they can be tested without a Discord client.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <rex/discord_rpc.h>

namespace rex::discord_rpc::detail {

struct PartyState {
  std::string id;
  int size = 0;
  int max = 0;
  std::string join_secret;
};

// Overwrites the string's whole capacity, then empties it.
void WipeString(std::string& text);

void AppendJsonEscaped(std::string& out, std::string_view in);
std::string JsonEscape(std::string_view in);
// SET_ACTIVITY with the presence, then party and secrets when set; instance is false.
// The payload is built in one reserved buffer, so wiping it wipes every copy of the secret.
std::string ActivityPayload(const Presence& presence, const PartyState& party,
                            int64_t start_timestamp, uint32_t pid, std::string_view nonce);
std::string ClearActivityPayload(uint32_t pid, std::string_view nonce);
std::string SubscribePayload(std::string_view event, std::string_view nonce);

// The string value of the first `"key":` at or after `from`, with JSON escapes
// decoded; nullopt when absent, not a string, or malformed.
std::optional<std::string> JsonString(std::string_view json, std::string_view key, size_t from = 0);
// The integer value of the first `"key":` at or after `from`.
std::optional<int64_t> JsonInteger(std::string_view json, std::string_view key, size_t from = 0);
// The code and message of an ERROR event or CLOSE frame, without the rest of
// the frame, which may echo the join secret. Live invite codes in the message
// (`rr1.`, any case, and the base64url-or-dot run after it) become `<invite>`.
std::string DescribeError(std::string_view frame);
// Replaces each Live invite code in `text` with `<invite>`.
void RedactInviteCodes(std::string& text);

// One received frame: `evt`, and for join events data.secret or data.user.username.
struct Event {
  std::string evt;
  std::string secret;
  std::string user;
};
Event ParseEvent(std::string_view frame);

// IPC frames are an 8-byte header {opcode, body length}, both little-endian
// u32, then the JSON body. Longer bodies are refused before any allocation.
inline constexpr uint32_t kMaxFrameBytes = 64 * 1024;
// One drain pass handles at most this many frames, so a flooding peer cannot
// hold the worker or a join wait past its deadline.
inline constexpr int kMaxFramesPerDrain = 16;
// Close the connection if the peer has not accepted a write within this timeout.
inline constexpr uint32_t kWriteTimeoutMs = 500;

class FrameReader {
 public:
  enum class State { kPartial, kFrame, kOversized };
  FrameReader() = default;
  FrameReader(const FrameReader&) = delete;
  FrameReader& operator=(const FrameReader&) = delete;
  ~FrameReader() { Reset(); }
  // Bytes still needed for the header or body; never more than one frame.
  size_t Wanted() const;
  // Takes at most Wanted() bytes. After kFrame, read op() and body(), then Reset().
  State Feed(std::span<const char> bytes);
  uint32_t op() const { return op_; }
  const std::string& body() const { return body_; }
  // Wipes the buffered frame.
  void Reset();

 private:
  std::array<char, 8> header_{};
  size_t header_size_ = 0;
  uint32_t op_ = 0;
  uint32_t length_ = 0;
  std::string body_;
};

namespace testing {
// Connects to this pipe name or socket path instead of discovering Discord's
// IPC endpoint; empty restores discovery.
void SetIpcPath(std::string path);
}  // namespace testing

}  // namespace rex::discord_rpc::detail
