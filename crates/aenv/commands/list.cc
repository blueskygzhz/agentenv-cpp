// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/list.rs — `aenv list` / `aenv ls`.
#include "agentenv/aenv/commands.h"
#include "agentenv/aenv/client.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

// Rust: struct Args { output: Option<Format> }
int List(const Context& /*ctx*/, const std::vector<std::string>& /*args*/) {
    // Rust: Client::from_env()?.list_sandboxes()? then render a table.
    // Skeleton: no HTTP client compiled in, so print an empty table header.
    std::printf("SANDBOX ID    TEMPLATE    STATE    CPU    MEM (MiB)    DISK (MiB)    STARTED\n");
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
