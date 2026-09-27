// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/resume.rs — `aenv resume <id>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Resume(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "aenv resume: missing <sandbox_id>\n");
        return 2;
    }
    std::fprintf(stderr, "aenv resume: would resume sandbox '%s'\n", args[0].c_str());
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
