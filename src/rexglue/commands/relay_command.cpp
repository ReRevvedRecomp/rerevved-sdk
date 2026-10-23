#include "relay_command.h"
#include "relay_status.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>

#include <CLI/CLI.hpp>
#include <rex/logging.h>

#include "core/relay_transport.h"

namespace rexglue::cli {
namespace {
std::atomic<bool> stopped{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void Stop(int) {
  stopped.store(true, std::memory_order_relaxed);
}

struct Options {
  std::string bind = "0.0.0.0";
  uint16_t port = 30100;
  std::string secret_file;
  rex::relay::Config config;
};

rex::VoidResult Run(const Options& options) {
  constexpr size_t kMaxSecretSize = 4096;
  std::error_code error;
  if (!std::filesystem::is_regular_file(options.secret_file, error))
    return rex::Err(rex::ErrorCategory::IO, "Relay secret must be a regular file");
  const auto size = std::filesystem::file_size(options.secret_file, error);
  if (error || size > kMaxSecretSize)
    return rex::Err(rex::ErrorCategory::Config, "Relay secret file exceeds 4 KiB or is unreadable");
  std::ifstream file(options.secret_file, std::ios::binary);
  if (!file)
    return rex::Err(rex::ErrorCategory::IO, "Cannot read relay secret file");
  std::string secret(kMaxSecretSize + 1, '\0');
  file.read(secret.data(), static_cast<std::streamsize>(secret.size()));
  secret.resize(static_cast<size_t>(file.gcount()));
  if (file.bad())
    return rex::Err(rex::ErrorCategory::IO, "Cannot read relay secret file");
  if (secret.size() > kMaxSecretSize)
    return rex::Err(rex::ErrorCategory::Config, "Relay secret file exceeds 4 KiB");
  const auto first = secret.find_first_not_of(" \t\r\n\f\v");
  if (first == std::string::npos)
    return rex::Err(rex::ErrorCategory::Config, "Relay secret is empty after trimming");
  secret = secret.substr(first, secret.find_last_not_of(" \t\r\n\f\v") - first + 1);
  auto seed = rex::relay::RandomSeed();
  if (!seed)
    return std::unexpected(seed.error());
  auto socket = rex::relay::UdpSocket::Open(options.bind, options.port);
  if (!socket)
    return std::unexpected(socket.error());
  rex::relay::Core core(options.config, std::move(secret), *seed);
  rex::relay::TransportCounters transport;
  stopped.store(false, std::memory_order_relaxed);
  const auto previous = std::signal(SIGINT, Stop);
  if (previous == SIG_ERR)
    return rex::Err(rex::ErrorCategory::Platform, "Cannot register relay Ctrl+C handler");
  const auto start = std::chrono::steady_clock::now();
  auto milliseconds = [&] {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count());
  };
  REXLOG_INFO("Relay listening on {}:{} (up to {} rooms, {} members per room)", options.bind,
              options.port, options.config.max_rooms, options.config.max_members);
  RelayStatus status;
  uint64_t next_tick = 0;
  while (!stopped.load(std::memory_order_relaxed)) {
    auto now = milliseconds();
    const auto timeout =
        static_cast<uint32_t>(now >= next_tick ? 0 : std::min(uint64_t{100}, next_tick - now));
    rex::relay::Pump(**socket, core, now, transport, timeout);
    now = milliseconds();
    if (now >= next_tick) {
      next_tick = now + 100;
      const auto messages =
          status.Update(core.counters(), transport, (*socket)->connection_resets(), now);
      if (!messages.status.empty())
        REXLOG_INFO("{}", messages.status);
      if (!messages.warning.empty())
        REXLOG_WARN("{}", messages.warning);
      if (!messages.debug.empty())
        REXLOG_DEBUG("{}", messages.debug);
    }
  }
  std::signal(SIGINT, previous);
  REXLOG_INFO("{}",
              RelayStatus::Summary(core.counters(), transport, (*socket)->connection_resets()));
  return rex::Ok();
}
}  // namespace

void RegisterRelay(CLI::App& parent, DeferredAction& pending) {
  auto options = std::make_shared<Options>();
  auto* relay = parent.add_subcommand("relay", "Serve a private System Link UDP relay");
  relay->add_option("--bind", options->bind, "IPv4 bind address (default: 0.0.0.0)");
  relay->add_option("--port", options->port, "UDP port (default: 30100)")
      ->check(CLI::Range(1, 65535));
  relay->add_option("--secret-file", options->secret_file, "Shared-secret file")->required();
  relay->add_option("--max-rooms", options->config.max_rooms, "Room limit (default: 16)")
      ->check(CLI::Range(size_t{1}, rex::relay::kMaxRooms));
  relay->add_option("--max-members", options->config.max_members, "Members per room (default: 4)")
      ->check(CLI::Range(size_t{1}, rex::relay::kMaxMembers));
  relay->callback([options, &pending] { pending = [options] { return Run(*options); }; });
}
}  // namespace rexglue::cli
