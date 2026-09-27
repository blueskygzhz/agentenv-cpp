// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/connect.rs — `aenv connect <id>` (interactive PTY).
#include "agentenv/aenv/commands.h"
#include "agentenv/aenv/util.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Connect(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "aenv connect: missing <sandbox_id>\n");
        return 2;
    }
    // Rust: opens a PTY-backed interactive Start stream to the guest shell.
    std::fprintf(stderr, "aenv connect: would attach an interactive shell to '%s'\n",
                 args[0].c_str());
    (void)pty::AttachTty(0);
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
