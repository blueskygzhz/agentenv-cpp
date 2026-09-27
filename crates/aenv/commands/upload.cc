// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/upload.rs — `aenv upload <id> <local> <remote>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Upload(const Context& /*ctx*/, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::fprintf(stderr, "aenv upload: usage: upload <id> <local> <remote>\n");
        return 2;
    }
    std::fprintf(stderr, "aenv upload: would push '%s' -> '%s:%s'\n",
                 args[1].c_str(), args[0].c_str(), args[2].c_str());
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
