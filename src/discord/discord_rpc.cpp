#include <rex/discord_rpc.h>
#include "../core/relay_protocol.h"
#include "discord_rpc_payload.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

REXCVAR_DEFINE_BOOL(discord_activity, true, "Online",
                    "Discord activity. Enable Discord Rich Presence activity")
    .audience(rex::cvar::Audience::kPlayer);
REXCVAR_DEFINE_INT32(discord_instance_id, -1, "Online",
                     "Discord client to connect to, as in DISCORD_INSTANCE_ID; -1 tries each")
    .range(-1, 9)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .audience(rex::cvar::Audience::kAdvanced)
    .persistence(rex::cvar::Persistence::kNeverPersist);

#if REX_PLATFORM_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace rex::discord_rpc {

namespace detail {

void WipeString(std::string& text) {
  text.resize(text.capacity());
  rex::relay::SecureWipe(std::span<uint8_t>(reinterpret_cast<uint8_t*>(text.data()), text.size()));
  text.clear();
}

void AppendJsonEscaped(std::string& out, std::string_view in) {
  for (char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "\\u%04x", c);
          out += hex;
        } else {
          out += c;
        }
    }
  }
}

std::string JsonEscape(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 2);
  AppendJsonEscaped(out, in);
  return out;
}

std::string ActivityPayload(const Presence& p, const PartyState& party, int64_t start_timestamp,
                            uint32_t pid, std::string_view nonce) {
  // Escaping grows text at most sixfold; reserving that bound means the buffer
  // never reallocates, so no stale copy of the join secret is left behind.
  const size_t text = p.details_.size() + p.state_.size() + p.large_image_key_.size() +
                      p.large_image_text_.size() + p.small_image_key_.size() +
                      p.small_image_text_.size() + party.id.size() + party.join_secret.size() +
                      nonce.size();
  std::string out;
  out.reserve(256 + 6 * text);
  struct Object {
    std::string& out;
    bool first = true;
    void Key(const char* name) {
      if (!first)
        out += ',';
      first = false;
      out += '"';
      out += name;
      out += "\":";
    }
    void String(const char* name, std::string_view value) {
      if (value.empty())
        return;
      Key(name);
      out += '"';
      AppendJsonEscaped(out, value);
      out += '"';
    }
  };

  out += "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
  out += std::to_string(pid);
  out += ",\"activity\":{";
  Object activity{out};
  activity.String("details", p.details_);
  activity.String("state", p.state_);
  if (start_timestamp > 0) {
    activity.Key("timestamps");
    out += "{\"start\":";
    out += std::to_string(start_timestamp);
    out += '}';
  }
  if (!p.large_image_key_.empty() || !p.large_image_text_.empty() || !p.small_image_key_.empty() ||
      !p.small_image_text_.empty()) {
    activity.Key("assets");
    out += '{';
    Object assets{out};
    assets.String("large_image", p.large_image_key_);
    assets.String("large_text", p.large_image_text_);
    assets.String("small_image", p.small_image_key_);
    assets.String("small_text", p.small_image_text_);
    out += '}';
  }
  if (!party.id.empty()) {
    activity.Key("party");
    out += '{';
    Object value{out};
    value.String("id", party.id);
    if (party.max > 0) {
      value.Key("size");
      out += '[';
      out += std::to_string(party.size);
      out += ',';
      out += std::to_string(party.max);
      out += ']';
    }
    out += '}';
  }
  if (!party.join_secret.empty()) {
    activity.Key("secrets");
    out += '{';
    Object secrets{out};
    secrets.String("join", party.join_secret);
    out += '}';
  }
  activity.Key("instance");
  out += "false}},\"nonce\":\"";
  AppendJsonEscaped(out, nonce);
  out += "\"}";
  return out;
}

std::string ClearActivityPayload(uint32_t pid, std::string_view nonce) {
  std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
  payload += std::to_string(pid);
  payload += ",\"activity\":null},\"nonce\":\"";
  payload += JsonEscape(nonce);
  payload += "\"}";
  return payload;
}

std::string SubscribePayload(std::string_view event, std::string_view nonce) {
  return "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"" + JsonEscape(event) + "\",\"nonce\":\"" +
         JsonEscape(nonce) + "\"}";
}

namespace {
bool Space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
std::optional<unsigned> Hex4(std::string_view text, size_t at) {
  if (text.size() < at + 4)
    return std::nullopt;
  unsigned value = 0;
  for (size_t i = at; i < at + 4; ++i) {
    const char c = text[i];
    value <<= 4;
    if (c >= '0' && c <= '9')
      value |= unsigned(c - '0');
    else if (c >= 'a' && c <= 'f')
      value |= unsigned(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      value |= unsigned(c - 'A' + 10);
    else
      return std::nullopt;
  }
  return value;
}
void AppendUtf8(std::string& out, uint32_t code) {
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xC0 | (code >> 6));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xE0 | (code >> 12));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code >> 18));
    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  }
}
}  // namespace

std::optional<std::string> JsonString(std::string_view json, std::string_view key, size_t from) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  for (size_t at = json.find(quoted, from); at != std::string_view::npos;
       at = json.find(quoted, at + 1)) {
    size_t i = at + quoted.size();
    while (i < json.size() && Space(json[i]))
      ++i;
    if (i >= json.size() || json[i] != ':')
      continue;
    ++i;
    while (i < json.size() && Space(json[i]))
      ++i;
    if (i >= json.size() || json[i] != '"')
      return std::nullopt;
    std::string value;
    value.reserve(json.size() - i);
    // Malformed input can follow a complete secret, so every exit wipes the
    // partial value; a returned value has already been moved out.
    struct Wipe {
      std::string& text;
      ~Wipe() { WipeString(text); }
    } wipe{value};
    for (++i; i < json.size(); ++i) {
      const char c = json[i];
      if (c == '"')
        return value;
      if (c != '\\') {
        value += c;
        continue;
      }
      if (++i >= json.size())
        return std::nullopt;
      switch (json[i]) {
        case '"':
        case '\\':
        case '/':
          value += json[i];
          break;
        case 'b':
          value += '\b';
          break;
        case 'f':
          value += '\f';
          break;
        case 'n':
          value += '\n';
          break;
        case 'r':
          value += '\r';
          break;
        case 't':
          value += '\t';
          break;
        case 'u': {
          auto unit = Hex4(json, i + 1);
          if (!unit)
            return std::nullopt;
          i += 4;
          uint32_t code = *unit;
          if (code >= 0xD800 && code <= 0xDBFF) {
            // A high surrogate must be followed by an escaped low surrogate.
            auto low = json.substr(i + 1, 2) == "\\u" ? Hex4(json, i + 3) : std::nullopt;
            if (!low || *low < 0xDC00 || *low > 0xDFFF)
              return std::nullopt;
            code = 0x10000 + ((code - 0xD800) << 10) + (*low - 0xDC00);
            i += 6;
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return std::nullopt;
          }
          AppendUtf8(value, code);
          break;
        }
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<int64_t> JsonInteger(std::string_view json, std::string_view key, size_t from) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  for (size_t at = json.find(quoted, from); at != std::string_view::npos;
       at = json.find(quoted, at + 1)) {
    size_t i = at + quoted.size();
    while (i < json.size() && Space(json[i]))
      ++i;
    if (i >= json.size() || json[i] != ':')
      continue;
    ++i;
    while (i < json.size() && Space(json[i]))
      ++i;
    int64_t value = 0;
    const auto [end, error] = std::from_chars(json.data() + i, json.data() + json.size(), value);
    if (error != std::errc())
      return std::nullopt;
    return value;
  }
  return std::nullopt;
}

void RedactInviteCodes(std::string& text) {
  constexpr std::string_view kMark = "<invite>";
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
  // The run also takes '.', so a code glued to another code is redacted whole.
  auto code_char = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.';
  };
  size_t at = 0;
  while (at + 4 <= text.size()) {
    if (lower(text[at]) != 'r' || lower(text[at + 1]) != 'r' || text[at + 2] != '1' ||
        text[at + 3] != '.') {
      ++at;
      continue;
    }
    size_t end = at + 4;
    while (end < text.size() && code_char(text[end]))
      ++end;
    // Overwrite in place first so the replaced bytes do not linger.
    std::fill(text.begin() + at, text.begin() + end, '\0');
    text.replace(at, end - at, kMark);
    at += kMark.size();
  }
}

std::string DescribeError(std::string_view frame) {
  constexpr size_t kMaxMessage = 200;
  const auto data = frame.find("\"data\"");
  const size_t from = data == std::string_view::npos ? 0 : data;
  const auto code = JsonInteger(frame, "code", from);
  auto message = JsonString(frame, "message", from);
  std::string text = "code " + (code ? std::to_string(*code) : std::string("unknown"));
  if (message && !message->empty()) {
    RedactInviteCodes(*message);
    text += ": ";
    text += std::string_view(*message).substr(0, kMaxMessage);
  }
  if (message)
    WipeString(*message);
  return text;
}

Event ParseEvent(std::string_view frame) {
  Event event;
  event.evt = JsonString(frame, "evt").value_or("");
  const auto data = frame.find("\"data\"");
  if (data == std::string_view::npos)
    return event;
  if (event.evt == "ACTIVITY_JOIN")
    event.secret = JsonString(frame, "secret", data).value_or("");
  else if (event.evt == "ACTIVITY_JOIN_REQUEST")
    event.user = JsonString(frame, "username", data).value_or("");
  return event;
}

size_t FrameReader::Wanted() const {
  if (header_size_ < header_.size())
    return header_.size() - header_size_;
  return length_ - body_.size();
}

FrameReader::State FrameReader::Feed(std::span<const char> bytes) {
  size_t used = 0;
  if (header_size_ < header_.size()) {
    used = std::min(bytes.size(), header_.size() - header_size_);
    std::memcpy(header_.data() + header_size_, bytes.data(), used);
    header_size_ += used;
    if (header_size_ < header_.size())
      return State::kPartial;
    // The supported desktop hosts are little-endian, like the protocol.
    std::memcpy(&op_, header_.data(), 4);
    std::memcpy(&length_, header_.data() + 4, 4);
    if (length_ > kMaxFrameBytes)
      return State::kOversized;
    body_.reserve(length_);
  }
  const size_t take = std::min(bytes.size() - used, size_t(length_) - body_.size());
  body_.append(bytes.data() + used, take);
  return body_.size() == length_ ? State::kFrame : State::kPartial;
}

void FrameReader::Reset() {
  WipeString(body_);
  header_.fill(0);
  header_size_ = 0;
  op_ = 0;
  length_ = 0;
}

}  // namespace detail

namespace {

// One Discord client index, or -1 to try 0-9. The option wins over the
// DISCORD_INSTANCE_ID convention used to test two clients on one machine.
int DiscordInstance() {
  int instance = REXCVAR_GET(discord_instance_id);
  if (instance < 0) {
    if (const char* env = std::getenv("DISCORD_INSTANCE_ID"); env && env[0] >= '0' &&
                                                              env[0] <= '9' && !env[1])
      instance = env[0] - '0';
  }
  return instance;
}

// IPC frame opcodes defined by the Discord RPC protocol.
enum : uint32_t {
  kOpHandshake = 0,
  kOpFrame = 1,
  kOpClose = 2,
  kOpPing = 3,
  kOpPong = 4,
};

std::atomic<bool> g_running{false};
std::thread g_thread;
std::mutex g_mutex;
Presence g_presence;
detail::PartyState g_party;
std::string g_client_id;
std::atomic<bool> g_dirty{false};
// Join events go to a WaitForJoin caller first, otherwise to the handler.
JoinHandler g_join_handler;
std::condition_variable g_join_changed;
bool g_join_waiting = false;
std::string g_join_secret;
std::string g_ipc_path;  // testing override of the IPC endpoint
detail::FrameReader g_reader;

using IoClock = std::chrono::steady_clock;
// The deadline of the operation running on this thread; every connect and
// frame write ends by it, and by kWriteTimeoutMs after it starts.
thread_local IoClock::time_point t_io_deadline = IoClock::time_point::max();

class IoDeadline {
 public:
  explicit IoDeadline(IoClock::time_point deadline) : previous_(t_io_deadline) {
    t_io_deadline = std::min(previous_, deadline);
  }
  ~IoDeadline() { t_io_deadline = previous_; }
  IoDeadline(const IoDeadline&) = delete;
  IoDeadline& operator=(const IoDeadline&) = delete;

 private:
  IoClock::time_point previous_;
};

IoClock::time_point IoLimit() {
  return std::min(t_io_deadline,
                  IoClock::now() + std::chrono::milliseconds(detail::kWriteTimeoutMs));
}

int RemainingMs(IoClock::time_point limit) {
  const auto left =
      std::chrono::duration_cast<std::chrono::milliseconds>(limit - IoClock::now()).count();
  return static_cast<int>(std::clamp<int64_t>(left, 0, detail::kWriteTimeoutMs));
}

bool WantJoinEvents() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_join_waiting || static_cast<bool>(g_join_handler);
}

void HandleFrame(const std::string& frame, bool* out_ready) {
  auto event = detail::ParseEvent(frame);
  if (event.evt == "READY") {
    if (out_ready)
      *out_ready = true;
  } else if (event.evt == "ACTIVITY_JOIN" && !event.secret.empty()) {
    JoinHandler handler;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_join_waiting) {
        detail::WipeString(g_join_secret);
        g_join_secret = event.secret;
        g_join_changed.notify_all();
      } else {
        handler = g_join_handler;
      }
    }
    REXLOG_DEBUG("DiscordRPC: join event received");
    if (handler)
      handler(event.secret);
  } else if (event.evt == "ACTIVITY_JOIN_REQUEST") {
    REXLOG_INFO("DiscordRPC: {} asked to join; answer in Discord",
                event.user.empty() ? std::string("a user") : event.user);
  } else if (event.evt == "ERROR") {
    REXLOG_WARN("DiscordRPC: request failed: {}", detail::DescribeError(frame));
  }
  detail::WipeString(event.secret);
}

#if REX_PLATFORM_WIN32
HANDLE g_pipe = INVALID_HANDLE_VALUE;
bool IsConnected() {
  return g_pipe != INVALID_HANDLE_VALUE;
}
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
int g_sock = -1;
bool IsConnected() {
  return g_sock != -1;
}
#endif

#if REX_PLATFORM_WIN32

bool OpenIpc(const char* name) {
  // Overlapped, so no read or write can wait on the peer without a bound.
  HANDLE h = ::CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  g_pipe = h;
  return true;
}

bool ConnectToDiscord() {
  g_reader.Reset();
  if (!g_ipc_path.empty())
    return OpenIpc(g_ipc_path.c_str());
  const int instance = DiscordInstance();
  for (int i = instance < 0 ? 0 : instance; i < (instance < 0 ? 10 : instance + 1); ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
    if (OpenIpc(name))
      return true;
  }
  REXLOG_WARN("DiscordRPC: no Discord IPC pipe found (is Discord running?)");
  return false;
}

void CloseConnection() {
  g_reader.Reset();
  if (g_pipe != INVALID_HANDLE_VALUE) {
    ::CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
  }
}

#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC

std::vector<std::string> GetSocketDirs() {
  std::vector<std::string> dirs;
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg && *xdg)
    dirs.push_back(xdg);
  const char* tmp = std::getenv("TMPDIR");
  if (tmp && *tmp)
    dirs.push_back(tmp);
  dirs.push_back("/run/user/" + std::to_string(::getuid()));
  dirs.push_back("/tmp");
  return dirs;
}

// The socket is nonblocking before connect, so neither the connect nor later
// writes can wait past `limit`; see WriteAll.
bool OpenIpc(const std::string& path, IoClock::time_point limit) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    return false;
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    ::close(fd);
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  while (true) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      g_sock = fd;
      return true;
    }
    if (errno == EINPROGRESS || errno == EINTR) {
      pollfd writable{fd, POLLOUT, 0};
      int error = 0;
      socklen_t length = sizeof(error);
      if (::poll(&writable, 1, RemainingMs(limit)) > 0 &&
          ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) {
        g_sock = fd;
        return true;
      }
      break;
    }
    // A Unix socket with a full accept queue refuses instead of queueing; retry until the limit.
    if (errno != EAGAIN || IoClock::now() + std::chrono::milliseconds(10) >= limit)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::close(fd);
  return false;
}

bool ConnectToDiscord() {
  g_reader.Reset();
  const auto limit = IoLimit();
  if (!g_ipc_path.empty())
    return OpenIpc(g_ipc_path, limit);
  const int instance = DiscordInstance();
  for (const auto& dir : GetSocketDirs()) {
    for (int i = instance < 0 ? 0 : instance; i < (instance < 0 ? 10 : instance + 1); ++i) {
      if (OpenIpc(dir + "/discord-ipc-" + std::to_string(i), limit))
        return true;
    }
  }
  REXLOG_WARN("DiscordRPC: no Discord IPC socket found (is Discord running?)");
  return false;
}

void CloseConnection() {
  g_reader.Reset();
  if (g_sock != -1) {
    ::close(g_sock);
    g_sock = -1;
  }
}

#endif  // connect/close

#if REX_PLATFORM_WIN32

// One read or write on the overlapped pipe; an operation still pending after
// `timeout_ms` is cancelled and fails.
bool PipeIo(bool write, void* data, DWORD size, DWORD timeout_ms, DWORD* done) {
  *done = 0;
  OVERLAPPED overlapped{};
  overlapped.hEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!overlapped.hEvent)
    return false;
  const BOOL started = write ? ::WriteFile(g_pipe, data, size, nullptr, &overlapped)
                             : ::ReadFile(g_pipe, data, size, nullptr, &overlapped);
  bool ok = false;
  if (started || ::GetLastError() == ERROR_IO_PENDING) {
    if (!started && ::WaitForSingleObject(overlapped.hEvent, timeout_ms) != WAIT_OBJECT_0)
      ::CancelIoEx(g_pipe, &overlapped);
    ok = ::GetOverlappedResult(g_pipe, &overlapped, done, TRUE) != 0;
  }
  ::CloseHandle(overlapped.hEvent);
  return ok;
}

bool WriteAll(const void* buffer, size_t size, IoClock::time_point limit) {
  DWORD written = 0;
  return PipeIo(true, const_cast<void*>(buffer), static_cast<DWORD>(size),
                static_cast<DWORD>(RemainingMs(limit)), &written) &&
         written == size;
}

#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// The socket is nonblocking; the write fails once `limit` passes, however
// slowly the peer keeps accepting data.
bool WriteAll(const void* buffer, size_t size, IoClock::time_point limit) {
  const char* ptr = static_cast<const char*>(buffer);
  while (size > 0) {
    const ssize_t n = ::send(g_sock, ptr, size, kSendFlags);
    if (n > 0) {
      ptr += n;
      size -= static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
      return false;
    pollfd writable{g_sock, POLLOUT, 0};
    if (::poll(&writable, 1, RemainingMs(limit)) <= 0)
      return false;
  }
  return true;
}

#endif  // WriteAll

// The whole frame shares one limit: kWriteTimeoutMs from now, or the caller's
// earlier deadline. A failed or timed-out write leaves the stream mid-frame,
// so it closes the connection.
bool WriteFrame(uint32_t op, const std::string& payload) {
  if (!IsConnected())
    return false;
  const auto limit = IoLimit();
  uint32_t header[2] = {op, static_cast<uint32_t>(payload.size())};
  if (!WriteAll(header, sizeof(header), limit) ||
      (!payload.empty() && !WriteAll(payload.data(), payload.size(), limit))) {
    REXLOG_WARN("DiscordRPC: IPC write failed or timed out; disconnecting");
    CloseConnection();
    return false;
  }
  return true;
}

#if REX_PLATFORM_WIN32

// Reads only bytes already waiting, so a peer that withholds a frame body never
// blocks the caller. Returns 0 when nothing is waiting and -1 on failure.
ptrdiff_t ReadAvailable(char* buffer, size_t size) {
  DWORD avail = 0;
  if (!::PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr))
    return -1;
  if (avail == 0)
    return 0;
  // The bytes are already waiting, so the read completes without waiting on the peer.
  DWORD read = 0;
  if (!PipeIo(false, buffer, static_cast<DWORD>(std::min<size_t>(size, avail)),
              detail::kWriteTimeoutMs, &read) ||
      read == 0)
    return -1;
  return static_cast<ptrdiff_t>(read);
}

#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC

bool HasData() {
  if (!IsConnected())
    return false;
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(g_sock, &fds);
  timeval tv{0, 0};
  return ::select(g_sock + 1, &fds, nullptr, nullptr, &tv) > 0;
}

ptrdiff_t ReadAvailable(char* buffer, size_t size) {
  if (!HasData())
    return 0;
  const ssize_t n = ::recv(g_sock, buffer, size, 0);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return 0;
  return n > 0 ? static_cast<ptrdiff_t>(n) : -1;
}

#endif  // ReadAvailable

// Handles waiting frames until none remain, kMaxFramesPerDrain have been
// handled, or `deadline` passes.
void DrainMessages(bool* out_ready, std::chrono::steady_clock::time_point deadline) {
  IoDeadline io(deadline);  // PONG writes end with the pass
  std::array<char, 4096> chunk;
  int frames = 0;
  while (IsConnected() && frames < detail::kMaxFramesPerDrain &&
         std::chrono::steady_clock::now() < deadline) {
    const ptrdiff_t got = ReadAvailable(chunk.data(), std::min(chunk.size(), g_reader.Wanted()));
    if (got < 0) {
      CloseConnection();
      return;
    }
    if (got == 0)
      return;
    const auto state = g_reader.Feed(std::span<const char>(chunk.data(), size_t(got)));
    rex::relay::SecureWipe(
        std::span<uint8_t>(reinterpret_cast<uint8_t*>(chunk.data()), size_t(got)));
    if (state == detail::FrameReader::State::kOversized) {
      REXLOG_WARN("DiscordRPC: IPC frame over {} bytes; disconnecting", detail::kMaxFrameBytes);
      CloseConnection();
      return;
    }
    if (state == detail::FrameReader::State::kPartial)
      continue;
    ++frames;
    const uint32_t op = g_reader.op();
    if (op == kOpPing) {
      WriteFrame(kOpPong, g_reader.body());
    } else if (op == kOpFrame) {
      HandleFrame(g_reader.body(), out_ready);
    } else if (op == kOpClose) {
      REXLOG_WARN("DiscordRPC: server closed: {}", detail::DescribeError(g_reader.body()));
      CloseConnection();
      return;
    }
    g_reader.Reset();
  }
}

#if REX_PLATFORM_WIN32
uint32_t GetPid() {
  return static_cast<uint32_t>(::GetCurrentProcessId());
}
std::string GetNonce() {
  return std::to_string(::GetTickCount());
}
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
uint32_t GetPid() {
  return static_cast<uint32_t>(::getpid());
}
std::string GetNonce() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
}
#endif

bool SendHandshake() {
  std::string payload = std::string("{\"v\":1,\"client_id\":\"") + g_client_id + "\"}";
  return WriteFrame(kOpHandshake, payload);
}

bool SendClearActivity() {
  return WriteFrame(kOpFrame, detail::ClearActivityPayload(GetPid(), GetNonce()));
}

bool SendActivity(int64_t start_timestamp) {
  std::string payload;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    payload = detail::ActivityPayload(g_presence, g_party, start_timestamp, GetPid(), GetNonce());
  }
  const bool sent = WriteFrame(kOpFrame, payload);
  detail::WipeString(payload);
  return sent;
}

bool SendSubscriptions() {
  return WriteFrame(kOpFrame, detail::SubscribePayload("ACTIVITY_JOIN", GetNonce() + "j")) &&
         WriteFrame(kOpFrame, detail::SubscribePayload("ACTIVITY_JOIN_REQUEST", GetNonce() + "r"));
}

void WorkerThread() {
  using clock = std::chrono::steady_clock;
  auto next_reconnect = clock::now();
  int64_t start_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  bool handshake_sent = false;
  bool ready = false;
  bool subscribed = false;
  bool last_activity_enabled = REXCVAR_GET(discord_activity);

  while (g_running.load()) {
    if (!IsConnected()) {
      if (clock::now() < next_reconnect) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      if (!ConnectToDiscord()) {
        next_reconnect = clock::now() + std::chrono::seconds(15);
        continue;
      }
      handshake_sent = false;
      ready = false;
      subscribed = false;
    }

    if (!handshake_sent) {
      if (!SendHandshake()) {
        CloseConnection();
        next_reconnect = clock::now() + std::chrono::seconds(15);
        continue;
      }
      handshake_sent = true;
    }

    DrainMessages(&ready, clock::now() + std::chrono::milliseconds(100));

    // Join events are independent of discord_activity; they need only a join consumer.
    if (ready && !subscribed && WantJoinEvents()) {
      if (!SendSubscriptions()) {
        CloseConnection();
        next_reconnect = clock::now() + std::chrono::seconds(5);
        continue;
      }
      subscribed = true;
    }

    const bool activity_enabled = REXCVAR_GET(discord_activity);
    if (activity_enabled != last_activity_enabled) {
      last_activity_enabled = activity_enabled;
      g_dirty = true;
    }

    if (ready && g_dirty.exchange(false)) {
      if (activity_enabled) {
        if (!SendActivity(start_timestamp)) {
          CloseConnection();
          next_reconnect = clock::now() + std::chrono::seconds(5);
          continue;
        }
      } else {
        if (!SendClearActivity()) {
          CloseConnection();
          next_reconnect = clock::now() + std::chrono::seconds(5);
          continue;
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  if (IsConnected()) {
    SendClearActivity();
    WriteFrame(kOpClose, "{}");
    CloseConnection();
  }
}

void SyncClearAndClose() {
  if (!IsConnected())
    return;
  SendClearActivity();
  WriteFrame(kOpClose, "{}");
  CloseConnection();
}

#if REX_PLATFORM_WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD) {
  SyncClearAndClose();
  return FALSE;
}
#endif

struct AtExitInstaller {
  AtExitInstaller() {
    std::atexit([] { SyncClearAndClose(); });
#if REX_PLATFORM_WIN32
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif
  }
} g_atExitInstaller;

}  // namespace

void Start(const char* client_id, const Presence& initial) {
  if (!client_id || *client_id == '\0') {
    REXLOG_WARN("DiscordRPC: Start() called with null/empty client_id — skipping");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_client_id = client_id;
    g_presence = initial;
  }
  g_dirty = true;
  if (g_running.exchange(true))
    return;
  g_thread = std::thread(WorkerThread);
}

void SetPresence(const Presence& p) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence = p;
  }
  g_dirty = true;
}

void SetDetails(const std::string& details) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence.details_ = details;
  }
  g_dirty = true;
}

void SetState(const std::string& state) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence.state_ = state;
  }
  g_dirty = true;
}

void SetLargeImage(const std::string& key, const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence.large_image_key_ = key;
    g_presence.large_image_text_ = text;
  }
  g_dirty = true;
}

void SetSmallImage(const std::string& key, const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presence.small_image_key_ = key;
    g_presence.small_image_text_ = text;
  }
  g_dirty = true;
}

void SetParty(const std::string& id, int size, int max) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_party.id = id;
    g_party.size = size;
    g_party.max = max;
  }
  g_dirty = true;
}

void ClearParty() {
  SetParty({}, 0, 0);
}

void SetJoinSecret(const std::string& secret) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    detail::WipeString(g_party.join_secret);
    g_party.join_secret = secret;
  }
  g_dirty = true;
}

void ClearJoinSecret() {
  SetJoinSecret({});
}

void SetJoinHandler(JoinHandler handler) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_join_handler = std::move(handler);
}

std::string WaitForJoin(const char* client_id, std::chrono::milliseconds timeout) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + timeout;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_join_waiting)
      return {};
    g_join_waiting = true;
    detail::WipeString(g_join_secret);
  }
  auto finish = [] {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_join_waiting = false;
    return std::exchange(g_join_secret, {});
  };
  if (g_running.load()) {
    // The worker owns the connection and subscribes on its next pass.
    std::unique_lock<std::mutex> lock(g_mutex);
    g_join_changed.wait_until(lock, deadline, [] { return !g_join_secret.empty(); });
    lock.unlock();
    return finish();
  }
  if (!client_id || *client_id == '\0') {
    finish();
    return {};
  }
  // No worker yet: use the shared connection on this thread and close it
  // cleanly before returning, so a later Start connects afresh.
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_client_id = client_id;
  }
  // Connecting, writing and draining all end by the caller's deadline.
  IoDeadline io(deadline);
  bool ready = false;
  bool subscribed = false;
  if (ConnectToDiscord() && SendHandshake()) {
    while (clock::now() < deadline && IsConnected()) {
      DrainMessages(&ready, std::min(deadline, clock::now() + std::chrono::milliseconds(100)));
      if (ready && !subscribed) {
        if (!SendSubscriptions())
          break;
        subscribed = true;
      }
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_join_secret.empty())
          break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (IsConnected()) {
    WriteFrame(kOpClose, "{}");
    CloseConnection();
  }
  return finish();
}

void Stop() {
  if (g_running.exchange(false) && g_thread.joinable())
    g_thread.join();
  std::lock_guard<std::mutex> lock(g_mutex);
  detail::WipeString(g_party.join_secret);
}

namespace detail::testing {
void SetIpcPath(std::string path) {
  g_ipc_path = std::move(path);
}
}  // namespace detail::testing

}  // namespace rex::discord_rpc
