// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/delete.rs — `aenv delete <id>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Delete_(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "aenv delete: missing <sandbox_id>\n");
        return 2;
    }
    std::fprintf(stderr, "aenv delete: would delete sandbox '%s'\n", args[0].c_str());
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
