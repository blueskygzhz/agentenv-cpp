// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/snapshot.rs — `aenv snapshot <id>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Snapshot(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "aenv snapshot: missing <sandbox_id>\n");
        return 2;
    }
    std::fprintf(stderr, "aenv snapshot: would snapshot sandbox '%s'\n", args[0].c_str());
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
