// SPDX-License-Identifier: MIT
// Rust: src/api/impls/image_build.rs (+ image_build/*.rs) — the BuildKit-backed
// image build flow.
//
// The flow itself is a long-lived axum/tonic tunnel plus a BuildKit worker in a
// sandbox; that half needs the transport stack. What is portable is the part
// that decides things:
//
//  * `DockerfileReadyCommand` translates a Dockerfile HEALTHCHECK into a ready
//    command. Getting this wrong is not cosmetic: a mis-rendered healthcheck
//    becomes the template's readiness gate, so it would either never pass
//    (build times out) or pass immediately (sandbox handed out before it
//    works).
//
//  * The build-session state machine, whose transitions are all one-way. A
//    build that was cancelled must never be observed as ready afterwards,
//    which is what keeps a client from tunnelling into a torn-down sandbox.
#ifndef AGENTENV_API_IMAGE_BUILD_H_
#define AGENTENV_API_IMAGE_BUILD_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace api {

/// Rust `transport::MAX_TUNNEL_CONNECTIONS` — the ceiling on concurrent
/// tunnel readers for one build session.
const uint32_t kMaxTunnelConnections = 8;

/// Rust `BuildJournal` — what a restart needs to resume or clean up a build.
struct BuildJournal {
    /// The cache (seed) image this build writes into.
    std::string cache;
    /// The parent build, when this one was seeded from another.
    core::Optional<std::string> parent;

    /// Rust `BuildJournal::persist` key — `build/{id}`.
    static std::string JournalKey(const std::string& build_id);

    core::Json ToJson() const;
    static core::Expected<BuildJournal, std::string> FromJson(const core::Json& json);
};

/// Rust `cache::seed_name`.
std::string BuildSeedName(const std::string& build_id);

/// Rust `enum SessionState`.
struct BuildSessionState {
    enum class Kind {
        Starting,
        Ready,
        Publishing,
        Cancelled,
        Finished,
    };

    Kind kind = Kind::Starting;
    /// Live when kind == Ready: the address the tunnel points at.
    std::string address;
    /// Live when kind == Finished: absent means the build succeeded.
    core::Optional<snapshot::TemplateBuildErrorReason> failure;

    static BuildSessionState MakeStarting();
    static BuildSessionState MakeReady(const std::string& address);
    static BuildSessionState MakePublishing();
    static BuildSessionState MakeCancelled();
    static BuildSessionState MakeFinished(
        const core::Optional<snapshot::TemplateBuildErrorReason>& failure);
};

/// Rust `BuildSession` — the observable state of one in-flight build.
///
/// Every transition is guarded, and each returns whether it actually applied.
/// Rust expresses this with `watch::Sender::send_if_modified`; the guards are
/// the same here.
class BuildSession {
 public:
    BuildSession() : state_(BuildSessionState::MakeStarting()) {}

    const BuildSessionState& state() const { return state_; }

    /// Rust `BuildSession::ready` — only a starting build can become ready.
    /// A build that was already cancelled must never be observed as ready, or
    /// a client would tunnel into a sandbox that is being torn down.
    bool Ready(const std::string& address);

    /// Only a ready build starts publishing: publishing from `Starting` would
    /// mean committing a build whose sandbox never came up.
    bool Publishing();

    /// Rust's cancel path. A finished build is terminal — cancelling it would
    /// discard the recorded outcome.
    bool Cancel();

    /// Terminal. Records the failure reason (absent on success).
    bool Finish(const core::Optional<snapshot::TemplateBuildErrorReason>& failure);

    /// Rust `is_publishing`.
    bool IsPublishing() const;
    /// Whether the session reached a terminal state.
    bool IsTerminal() const;

 private:
    BuildSessionState state_;
};

/// Rust `dockerfile_ready_command`.
///
/// Renders a Dockerfile `HEALTHCHECK` into a single shell-quoted command
/// string, or nothing when the image declares no usable healthcheck.
///
/// Recognised forms, matching Docker's own:
///   * `["NONE"]`            -> no ready command (healthcheck disabled)
///   * `["CMD-SHELL", "..."]` -> run through the image's SHELL
///   * `["CMD", arg...]`      -> exec form, used verbatim
/// Anything else is an error rather than a silent skip: an unrecognised
/// healthcheck would otherwise become "always ready".
core::Expected<core::Optional<std::string>, std::string> DockerfileReadyCommand(
    const core::Optional<core::Json>& image_config);

/// Rust `build_startup_commands`.
///
/// The request wins over the image: an explicit `startCmd`/`readyCmd` is the
/// caller's decision. Only when the request is silent does the image's own
/// ENTRYPOINT/CMD (for start) or HEALTHCHECK (for ready) apply.
core::Expected<core::Unit, std::string> BuildStartupCommands(
    const core::Optional<std::string>& request_start_cmd,
    const core::Optional<std::string>& request_ready_cmd,
    const snapshot::CommandContext& context, const core::Optional<core::Json>& image_config,
    core::Optional<std::string>* start_cmd, core::Optional<std::string>* ready_cmd);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_IMAGE_BUILD_H_
