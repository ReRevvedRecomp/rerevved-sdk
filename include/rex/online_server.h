/**
 * @file        rex/online_server.h
 * @brief       The System Link online server, run inside the host process
 *
 * Safe from any host thread.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <rex/result.h>

namespace rex::online_server {

inline constexpr std::string_view kRoom = "testers";

struct Options {
  std::string bind_address = "0.0.0.0";
  uint16_t port = 30100;  // 0 lets the system choose
  std::filesystem::path password_file;
  uint32_t max_members = 8;
};

struct Status {
  bool running = false;
  uint16_t port = 0;
  uint32_t players = 0;
  uint32_t games = 0;
};

// Creates `password_file` with a random password when it is missing. The
// code is a credential: keep it out of logs.
Result<std::string> MakeInvite(std::string_view address,
                               const std::filesystem::path& password_file);

// A failed start leaves nothing running.
VoidResult Start(const Options& options);
// Call it before the process exits.
void Stop();
Status Current();

}  // namespace rex::online_server
