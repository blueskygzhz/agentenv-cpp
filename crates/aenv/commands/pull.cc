// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/pull.rs — `aenv pull <image_ref>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Pull(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "aenv pull: missing <image_ref>\n");
        return 2;
    }
    std::fprintf(stderr, "aenv pull: would pull image '%s'\n", args[0].c_str());
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
