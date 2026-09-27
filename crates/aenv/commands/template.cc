// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/template.rs — `aenv template list|delete`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Template(const Context& /*ctx*/, const std::vector<std::string>& args) {
    const std::string sub = args.empty() ? std::string("list") : args[0];
    if (sub == "delete") {
        if (args.size() < 2) {
            std::fprintf(stderr, "aenv template delete: missing <template>\n");
            return 2;
        }
        std::fprintf(stderr, "aenv template delete: would delete '%s'\n", args[1].c_str());
        return 0;
    }
    // list
    std::printf("TEMPLATE ID    ALIAS    STATUS\n");
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
