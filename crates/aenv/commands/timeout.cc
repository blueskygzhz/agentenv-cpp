// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/timeout.rs — `aenv timeout <id> <secs>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>
#include <cstdlib>

namespace agentenv {
namespace aenv {
namespace commands {

int Timeout(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::fprintf(stderr, "aenv timeout: usage: timeout <sandbox_id> <secs>\n");
        return 2;
    }
    long secs = std::strtol(args[1].c_str(), nullptr, 10);
    std::fprintf(stderr, "aenv timeout: would set sandbox '%s' timeout=%lds\n",
                 args[0].c_str(), secs);
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
