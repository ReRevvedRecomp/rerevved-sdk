#include "port_mapping_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include <rex/logging.h>

#include "relay_transport.h"

namespace rex::port_mapping {
namespace internal {
namespace {
std::array<uint8_t, 4> Octets(uint32_t address) {
  std::array<uint8_t, 4> octets{};
  std::memcpy(octets.data(), &address, octets.size());
  return octets;
}

struct Answer {
  NatPmpOutcome outcome = NatPmpOutcome::kSilent;
  relay::Bytes reply;
};

Answer Ask(relay::Endpoint gateway, const relay::Bytes& request, size_t reply_size,
           const std::atomic<bool>* stop, bool* sent = nullptr) {
  // A loopback gateway is a test; staying on loopback raises no firewall prompt.
  auto socket = relay::UdpSocket::Open(Octets(gateway.ipv4)[0] == 127 ? "127.0.0.1" : "0.0.0.0", 0);
  if (!socket)
    return {};
  for (const uint32_t wait_ms : {250u, 500u, 1000u}) {
    if (stop && stop->load())
      return {};
    if (!(*socket)->Send({gateway, request}))
      return {};
    if (sent)
      *sent = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      if (left <= 0)
        break;
      auto received = (*socket)->Receive(static_cast<uint32_t>(left));
      if (!received)
        return {};
      if (!*received)
        continue;
      const auto& reply = **received;
      // The reply opcode is the request's plus 128; bytes 2 and 3 are the
      // result. An error reply can be shorter than a success.
      if (reply.endpoint != gateway || reply.bytes.size() < 4 || reply.bytes[0] != 0 ||
          reply.bytes[1] != request[1] + 128)
        continue;
      if (reply.bytes[2] != 0 || reply.bytes[3] != 0)
        return {NatPmpOutcome::kRefused, {}};
      if (reply.bytes.size() < reply_size)
        continue;
      return {NatPmpOutcome::kGranted, reply.bytes};
    }
  }
  return {};
}

Answer AskForPort(relay::Endpoint gateway, uint16_t port, uint16_t outside_port, uint32_t lifetime,
                  const std::atomic<bool>* stop, bool* sent = nullptr) {
  const relay::Bytes request{0,
                             1,  // map UDP
                             0,
                             0,
                             static_cast<uint8_t>(port >> 8),
                             static_cast<uint8_t>(port),
                             static_cast<uint8_t>(outside_port >> 8),
                             static_cast<uint8_t>(outside_port),
                             static_cast<uint8_t>(lifetime >> 24),
                             static_cast<uint8_t>(lifetime >> 16),
                             static_cast<uint8_t>(lifetime >> 8),
                             static_cast<uint8_t>(lifetime)};
  auto answer = Ask(gateway, request, 16, stop, sent);
  if (answer.outcome == NatPmpOutcome::kGranted &&
      (answer.reply[8] != request[4] || answer.reply[9] != request[5]))
    return {};
  return answer;
}

std::string AddressText(uint32_t address) {
  const auto octets = Octets(address);
  return fmt::format("{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
}

const char* FailureName(Failure failure) {
  switch (failure) {
    case Failure::kNoNetwork:
      return "no network";
    case Failure::kNoDiscovery:
      return "SSDP Discovery is disabled";
    case Failure::kNoRouter:
      return "no router answered";
    case Failure::kPortsTaken:
      return "the ports are taken";
    case Failure::kSharedAddress:
      return "the router has no public address";
    case Failure::kRefused:
      return "the router refused";
    case Failure::kNone:
      break;
  }
  return "none";
}

struct Mapper {
  Environment environment;
  Options options;
  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stop{false};
  Status status;  // guarded by mutex
  std::thread worker;

  void Publish(Status next) {
    {
      std::lock_guard lock(mutex);
      status = next;
    }
    if (options.changed)
      options.changed(next);
  }
  bool StopWithin(uint32_t seconds) {
    std::unique_lock lock(mutex);
    return wake.wait_for(lock, std::chrono::seconds(seconds), [this] { return stop.load(); });
  }

  void Run() {
    Failure cause = Failure::kNoNetwork;
    std::optional<Grant> grant;
    bool upnp = false;
    relay::Endpoint gateway;
    if (const auto route = environment.find_route()) {
      cause = Failure::kNoRouter;
      gateway = {route->gateway, environment.nat_pmp_port};
      if (environment.upnp_open) {
        const auto result = environment.upnp_open(*route, options, stop);
        cause = result.failure;
        if (result.failure == Failure::kNone && IsUnreachable(result.grant.address)) {
          environment.upnp_close(result.grant.port);
          cause = Failure::kSharedAddress;
        } else if (result.failure == Failure::kNone) {
          grant = result.grant;
          upnp = true;
        }
      }
      // Some routers refuse UPnP and grant the same mapping over NAT-PMP.
      if (!grant && cause != Failure::kSharedAddress) {
        const auto result =
            NatPmpOpen(gateway, options.port, options.port, environment.nat_pmp_lifetime, stop);
        if (result.outcome == NatPmpOutcome::kGranted && !IsUnreachable(result.grant.address)) {
          grant = result.grant;
        } else if (result.outcome == NatPmpOutcome::kGranted) {
          NatPmpClose(gateway, options.port);
          cause = Failure::kSharedAddress;
        } else {
          // The router may hold a mapping whose reply was lost.
          if (result.port_asked)
            NatPmpClose(gateway, options.port);
          if (result.outcome == NatPmpOutcome::kRefused && cause == Failure::kNoRouter)
            cause = Failure::kRefused;
        }
      }
    }
    // Close discards the status, so a stop reports nothing.
    if (!grant && stop.load())
      return;
    if (!grant) {
      Status failed{State::kFailed, cause, {}, 0, false};
      if (cause == Failure::kNoRouter && environment.discovery_disabled &&
          environment.discovery_disabled())
        failed.failure = Failure::kNoDiscovery;
      else if (cause == Failure::kNoRouter && environment.public_network)
        failed.public_network = environment.public_network();
      REXLOG_WARN("Router port for UDP {} not opened: {}", options.port,
                  FailureName(failed.failure));
      Publish(std::move(failed));
      return;
    }
    // The outside address stays out of the log.
    REXLOG_INFO("Router port open over {}: outside UDP {} to UDP {} on this computer",
                upnp ? "UPnP" : "NAT-PMP", grant->port, options.port);
    Publish({State::kOpen, Failure::kNone, AddressText(grant->address), grant->port, false});

    if (upnp) {
      // A UPnP mapping does not expire.
      std::unique_lock lock(mutex);
      wake.wait(lock, [this] { return stop.load(); });
      lock.unlock();
      environment.upnp_close(grant->port);
    } else {
      // Renewed at half of what the router granted, which can be less than asked.
      auto expires = std::chrono::steady_clock::now() + std::chrono::seconds(grant->lifetime);
      uint32_t wait = std::max<uint32_t>(1, grant->lifetime / 2);
      while (!StopWithin(wait)) {
        const auto asked = std::chrono::steady_clock::now();
        const auto renewed =
            NatPmpOpen(gateway, options.port, grant->port, environment.nat_pmp_lifetime, stop);
        if (renewed.outcome == NatPmpOutcome::kGranted) {
          grant = renewed.grant;
          expires = asked + std::chrono::seconds(grant->lifetime);
          wait = std::max<uint32_t>(1, grant->lifetime / 2);
          Publish({State::kOpen, Failure::kNone, AddressText(grant->address), grant->port, false});
          continue;
        }
        if (stop.load())
          break;
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(
                              expires - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
          REXLOG_WARN("Router port for UDP {} was lost: the router stopped renewing it",
                      options.port);
          Publish({State::kFailed, Failure::kRefused, {}, 0, false});
          // A renewal whose reply was lost may have kept the mapping alive.
          NatPmpClose(gateway, options.port);
          return;
        }
        // The mapping still has time left; ask again soon, and by its end.
        wait = static_cast<uint32_t>(std::min<int64_t>(
            left, std::min<uint32_t>(60, std::max<uint32_t>(1, grant->lifetime / 8))));
      }
      NatPmpClose(gateway, options.port);
    }
    REXLOG_INFO("Router port for UDP {} closed", options.port);
  }

  ~Mapper() {
    {
      std::lock_guard lock(mutex);
      stop.store(true);
    }
    wake.notify_all();
    if (worker.joinable())
      worker.join();
  }
};

std::mutex mutex;
std::unique_ptr<Mapper> mapper;  // guarded by mutex
bool retiring = false;           // guarded by mutex
std::condition_variable retired;

// Ends the mapper with the lock released: its thread can wait seconds for the
// router, and status readers must not wait with it. Open fails meanwhile, so
// no new mapping starts that this removal would take away.
void Retire(std::unique_lock<std::mutex>& lock) {
  auto ending = std::move(mapper);
  retiring = true;
  lock.unlock();
  ending.reset();
  lock.lock();
  retiring = false;
  retired.notify_all();
}
}  // namespace

bool IsUnreachable(uint32_t address) {
  const auto octets = Octets(address);
  return octets[0] == 0 || octets[0] == 10 || octets[0] == 127 ||
         (octets[0] == 100 && (octets[1] & 0xC0) == 64) ||
         (octets[0] == 172 && (octets[1] & 0xF0) == 16) || (octets[0] == 192 && octets[1] == 168) ||
         (octets[0] == 169 && octets[1] == 254);
}

NatPmpResult NatPmpOpen(relay::Endpoint gateway, uint16_t port, uint16_t outside_port,
                        uint32_t lifetime, const std::atomic<bool>& stop) {
  NatPmpResult result;
  // Opcode 0 asks for the outside address.
  const auto address = Ask(gateway, {0, 0}, 12, &stop);
  result.outcome = address.outcome;
  if (address.outcome != NatPmpOutcome::kGranted)
    return result;
  const auto mapped = AskForPort(gateway, port, outside_port, lifetime, &stop, &result.port_asked);
  result.outcome = mapped.outcome;
  if (mapped.outcome != NatPmpOutcome::kGranted)
    return result;
  std::memcpy(&result.grant.address, address.reply.data() + 8, 4);
  result.grant.port = static_cast<uint16_t>((mapped.reply[10] << 8) | mapped.reply[11]);
  result.grant.lifetime = (uint32_t{mapped.reply[12]} << 24) | (uint32_t{mapped.reply[13]} << 16) |
                          (uint32_t{mapped.reply[14]} << 8) | uint32_t{mapped.reply[15]};
  if (result.grant.port == 0 || result.grant.lifetime == 0)
    result.outcome = NatPmpOutcome::kRefused;
  return result;
}

void NatPmpClose(relay::Endpoint gateway, uint16_t port) {
  // A lifetime of 0 with outside port 0 removes the mapping.
  AskForPort(gateway, port, 0, 0, nullptr);
}

VoidResult OpenWith(Environment environment, const Options& options) {
  std::unique_lock lock(mutex);
  if (options.port == 0)
    return Err(ErrorCategory::Config, "A router port needs a UDP port");
  if (retiring)
    return Err(ErrorCategory::Runtime, "A router port is closing");
  if (mapper) {
    {
      std::lock_guard status_lock(mapper->mutex);
      if (mapper->status.state != State::kFailed)
        return Err(ErrorCategory::Runtime, "A router port is already open or opening");
    }
    // A failed attempt has nothing open; its thread has ended or is ending.
    Retire(lock);
  }
  auto starting = std::make_unique<Mapper>();
  starting->environment = std::move(environment);
  starting->options = options;
  starting->status.state = State::kOpening;
  try {
    starting->worker = std::thread([state = starting.get()] { state->Run(); });
  } catch (const std::system_error&) {
    return Err(ErrorCategory::Runtime, "The router port thread could not start");
  }
  mapper = std::move(starting);
  return Ok();
}
}  // namespace internal

VoidResult Open(const Options& options) {
  return internal::OpenWith(internal::SystemEnvironment(), options);
}

void Close() {
  std::unique_lock lock(internal::mutex);
  // Another thread's Close may still be waiting for the router.
  internal::retired.wait(lock, [] { return !internal::retiring; });
  if (internal::mapper)
    internal::Retire(lock);
}

Status Current() {
  std::lock_guard lock(internal::mutex);
  if (!internal::mapper)
    return {};
  std::lock_guard status_lock(internal::mapper->mutex);
  return internal::mapper->status;
}

}  // namespace rex::port_mapping
