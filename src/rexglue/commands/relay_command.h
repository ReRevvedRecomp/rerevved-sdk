#pragma once

#include "../cli_utils.h"

namespace CLI {
class App;
}

namespace rexglue::cli {
void RegisterRelay(CLI::App& parent, DeferredAction& pending);
}  // namespace rexglue::cli
