#include "host_session_data.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <random>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <rex/logging.h>

#include "../../core/relay_protocol.h"
#include "relay_system_link.h"
#include "system_link_config.h"

namespace rex::kernel::xam::host_data {
namespace {

using rex::session_data::State;
using rex::session_data::Status;

static_assert(kMagic == relay_link::kSideChannelMagic);
static_assert(kMaxMessageSize <= relay::kMaxPayload);

relay::Digest Mac(const SessionKey& key, std::span<const uint8_t> bytes) {
  Bytes input(kMacLabel.begin(), kMacLabel.end());
  input.insert(input.end(), bytes.begin(), bytes.end());
  return relay::HmacSha256(key, input);
}

Nonce OsRandomNonce() {
  Nonce nonce{};
  if (auto seed = relay::RandomSeed()) {
    std::copy_n(seed->begin(), nonce.size(), nonce.begin());
    relay::SecureWipe(*seed);
    return nonce;
  }
  std::random_device device;
  for (auto& byte : nonce)
    byte = static_cast<uint8_t>(device());
  return nonce;
}

const char* LinkName(Link link) {
  switch (link) {
    case Link::kRelay:
      return "relay";
    case Link::kLan:
      return "LAN";
    case Link::kNone:
      break;
  }
  return "none";
}

}  // namespace

bool HasMagic(std::span<const uint8_t> bytes) {
  return bytes.size() >= kMagic.size() &&
         std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) == 0;
}

Bytes Encode(const Message& message, const SessionKey& key) {
  if (message.data.size() > rex::session_data::kMaxBytes ||
      (message.type == Type::kRequest && !message.data.empty()) ||
      (message.type != Type::kRequest && message.type != Type::kReply))
    return {};
  Bytes out(kMagic.begin(), kMagic.end());
  out.reserve(kRequestSize + message.data.size());
  out.push_back(kVersion);
  out.push_back(static_cast<uint8_t>(message.type));
  out.push_back(static_cast<uint8_t>(message.data.size()));
  out.push_back(static_cast<uint8_t>(message.data.size() >> 8));
  out.insert(out.end(), message.session_id.begin(), message.session_id.end());
  out.insert(out.end(), message.nonce.begin(), message.nonce.end());
  out.insert(out.end(), message.data.begin(), message.data.end());
  const auto mac = Mac(key, out);
  out.insert(out.end(), mac.begin(), mac.end());
  return out;
}

std::optional<Message> Decode(std::span<const uint8_t> bytes, const SessionKey& key) {
  if (bytes.size() < kRequestSize || bytes.size() > kMaxMessageSize || !HasMagic(bytes) ||
      bytes[4] != kVersion)
    return std::nullopt;
  const auto type = static_cast<Type>(bytes[5]);
  const size_t length = bytes[6] | (size_t(bytes[7]) << 8);
  if ((type != Type::kRequest && type != Type::kReply) || (type == Type::kRequest && length != 0) ||
      bytes.size() != kRequestSize + length)
    return std::nullopt;
  const auto body = bytes.first(bytes.size() - kMacSize);
  if (!relay::ConstantTimeEqual(Mac(key, body), bytes.last(kMacSize)))
    return std::nullopt;
  Message message;
  message.type = type;
  std::copy_n(bytes.begin() + 8, message.session_id.size(), message.session_id.begin());
  std::copy_n(bytes.begin() + 16, message.nonce.size(), message.nonce.begin());
  message.data.assign(bytes.begin() + kHeaderSize, bytes.begin() + kHeaderSize + length);
  return message;
}

SessionInfo ParseSessionInfo(std::span<const uint8_t, 60> info) {
  SessionInfo parsed;
  std::copy_n(info.begin(), parsed.id.size(), parsed.id.begin());
  std::memcpy(&parsed.host_ipv4, info.data() + 8, sizeof(parsed.host_ipv4));
  std::copy_n(info.begin() + 44, parsed.key.size(), parsed.key.begin());
  return parsed;
}

Core::Core(RandomNonce random) : random_(random ? std::move(random) : RandomNonce(OsRandomNonce)) {}

void Core::SetProvider(Provider provider) {
  std::lock_guard lock(mutex_);
  provider_ = std::move(provider);
}

void Core::StartGenerationLocked(const SessionInfo& info) {
  ++generation_;
  state_ = State::kNone;
  bytes_.clear();
  info_ = info;
  serving_ = false;
  answered_.clear();
  host_ = {};
  nonces_.clear();
  nonce_uses_ = 0;
  changed_.notify_all();
}

void Core::HostCreated(const SessionInfo& info) {
  Provider provider;
  uint64_t generation = 0;
  {
    std::lock_guard lock(mutex_);
    StartGenerationLocked(info);
    generation = generation_;
    provider = provider_;
  }
  // The provider runs on the creating guest thread, outside the lock, so it
  // may read Current() or replace itself. An exception must not unwind into
  // the guest's XGI dispatch.
  Bytes bytes;
  try {
    if (provider)
      bytes = provider();
  } catch (const std::exception& error) {
    REXLOG_ERROR("Host session data: provider threw: {}; hosting without data", error.what());
    bytes.clear();
  } catch (...) {
    REXLOG_ERROR("Host session data: provider threw; hosting without data");
    bytes.clear();
  }
  if (bytes.size() > rex::session_data::kMaxBytes) {
    REXKRNL_WARN("Host session data: provider returned {} bytes; truncated to {}", bytes.size(),
                 rex::session_data::kMaxBytes);
    bytes.resize(rex::session_data::kMaxBytes);
  }
  std::lock_guard lock(mutex_);
  if (generation_ != generation) {
    REXKRNL_DEBUG("Host session data: session replaced while the provider ran");
    return;
  }
  state_ = State::kHost;
  bytes_ = std::move(bytes);
  serving_ = std::any_of(info.id.begin(), info.id.end(), [](uint8_t b) { return b != 0; });
  REXKRNL_INFO("Host session data: hosting with {} bytes{}", bytes_.size(),
               serving_ ? "" : " (no System Link session to serve)");
  changed_.notify_all();
}

std::optional<Outgoing> Core::JoinCreated(const SessionInfo& info, Route host,
                                          Clock::time_point now) {
  std::lock_guard lock(mutex_);
  StartGenerationLocked(info);
  if (host.link == Link::kNone || !host.ipv4) {
    state_ = State::kUnavailable;
    REXKRNL_DEBUG("Host session data: joined without a route to the host");
    return std::nullopt;
  }
  state_ = State::kPending;
  host_ = host;
  give_up_ = now + kGiveUp;
  retry_delay_ = kFirstRetry;
  next_retry_ = now + retry_delay_;
  REXKRNL_DEBUG("Host session data: requesting from the host over {}", LinkName(host.link));
  return RequestLocked();
}

void Core::Deleted() {
  std::lock_guard lock(mutex_);
  if (state_ != State::kNone)
    REXKRNL_DEBUG("Host session data: session deleted");
  StartGenerationLocked({});
}

bool Core::TakeReplyTokenLocked(Clock::time_point now) {
  if (reply_refill_ && now > *reply_refill_)
    reply_tokens_ =
        std::min(kRepliesPerSecond,
                 reply_tokens_ + kRepliesPerSecond *
                                     std::chrono::duration<double>(now - *reply_refill_).count());
  if (!reply_refill_ || now > *reply_refill_)
    reply_refill_ = now;
  if (reply_tokens_ < 1.0)
    return false;
  reply_tokens_ -= 1.0;
  return true;
}

std::optional<Outgoing> Core::Receive(Route from, std::span<const uint8_t> bytes,
                                      Clock::time_point now) {
  if (bytes.size() < kRequestSize || bytes.size() > kMaxMessageSize || !HasMagic(bytes)) {
    REXKRNL_DEBUG("Host session data: dropped a {}-byte datagram from {}", bytes.size(),
                  LinkName(from.link));
    return std::nullopt;
  }
  std::lock_guard lock(mutex_);
  const bool host = state_ == State::kHost && serving_;
  if (!host && state_ != State::kPending) {
    REXKRNL_DEBUG("Host session data: dropped a message without a matching session");
    return std::nullopt;
  }
  auto message = Decode(bytes, info_.key);
  const Type expected = host ? Type::kRequest : Type::kReply;
  if (!message || message->type != expected || message->session_id != info_.id ||
      (!host && std::find(nonces_.begin(), nonces_.end(), message->nonce) == nonces_.end())) {
    REXKRNL_DEBUG("Host session data: dropped an invalid or unauthenticated message from {}",
                  LinkName(from.link));
    return std::nullopt;
  }
  if (host) {
    auto answered = std::find_if(answered_.begin(), answered_.end(),
                                 [&](const auto& entry) { return entry.first == message->nonce; });
    if (answered != answered_.end() && answered->second >= kRepliesPerNonce) {
      REXKRNL_DEBUG("Host session data: dropped a replayed request");
      return std::nullopt;
    }
    if (!TakeReplyTokenLocked(now)) {
      REXKRNL_DEBUG("Host session data: dropped a request over the reply rate limit");
      return std::nullopt;
    }
    if (answered == answered_.end()) {
      // The oldest nonce goes first; the rate limit still bounds its replays.
      if (answered_.size() == kMaxAnsweredNonces)
        answered_.erase(answered_.begin());
      answered_.emplace_back(message->nonce, 0);
      answered = answered_.end() - 1;
    }
    ++answered->second;
    Message reply{Type::kReply, info_.id, message->nonce, bytes_};
    return Outgoing{from, Encode(reply, info_.key)};
  }
  state_ = State::kReceived;
  bytes_ = std::move(message->data);
  REXKRNL_INFO("Host session data: received {} bytes from the host", bytes_.size());
  changed_.notify_all();
  return std::nullopt;
}

std::optional<Outgoing> Core::Due(Clock::time_point now) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kPending)
    return std::nullopt;
  if (now >= give_up_) {
    state_ = State::kUnavailable;
    REXKRNL_WARN("Host session data: the host did not answer within {} seconds",
                 std::chrono::duration_cast<std::chrono::seconds>(kGiveUp).count());
    changed_.notify_all();
    return std::nullopt;
  }
  if (now < next_retry_)
    return std::nullopt;
  retry_delay_ = std::min(retry_delay_ * 2, kMaxRetry);
  next_retry_ = now + retry_delay_;
  return RequestLocked();
}

std::optional<Clock::time_point> Core::NextDeadline() const {
  std::lock_guard lock(mutex_);
  if (state_ != State::kPending)
    return std::nullopt;
  return std::min(next_retry_, give_up_);
}

Status Core::Current() const {
  std::lock_guard lock(mutex_);
  return {state_, bytes_};
}

Status Core::Wait(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  const uint64_t generation = generation_;
  changed_.wait_for(lock, timeout,
                    [&] { return state_ != State::kPending || generation_ != generation; });
  if (generation_ != generation)
    return {};
  return {state_, bytes_};
}

Outgoing Core::RequestLocked() {
  if (nonces_.empty() || nonce_uses_ >= kRepliesPerNonce) {
    nonces_.push_back(random_());
    nonce_uses_ = 0;
  }
  ++nonce_uses_;
  return {host_, Encode({Type::kRequest, info_.id, nonces_.back(), {}}, info_.key)};
}

// LanLink

namespace {
#if defined(_WIN32)
using Native = SOCKET;
using AddressSize = int;
constexpr Native kInvalidSocket = INVALID_SOCKET;
#else
using Native = int;
using AddressSize = socklen_t;
constexpr Native kInvalidSocket = -1;
#endif

void CloseSocket(Native socket) {
  if (socket == kInvalidSocket)
    return;
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}

// Returns the bound socket and its port, or kInvalidSocket.
Native OpenBound(uint32_t ipv4, uint16_t port, uint16_t* bound_port) {
  Native socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket == kInvalidSocket)
    return kInvalidSocket;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = ipv4;
  address.sin_port = htons(port);
  AddressSize size = sizeof(address);
  bool ok = ::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0;
#if defined(_WIN32)
  // An ICMP port-unreachable for an earlier send would otherwise fail the next receive.
  BOOL resets = FALSE;
  DWORD returned = 0;
  ok = ok && WSAIoctl(socket, SIO_UDP_CONNRESET, &resets, sizeof(resets), nullptr, 0, &returned,
                      nullptr, nullptr) == 0;
  // Nonblocking, so a full send buffer never stalls a guest thread in Send.
  u_long nonblocking = 1;
  ok = ok && ioctlsocket(socket, FIONBIO, &nonblocking) == 0;
#else
  ok = ok && fcntl(socket, F_SETFL, O_NONBLOCK) == 0;
#endif
  if (!ok) {
    CloseSocket(socket);
    return kInvalidSocket;
  }
  *bound_port = ntohs(address.sin_port);
  return socket;
}

std::string Ipv4Text(uint32_t ipv4) {
  in_addr address{};
  address.s_addr = ipv4;
  char text[16]{};
  return inet_ntop(AF_INET, &address, text, sizeof(text)) ? text : "<invalid>";
}
}  // namespace

struct LanLink::Impl {
  uint16_t requested_port = 0;
  Handler handler;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::thread worker;
  bool stop = false;
  bool rebind = false;
  uint32_t desired = 0;
  Native socket = kInvalidSocket;  // published for Send; owned and closed by the worker
  uint16_t bound_port = 0;
#if defined(_WIN32)
  bool winsock = false;
#endif

  void Run() {
    std::array<uint8_t, kMaxMessageSize + 1> buffer{};
    Native owned = kInvalidSocket;
    uint32_t bound = 0;
    for (;;) {
      uint32_t want = 0;
      bool reopen = false;
      {
        std::unique_lock lock(mutex);
        if (owned == kInvalidSocket)
          changed.wait(lock, [this] { return stop || rebind; });
        if (stop)
          break;
        want = desired;
        reopen = std::exchange(rebind, false) && (want != bound || owned == kInvalidSocket);
        if (reopen) {
          socket = kInvalidSocket;
          bound_port = 0;
        }
      }
      if (reopen) {
        CloseSocket(owned);
        owned = kInvalidSocket;
        bound = want;
        if (!want)
          continue;
        uint16_t port = 0;
        owned = OpenBound(want, requested_port, &port);
        if (owned == kInvalidSocket) {
          REXKRNL_WARN("Host session data: cannot bind UDP {}:{}", Ipv4Text(want), requested_port);
          continue;
        }
        REXKRNL_DEBUG("Host session data: LAN side socket bound to {}:{}", Ipv4Text(want), port);
        std::lock_guard lock(mutex);
        socket = owned;
        bound_port = port;
        continue;
      }
      // A short wait bounds how long a rebind or stop request waits for the worker.
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(owned, &readable);
      timeval timeout{0, 200000};
      if (select(static_cast<int>(owned + 1), &readable, nullptr, nullptr, &timeout) <= 0)
        continue;
      sockaddr_in from{};
      AddressSize from_size = sizeof(from);
      const auto count =
          recvfrom(owned, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
                   0, reinterpret_cast<sockaddr*>(&from), &from_size);
      if (count <= 0)
        continue;
      const std::span<const uint8_t> bytes{buffer.data(), static_cast<size_t>(count)};
      if (!HasMagic(bytes) || bytes.size() > kMaxMessageSize) {
        REXKRNL_DEBUG("Host session data: dropped a {}-byte LAN datagram", bytes.size());
        continue;
      }
      if (handler)
        handler({Link::kLan, from.sin_addr.s_addr, ntohs(from.sin_port)}, bytes);
    }
    CloseSocket(owned);
  }
};

LanLink::LanLink(uint16_t port, Handler handler) : impl_(std::make_unique<Impl>()) {
  impl_->requested_port = port;
  impl_->handler = std::move(handler);
#if defined(_WIN32)
  WSADATA data{};
  impl_->winsock = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
}

LanLink::~LanLink() {
  Stop();
#if defined(_WIN32)
  if (impl_->winsock)
    WSACleanup();
#endif
}

void LanLink::Stop() {
  std::thread worker;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stop = true;
    worker = std::move(impl_->worker);
  }
  impl_->changed.notify_all();
  if (worker.joinable())
    worker.join();
}

void LanLink::Bind(uint32_t ipv4) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->stop)
    return;
  impl_->desired = ipv4;
  impl_->rebind = true;
  if (!impl_->worker.joinable())
    impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
  impl_->changed.notify_all();
}

bool LanLink::Send(Route to, std::span<const uint8_t> bytes) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->socket == kInvalidSocket || !to.ipv4 || !to.port)
    return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = to.ipv4;
  address.sin_port = htons(to.port);
  return sendto(impl_->socket, reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == static_cast<int>(bytes.size());
}

uint16_t LanLink::port() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->bound_port;
}

// Process service

Service::Service(Sender sender, uint16_t lan_port)
    : sender_(std::move(sender)),
      relay_handler_(!sender_),
      lan_(lan_port, [this](Route from, std::span<const uint8_t> bytes) { Receive(from, bytes); }) {
  if (relay_handler_)
    relay_link::SystemLinkClient().SetSideChannelHandler(
        [this](uint32_t source, std::span<const uint8_t> bytes) {
          Receive({Link::kRelay, source, 0}, bytes);
        });
}

Service::~Service() {
  Shutdown();
}

void Service::Created(uint32_t flags, const lan::SessionSnapshot& session) {
  const auto mode = system_link::CurrentMode();
  const SessionInfo info = session.created
                               ? ParseSessionInfo(std::span<const uint8_t, 60>(session.info))
                               : SessionInfo{};
  if (mode == system_link::Mode::kLan && session.created)
    lan_.Bind(lan::SystemLinkState().local_ipv4());
  if (flags & lan::kSessionCreateHost) {
    core_.HostCreated(info);
    return;
  }
  Route host;
  if (session.created && info.host_ipv4) {
    if (mode == system_link::Mode::kRelay)
      host = {Link::kRelay, info.host_ipv4, 0};
    else if (mode == system_link::Mode::kLan)
      host = {Link::kLan, info.host_ipv4, kLanPort};
  }
  if (host.link == Link::kNone && mode != system_link::Mode::kOff)
    REXKRNL_WARN("Host session data: joined session has no usable host address");
  Join(info, host);
}

void Service::Join(const SessionInfo& info, Route host) {
  if (auto first = core_.JoinCreated(info, host, Clock::now()))
    Send(*first);
  Kick();
}

void Service::Receive(Route from, std::span<const uint8_t> bytes) {
  if (auto reply = core_.Receive(from, bytes, Clock::now()))
    Send(*reply);
}

void Service::Shutdown() {
  std::thread timer;
  {
    std::lock_guard lock(timer_mutex_);
    if (stopped_)
      return;
    stopped_ = true;
    timer = std::move(timer_);
  }
  timer_changed_.notify_all();
  if (timer.joinable())
    timer.join();
  lan_.Stop();
  // A handler call already running finishes against this never-freed service
  // and finds it stopped; the relay client's own destructor joins its thread.
  if (relay_handler_)
    relay_link::SystemLinkClient().SetSideChannelHandler({});
}

void Service::Send(const Outgoing& outgoing) {
  {
    std::lock_guard lock(timer_mutex_);
    if (stopped_)
      return;
  }
  bool sent = false;
  if (sender_)
    sent = sender_(outgoing);
  else if (outgoing.route.link == Link::kRelay)
    sent = relay_link::SystemLinkClient().SendSideChannel(outgoing.route.ipv4, outgoing.bytes);
  else if (outgoing.route.link == Link::kLan)
    sent = lan_.Send(outgoing.route, outgoing.bytes);
  if (!sent)
    REXKRNL_DEBUG("Host session data: {} send failed", LinkName(outgoing.route.link));
}

// Wakes the retry timer, starting it on the first joined session.
void Service::Kick() {
  std::lock_guard lock(timer_mutex_);
  if (stopped_)
    return;
  wake_ = true;
  if (!timer_.joinable())
    timer_ = std::thread([this] { TimerLoop(); });
  timer_changed_.notify_all();
}

void Service::TimerLoop() {
  std::unique_lock lock(timer_mutex_);
  while (!stopped_) {
    if (const auto deadline = core_.NextDeadline())
      timer_changed_.wait_until(lock, *deadline, [this] { return wake_ || stopped_; });
    else
      timer_changed_.wait(lock, [this] { return wake_ || stopped_; });
    if (stopped_)
      break;
    wake_ = false;
    lock.unlock();
    if (auto retry = core_.Due(Clock::now()))
      Send(*retry);
    lock.lock();
  }
}

Service& ProcessService() {
  static Service* service = [] {
    // Construct the relay client first so its destructor is registered before
    // the atexit hook below; exit handlers run in reverse order, so the hook
    // stops our timer and LAN worker while the relay client still exists.
    relay_link::SystemLinkClient();
    auto* created = new Service();
    std::atexit([] { ProcessService().Shutdown(); });
    return created;
  }();
  return *service;
}

void OnSessionCreate(uint32_t flags, const lan::SessionSnapshot& session) {
  ProcessService().Created(flags, session);
}

void OnSessionDelete() {
  ProcessService().core().Deleted();
}

void OnNetworkCleanup() {
  ProcessService().core().Deleted();
}

}  // namespace rex::kernel::xam::host_data

namespace rex::session_data {

void SetHostDataProvider(std::function<std::vector<std::uint8_t>()> provider) {
  rex::kernel::xam::host_data::ProcessService().core().SetProvider(std::move(provider));
}

Status Current() {
  return rex::kernel::xam::host_data::ProcessService().core().Current();
}

Status WaitForHostData(std::chrono::milliseconds timeout) {
  return rex::kernel::xam::host_data::ProcessService().core().Wait(timeout);
}

}  // namespace rex::session_data
