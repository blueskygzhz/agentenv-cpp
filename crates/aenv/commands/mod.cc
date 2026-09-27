// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/commands/mod.rs — shared command surface + dispatch.
#include "agentenv/aenv/commands.h"
#include "agentenv/aenv/client.h"
#include "agentenv/core/identity.h"

namespace agentenv {
namespace aenv {
namespace commands {

std::string ResolveTemplate(const Context& /*ctx*/, const std::string& arg) {
    // Rust: if it parses as a UUID, pass through; otherwise resolve the alias.
    core::Uuid u;
    if (core::Uuid::Parse(arg, &u)) {
        return arg;
    }
    // TODO: call client.resolve_alias(arg). Skeleton returns the arg unchanged.
    return arg;
}

int Dispatch(const std::string& name, const Context& ctx,
             const std::vector<std::string>& args) {
    if (name == "auth")     return Auth(ctx, args);
    if (name == "build")    return Build(ctx, args);
    if (name == "connect")  return Connect(ctx, args);
    if (name == "delete")   return Delete_(ctx, args);
    if (name == "download") return Download(ctx, args);
    if (name == "exec")     return Exec(ctx, args);
    if (name == "list" || name == "ls") return List(ctx, args);
    if (name == "pause")    return Pause(ctx, args);
    if (name == "pull")     return Pull(ctx, args);
    if (name == "resume")   return Resume(ctx, args);
    if (name == "snapshot") return Snapshot(ctx, args);
    if (name == "start")    return Start(ctx, args);
    if (name == "template") return Template(ctx, args);
    if (name == "timeout")  return Timeout(ctx, args);
    if (name == "upload")   return Upload(ctx, args);
    return -1;  // unknown
}

}  // namespace commands
}  // namespace aenv
}  // namespace agentenv
