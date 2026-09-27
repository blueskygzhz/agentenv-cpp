// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/mod.rs + one module per subcommand.
// C++11: the shared command surface. Each subcommand is implemented in its own
// translation unit commands/<name>.cc, matching the Rust file layout 1:1.
#ifndef AGENTENV_AENV_COMMANDS_H_
#define AGENTENV_AENV_COMMANDS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace agentenv {
namespace aenv {
namespace commands {

/// Rust: commands::mod::DEFAULT_TIMEOUT_SECS.
static const uint32_t kDefaultTimeoutSecs = 300;

/// Shared CLI context (endpoint + auth) threaded into every command.
struct Context {
    std::string api_endpoint;   // e.g. "http://127.0.0.1:6767"
    std::string auth_token;
    bool        verbose = false;
};

/// Rust: commands::mod::CpuMemoryArgs — optional CPU/mem overrides.
struct CpuMemoryArgs {
    bool     has_cpu_count = false;   uint32_t cpu_count = 0;
    bool     has_memory_mb = false;   uint32_t memory_mb = 0;
    /// Rust `is_set()`.
    bool IsSet() const { return has_cpu_count || has_memory_mb; }
};

/// Rust: commands::mod::resolve_template — pass through a UUID or resolve an alias.
std::string ResolveTemplate(const Context& ctx, const std::string& arg);

// One entry point per subcommand. Return codes: 0 == ok, non-zero == error.
// Each is defined in commands/<name>.cc (see Rust commands/<name>.rs).
int Auth    (const Context& ctx, const std::vector<std::string>& args);
int Build   (const Context& ctx, const std::vector<std::string>& args);
int Connect (const Context& ctx, const std::vector<std::string>& args);
int Delete_ (const Context& ctx, const std::vector<std::string>& args);
int Download(const Context& ctx, const std::vector<std::string>& args);
int Exec    (const Context& ctx, const std::vector<std::string>& args);
int List    (const Context& ctx, const std::vector<std::string>& args);
int Pause   (const Context& ctx, const std::vector<std::string>& args);
int Pull    (const Context& ctx, const std::vector<std::string>& args);
int Resume  (const Context& ctx, const std::vector<std::string>& args);
int Snapshot(const Context& ctx, const std::vector<std::string>& args);
int Start   (const Context& ctx, const std::vector<std::string>& args);
int Template(const Context& ctx, const std::vector<std::string>& args);
int Timeout (const Context& ctx, const std::vector<std::string>& args);
int Upload  (const Context& ctx, const std::vector<std::string>& args);

/// Dispatch a subcommand name to its Run function. Returns -1 if unknown.
int Dispatch(const std::string& name, const Context& ctx,
             const std::vector<std::string>& args);

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
#endif  // AGENTENV_AENV_COMMANDS_H_
