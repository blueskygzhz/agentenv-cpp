// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/build.rs — `aenv build <context>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

// Rust: struct Args { context: PathBuf, tag: Option<String>, cpu_memory, ... }
int Build(const Context& /*ctx*/, const std::vector<std::string>& args) {
    const std::string context_dir = args.empty() ? std::string(".") : args[0];
    // Rust: builds an OCI/overlaybd template from the build context and registers it.
    std::fprintf(stderr, "aenv build: would build a template from context '%s'\n",
                 context_dir.c_str());
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
