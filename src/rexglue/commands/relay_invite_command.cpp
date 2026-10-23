#include "relay_invite_command.h"

#include <cstdio>
#include <CLI/CLI.hpp>
#include "core/relay_invite.h"

namespace rexglue::cli {
void RegisterRelayInvite(CLI::App& parent, DeferredAction& pending) {
  struct Options {
    std::string address;
    std::string secret_file;
    std::string room = "testers";
    bool create_secret = false;
  };
  auto options = std::make_shared<Options>();
  auto* command = parent.add_subcommand("relay-invite", "Create a private System Link invite code");
  command->add_option("--address", options->address, "Relay IPv4 or DNS host:port")->required();
  command->add_option("--secret-file", options->secret_file, "Shared-secret file")->required();
  command->add_option("--room", options->room, "Room name (default: testers)");
  command->add_flag("--create-secret", options->create_secret,
                    "Create a missing secret file; read an existing file unchanged");
  command->callback([options, &pending] {
    pending = [options]() -> rex::VoidResult {
      auto code = rex::relay::MakeInvite(options->address, options->secret_file, options->room,
                                         options->create_secret);
      if (!code)
        return std::unexpected(code.error());
      if (std::printf("%s\n", code->c_str()) < 0 || std::fflush(stdout))
        return rex::Err(rex::ErrorCategory::IO, "Cannot write invite code to stdout");
      return rex::Ok();
    };
  });
}
}  // namespace rexglue::cli
