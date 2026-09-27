// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/download.rs — `aenv download <id> <remote> <local>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Download(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::fprintf(stderr, "aenv download: usage: download <id> <remote> <local>\n");
        return 2;
    }
    std::fprintf(stderr, "aenv download: would fetch '%s:%s' -> '%s'\n",
                 args[0].c_str(), args[1].c_str(), args[2].c_str());
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
