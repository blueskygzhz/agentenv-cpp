// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/exec.rs — `aenv exec <id> -- <cmd...>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

// Rust: struct Args { sandbox_id: String, command: Vec<String> }
int Exec(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::fprintf(stderr, "aenv exec: usage: exec <sandbox_id> -- <cmd...>\n");
        return 2;
    }
    const std::string& sandbox_id = args[0];
    // Rust: opens a gRPC Start stream and drains stdout/stderr, returns exit code.
    std::fprintf(stderr, "aenv exec: would run in sandbox '%s':", sandbox_id.c_str());
    for (size_t i = 1; i < args.size(); ++i) {
        std::fprintf(stderr, " %s", args[i].c_str());
    }
    std::fprintf(stderr, "\n");
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
