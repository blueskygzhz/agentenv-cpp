// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/pause.rs — `aenv pause <id>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Pause(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "aenv pause: missing <sandbox_id>\n");
        return 2;
    }
    std::fprintf(stderr, "aenv pause: would pause sandbox '%s'\n", args[0].c_str());
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
