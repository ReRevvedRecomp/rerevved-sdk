#include <rex/online_server.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <rex/logging.h>

#include "relay_invite.h"
#include "relay_transport.h"

namespace rex::online_server {
namespace {
struct Server {
  std::unique_ptr<relay::UdpSocket> socket;
  std::unique_ptr<relay::Core> core;
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> players{0};
  std::atomic<uint32_t> games{0};
  std::thread worker;

  // The core is single-threaded: only this loop touches it and the socket.
  void Serve() {
    relay::TransportCounters transport;
    const auto start = std::chrono::steady_clock::now();
    while (!stop.load(std::memory_order_relaxed)) {
      const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - start)
                                                 .count());
      // Sleeps on the socket for up to 100 ms, which also bounds how long Stop waits.
      relay::Pump(*socket, *core, now, transport, 100);
      const auto counters = core->counters();
      players.store(static_cast<uint32_t>(counters.peers), std::memory_order_relaxed);
      games.store(static_cast<uint32_t>(counters.rooms), std::memory_order_relaxed);
    }
  }
  ~Server() {
    stop.store(true, std::memory_order_relaxed);
    if (worker.joinable())
      worker.join();
  }
};

std::mutex mutex;
std::unique_ptr<Server> server;  // guarded by mutex
}  // namespace

Result<std::string> MakeInvite(std::string_view address,
                               const std::filesystem::path& password_file) {
  return relay::MakeInvite(address, password_file, kRoom, true);
}

VoidResult Start(const Options& options) {
  std::lock_guard lock(mutex);
  if (server)
    return Err(ErrorCategory::Runtime, "The online server is already running");
  auto password = relay::ReadPassword(options.password_file);
  if (!password)
    return std::unexpected(password.error());
  auto seed = relay::RandomSeed();
  if (!seed)
    return std::unexpected(seed.error());
  auto socket = relay::UdpSocket::Open(options.bind_address, options.port);
  if (!socket)
    return std::unexpected(socket.error());
  auto starting = std::make_unique<Server>();
  relay::Config config;
  config.max_members = options.max_members;
  try {
    starting->core = std::make_unique<relay::Core>(config, std::move(*password), *seed);
  } catch (const std::invalid_argument&) {
    return Err(ErrorCategory::Config, "Invalid online server options");
  }
  starting->socket = std::move(*socket);
  const auto port = starting->socket->endpoint().port;
  try {
    starting->worker = std::thread([state = starting.get()] { state->Serve(); });
  } catch (const std::system_error&) {
    return Err(ErrorCategory::Runtime, "The online server thread could not start");
  }
  server = std::move(starting);
  REXLOG_INFO("Online server listening on {}:{} (up to {} players)", options.bind_address, port,
              options.max_members);
  return Ok();
}

void Stop() {
  // Held until the thread has ended and the port is closed, so a Start on
  // another thread cannot run a second server meanwhile.
  std::lock_guard lock(mutex);
  if (!server)
    return;
  server.reset();
  REXLOG_INFO("Online server stopped");
}

Status Current() {
  std::lock_guard lock(mutex);
  if (!server)
    return {};
  return {true, server->socket->endpoint().port, server->players.load(std::memory_order_relaxed),
          server->games.load(std::memory_order_relaxed)};
}

}  // namespace rex::online_server
