// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/start.rs — `aenv start <template>`.
#include "agentenv/aenv/commands.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

// Rust: struct Args { template: String, cpu_memory: CpuMemoryArgs, timeout, ... }
int Start(const Context& ctx, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "aenv start: missing <template>\n");
        return 2;
    }
    std::string template_ref = ResolveTemplate(ctx, args[0]);
    // Rust: Client::from_env()?.create_sandbox(template, cpu, mem, timeout)?
    std::fprintf(stderr, "aenv start: would create sandbox from template '%s' (timeout=%us)\n",
                 template_ref.c_str(), kDefaultTimeoutSecs);
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
