// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/auth.rs — `aenv auth login|logout|status`.
#include "agentenv/aenv/commands.h"
#include "agentenv/aenv/util.h"

#include <cstdio>

namespace agentenv {
namespace aenv {
namespace commands {

int Auth(const Context& /*ctx*/, const std::vector<std::string>& args) {
    const std::string sub = args.empty() ? std::string("status") : args[0];
    if (sub == "login") {
        // Rust: prompt/read token, then auth::save_token(profile, token).
        std::fprintf(stderr, "aenv auth login: would persist an auth token\n");
        return 0;
    }
    if (sub == "logout") {
        std::fprintf(stderr, "aenv auth logout: would clear the stored token\n");
        return 0;
    }
    // status
    auto tok = auth::LoadToken("default");
    std::fprintf(stderr, "aenv auth status: %s\n",
                 (tok.ok() && !tok.value().empty()) ? "logged in" : "not logged in");
    return 0;
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
