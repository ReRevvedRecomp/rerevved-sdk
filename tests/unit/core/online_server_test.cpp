#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <rex/online_server.h>

#include "core/relay_invite.h"
#include "kernel/xam/relay_system_link.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace std::chrono_literals;
namespace online_server = rex::online_server;
namespace relay_link = rex::kernel::xam::relay_link;

namespace {
struct PasswordFolder {
  std::filesystem::path folder;
  std::filesystem::path file;
  PasswordFolder() {
    static int next = 0;
    folder = std::filesystem::temp_directory_path() /
             ("rex-online-server-test-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
              std::to_string(next++));
    std::filesystem::create_directories(folder);
    file = folder / "online-server-password.txt";
  }
  ~PasswordFolder() {
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    std::filesystem::remove(folder, ignored);
  }
};
// Loopback only, so the test never raises a firewall prompt.
online_server::Options LoopbackOptions(const std::filesystem::path& password_file) {
  online_server::Options options;
  options.bind_address = "127.0.0.1";
  options.port = 0;
  options.password_file = password_file;
  return options;
}
uint32_t WaitForPlayers(uint32_t expected) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (online_server::Current().players != expected &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  return online_server::Current().players;
}
}  // namespace

TEST_CASE("the in-process online server admits a player, stops and starts again",
          "[online_server][relay_client][network]") {
  PasswordFolder password;
  {
    std::ofstream out(password.file, std::ios::binary);
    out << "  a-password-of-twenty\r\n";
  }
  CHECK_FALSE(online_server::Current().running);
  online_server::Stop();  // Stopping with nothing running does nothing.

  for (int round = 0; round < 2; ++round) {
    const auto started = online_server::Start(LoopbackOptions(password.file));
    if (!started)
      SKIP("Loopback UDP socket unavailable");
    const auto status = online_server::Current();
    REQUIRE(status.running);
    REQUIRE(status.port != 0);
    CHECK(status.players == 0);
    // One server per process: a second start fails and leaves the first running.
    CHECK_FALSE(online_server::Start(LoopbackOptions(password.file)));
    CHECK(online_server::Current().port == status.port);

    const rex::relay::Endpoint endpoint{htonl(INADDR_LOOPBACK), status.port};
    relay_link::Client wrong;
    CHECK_FALSE(wrong.Start(endpoint, "room", "not the password", {}));
    relay_link::Client player;
    REQUIRE(player.Start(endpoint, "room", "a-password-of-twenty", {}));
    CHECK(WaitForPlayers(1) == 1);
    CHECK(online_server::Current().games == 1);

    online_server::Stop();
    CHECK_FALSE(online_server::Current().running);
    CHECK(online_server::Current().port == 0);
    player.Stop();
  }
}

TEST_CASE("an invite code made in process joins the in-process server",
          "[online_server][relay_invite][relay_client][network]") {
  PasswordFolder password;
  REQUIRE_FALSE(std::filesystem::exists(password.file));
  const auto first = online_server::MakeInvite("203.0.113.7:30100", password.file);
  REQUIRE(first);
  REQUIRE(std::filesystem::exists(password.file));

  const auto decoded = rex::relay::DecodeInvite(*first);
  REQUIRE(decoded);
  CHECK(decoded->address.host == "203.0.113.7");
  CHECK(decoded->address.port == 30100);
  CHECK(decoded->room == "testers");
  CHECK(decoded->secret_size == 32);

  // The file is never changed: a second code carries the same password.
  const auto second = online_server::MakeInvite("127.0.0.1:30101", password.file);
  REQUIRE(second);
  const auto local = rex::relay::DecodeInvite(*second);
  REQUIRE(local);
  CHECK(local->address.port == 30101);
  CHECK(local->SecretView() == decoded->SecretView());

  const auto started = online_server::Start(LoopbackOptions(password.file));
  if (!started)
    SKIP("Loopback UDP socket unavailable");
  const rex::relay::Endpoint endpoint{htonl(INADDR_LOOPBACK), online_server::Current().port};
  relay_link::Client player;
  CHECK(player.Start(endpoint, decoded->room, relay_link::Secret(decoded->SecretView()), {}));
  CHECK(WaitForPlayers(1) == 1);
  online_server::Stop();
  player.Stop();
}

TEST_CASE("in-process invites reject what they cannot encode", "[online_server][relay_invite]") {
  PasswordFolder password;
  CHECK_FALSE(online_server::MakeInvite("no port", password.file));
  CHECK_FALSE(std::filesystem::exists(password.file));
  {
    std::ofstream out(password.file, std::ios::binary);
    out << "short\n";
  }
  // An existing password that is too short for a code is reported, not replaced.
  CHECK_FALSE(online_server::MakeInvite("203.0.113.7:30100", password.file));
  CHECK(std::filesystem::file_size(password.file) == 6);
}

TEST_CASE("the in-process online server rejects options it cannot serve", "[online_server]") {
  PasswordFolder password;
  CHECK_FALSE(online_server::Start(LoopbackOptions(password.file)));
  {
    std::ofstream out(password.file, std::ios::binary);
    out << " \r\n";
  }
  CHECK_FALSE(online_server::Start(LoopbackOptions(password.file)));
  {
    std::ofstream out(password.file, std::ios::binary | std::ios::trunc);
    out << "a-password-of-twenty\n";
  }
  auto options = LoopbackOptions(password.file);
  options.max_members = 0;
  CHECK_FALSE(online_server::Start(options));
  options = LoopbackOptions(password.file);
  options.bind_address = "not an address";
  CHECK_FALSE(online_server::Start(options));
  // A failed start leaves nothing running.
  CHECK_FALSE(online_server::Current().running);
}
