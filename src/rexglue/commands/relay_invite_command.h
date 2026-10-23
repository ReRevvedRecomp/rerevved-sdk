#pragma once

#include "../cli_utils.h"

namespace CLI {
class App;
}
namespace rexglue::cli {
void RegisterRelayInvite(CLI::App& parent, DeferredAction& pending);
}
