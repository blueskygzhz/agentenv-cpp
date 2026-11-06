// SPDX-License-Identifier: MIT
// Rust: src/sandbox/envd.rs + src/sandbox/envd/user.rs — the guest-agent
// (envd) handle the orchestrator reaches a running sandbox through.
//
// What this file deliberately does *not* contain any more: an `EnvdClient`
// interface with `Exec` / `Signal` / `WaitReady`. That type had no Rust
// counterpart and no caller. Upstream's shape is a single concrete
// `EnvdInstance` (HTTP control plane + two gRPC channels) plus the narrow
// `EnvdApi` trait, and keeping an invented third surface alongside it is how a
// port grows an API the server it is standing in for cannot speak.
#ifndef AGENTENV_SANDBOX_ENVD_H_
#define AGENTENV_SANDBOX_ENVD_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/sandbox/metrics.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/access.h"
#include "agentenv/sandbox/process.h"
#include "agentenv/sandbox/types.h"

namespace agentenv {
namespace sandbox {

/// Rust `envd::user::BUSYBOX` — the tools-drive BusyBox the passwd append runs
/// under. The guest userland may be empty (scratch / distroless), so the
/// script cannot assume a shell or coreutils of its own.
extern const char* const kEnvdBusyboxPath;

/// Rust `envd::user::ADD_PASSWD_ENTRY` — `include_str!("add_passwd_entry.sh")`.
///
/// Carried as a string constant for the same reason upstream embeds it in the
/// binary rather than shipping it on the tools drive: it must also apply to
/// sandboxes pinned to an older, immutable tools release.
extern const char* const kAddPasswdEntryScript;

/// Rust `envd::user::ResolvedUser`.
struct ResolvedUser {
    /// The account name to hand envd as its default user.
    std::string name;
    /// Rust `Option<String>` — the `/etc/passwd` record that must be appended
    /// to the guest before `name` names anything. Absent when the account is
    /// already present and usable, which is what makes a snapshot restore
    /// reuse the identity instead of appending a second copy.
    core::Optional<std::string> passwd_entry;

    bool operator==(const ResolvedUser& o) const {
        return name == o.name && passwd_entry.has_value() == o.passwd_entry.has_value() &&
               (!passwd_entry.has_value() || *passwd_entry == *o.passwd_entry);
    }
    bool operator!=(const ResolvedUser& o) const { return !(*this == o); }
};

/// Rust `envd::user::needs_resolution` — true when the Dockerfile `USER` value
/// cannot be handed to envd as-is.
///
/// envd requires a *name*; Docker also permits a bare UID or a `user:group`
/// pair. Those two forms are what this predicate selects, and selecting them
/// is what triggers the extra guest round trip in `EnvdInstance::Init`.
bool EnvdUserNeedsResolution(const std::string& user);

/// Rust `envd::user::resolve_user` — the pure half of default-user resolution.
///
/// Maps a Dockerfile `USER` value onto an account name that exists in the
/// guest, given the guest's `/etc/passwd` and `/etc/group` verbatim. Pure and
/// byte-oriented, which is the whole point: unrelated names, GECOS comments
/// and home paths in those files need not be UTF-8, and refusing a file
/// because some *other* account has a non-UTF-8 comment would reject images
/// that work fine under Docker. Only the selected account's name and home are
/// converted, and those two conversions are errors when they fail.
///
/// The rules that carry real consequences:
///
///   * **A malformed record is an error, not a skipped line.** A seven-field
///     shape with a non-empty name is required, and both ids must parse. A
///     skipped line would silently hide an existing account, and the resolver
///     would then append a *second* record for an identity the guest already
///     has.
///   * **Line numbers count blank and comment lines**, because the number is
///     reported to an operator who will open the file.
///   * **An explicit group never reuses an existing account.** `nonroot:staff`
///     must not rewrite `nonroot`'s primary group, so it resolves to a
///     separate generated identity even though the account exists.
///   * **A generated name is reused only on an exact match** of every field
///     (`x`, uid, gid, empty GECOS, the same home, `/bin/sh`). Reusing a name
///     whose record differs would hand envd an account with the wrong home or
///     group; the suffix search moves past it instead.
///   * **The generated home is inherited from the matched account**, falling
///     back to `/`, so an explicit `uid:gid` keeps the image's home directory.
core::Expected<ResolvedUser, std::string> ResolveEnvdUser(const std::string& user,
                                                          const std::string& passwd,
                                                          const std::string& groups);

/// Rust `envd::LOG_DIRECTORY` / `ENVD_STDERR_PATH`.
extern const char* const kEnvdLogDirectory;
extern const char* const kEnvdStderrPath;

/// Rust `envd::is_log_filename`.
///
/// The guest names log files, and the name is pasted into a path this server
/// then fetches — so this is a path-traversal boundary, not a tidy-up filter.
/// Only `current` and daemontools' exact `@<24 hex>.s` rotation form pass.
bool IsEnvdLogFilename(const std::string& name);

/// Rust `InitPostRequest` as `post_init` builds it.
///
/// The envd wire model carries three further optional fields —
/// `volumeMounts`, `hyperloopIP`, `caBundle` — which this call path leaves at
/// `Default::default()`, i.e. `None`. They are therefore *absent* from the
/// body rather than present and null, which the field set here guarantees
/// structurally.
struct InitPostRequest {
    /// `envVars`. Unset is distinct from an empty map: unset leaves the
    /// guest's image environment alone, where `{}` is a request to set none.
    core::Optional<std::map<std::string, std::string> > env_vars;
    /// `accessToken`. Sent in the body *as well as* the `X-Access-Token`
    /// header: the header authenticates this call, the body is what envd
    /// stores and checks on every later one.
    core::Optional<std::string> access_token;
    /// `timestamp`, RFC 3339. The guest has no clock of its own across a
    /// snapshot restore, so this is how it learns the real time.
    core::Optional<std::string> timestamp;
    /// `defaultUser`.
    core::Optional<std::string> default_user;
    /// `defaultWorkdir`.
    core::Optional<std::string> default_workdir;

    /// serde's `skip_serializing_if = "Option::is_none"` for every field.
    ///
    /// This is the load-bearing rule of the whole body: envd reads an absent
    /// `defaultUser` as "keep what you have" and a null one as a value, so
    /// serialising `None` as `null` would clear the account a previous `init`
    /// established — which on the resolution path is the account this very
    /// sequence just created.
    core::Json ToJson() const;
};

/// Rust `trait EnvdApi` — the calls the orchestrator makes into a running
/// guest through envd.
///
/// Separate from `EnvdInstance` for the reason upstream gives: the backend
/// hands this out under the runtime lock and callers make the call *after*
/// releasing it, so what crosses the lock boundary has to be an interface with
/// no backend internals attached. It is also what lets the metrics sweep be
/// driven without a VMM.
///
/// Upstream's `logs()` is deliberately absent. It dispatches over envd's
/// *filesystem gRPC* service — `list_dir` on the guest log directory, `stat`
/// on envd's stderr, then a ranged read per file — and there is no gRPC stack
/// in this port (`EnvdInstance::FilesystemClient` reports exactly that).
/// Declaring it here would put a method on the interface that no
/// implementation can honour, and the orchestrator's log handler would then
/// fail at the transport rather than at a stated gap. `sandbox/logs.h` holds
/// the decoding and paging half, which is the part that does not need the
/// transport.
class EnvdApi {
 public:
    virtual ~EnvdApi() {}

    /// Rust `EnvdApi::metrics` — `ensure_live()` then `GET {base}/metrics`.
    ///
    /// The liveness gate comes first and is not an optimisation: a handle
    /// taken before the sandbox was torn down must not reach the IP address a
    /// *different* VM has since been given.
    virtual core::Expected<SandboxMetric, std::string> Metrics() const = 0;
};

/// Rust struct `EnvdInstance` — bootstrap handle to a guest envd daemon.
class EnvdInstance : public EnvdApi {
 public:
    /// A single health probe attempt bounded by `probe_timeout_ms`.
    /// Returns true when envd is healthy. Should itself respect the timeout.
    typedef std::function<bool(int32_t probe_timeout_ms)> HealthProbe;

    /// Rust `Executor::new(self.clone()).with_root_user().run_command_with_opts`.
    ///
    /// Injected rather than implemented: the real executor dispatches over
    /// envd's process gRPC channel, which this port does not have. It is a
    /// seam and not a stub — see `SetRootCommandRunner`.
    typedef std::function<core::Expected<ProcessOutput, std::string>(
        const std::string& cmd, const std::vector<std::string>& args, const ProcessOpts& opts)>
        RootCommandRunner;

    explicit EnvdInstance(std::string base_path);
    /// Rust `EnvdInstance::new(base_path, access_token)`.
    EnvdInstance(std::string base_path, const core::Optional<EnvdAccessToken>& access_token);

    const std::string& BasePath() const { return base_path_; }
    const std::string& GrpcAddress() const { return grpc_address_; }
    const core::Optional<EnvdAccessToken>& AccessToken() const { return access_token_; }

    /// Rust `EnvdInstance::invalidate`.
    ///
    /// Marks the runtime generation dead. The flag is *shared between copies*
    /// (Rust `Arc<AtomicBool>`), which is the entire point: the orchestrator
    /// hands copies of this handle out to callers that then make their call
    /// after releasing the runtime lock, and a copy taken before the sandbox
    /// was torn down must not keep working. A per-copy flag would let a stale
    /// handle reach the IP address a *different* VM has since been given.
    void Invalidate();

    /// Rust `EnvdInstance::ensure_live`.
    core::Expected<core::Unit, std::string> EnsureLive() const;

    /// Rust `EnvdInstance::process_client` / `filesystem_client`.
    ///
    /// Both are `ensure_live` followed by a gRPC `connect`. The liveness gate
    /// is ported and the connect is not: envd's process and filesystem
    /// services are tonic-generated and there is no gRPC stack here.
    ///
    /// The ordering is why these exist at all rather than being omitted. An
    /// invalidated runtime must be refused *before* any connect is attempted,
    /// and the two failures are not interchangeable to a caller: "no longer
    /// active" is a sandbox that is gone, which is terminal, while a missing
    /// transport is a property of this build. Collapsing them would make a
    /// torn-down sandbox look like a deployment problem.
    core::Expected<core::Unit, std::string> ProcessClient() const;
    core::Expected<core::Unit, std::string> FilesystemClient() const;

    /// Overrides the root-command runner used by the passwd append.
    ///
    /// Absent, `ResolveDefaultUser` *fails* when a record has to be appended
    /// rather than returning the resolved name. Returning the name would be
    /// the harmful option: envd rejects a default user that names no account,
    /// so every later SDK call against that sandbox would fail with an error
    /// pointing at the file operation rather than at the missing identity.
    void SetRootCommandRunner(const RootCommandRunner& runner);

    /// Rust `default_api::health_get` under `tokio::time::timeout` --
    /// `GET {base_path}/health` on a fresh connection (Rust's bootstrap client
    /// keeps no idle connections: the address may belong to the previous VM
    /// that held this slot). Healthy unless the transport fails or the status
    /// is a client or server error.
    ///
    /// No access token: `health_get` is the one generated operation that sends
    /// no `X-Access-Token`, so readiness can be observed before `init` has
    /// established one.
    HealthProbe HttpHealthProbe() const;

    /// Rust `wait_for_ready` with the real HTTP probe.
    core::Expected<core::Unit, std::string> WaitForReady(int64_t timeout_ms,
                                                         int64_t retry_interval_ms);

    /// Rust `wait_for_ready(timeout, retry_interval)`.
    /// Loops probing until healthy or the deadline elapses. HEALTH_PROBE_TIMEOUT
    /// (1s) caps each probe; retry_interval spaces attempts. `probe` is invoked
    /// for each attempt; when null a default always-fail probe is used.
    core::Expected<core::Unit, std::string> WaitForReady(int64_t timeout_ms,
                                                         int64_t retry_interval_ms,
                                                         const HealthProbe& probe);

    /// Rust `EnvdInstance::init`.
    ///
    /// Two sequences, selected by whether the requested default user can be
    /// handed to envd as-is:
    ///
    ///   * An ordinary name goes straight through as a single `POST /init`.
    ///   * A bare UID or a `user:group` pair needs the guest's account files
    ///     read first, which needs an authenticated runtime — so a
    ///     **bootstrap `init` runs first** with no env, `/` as the workdir and
    ///     `root` as the user. Skipping it makes every account read 401, and
    ///     running it *after* resolution is the same failure.
    ///
    /// Only `default_user` is replaced by resolution; `env_vars` and
    /// `default_workdir` reach the final call exactly as the caller passed
    /// them. The bootstrap call deliberately carries neither, because it is
    /// not the sandbox's configuration — overwriting the image environment
    /// with nothing and then setting it back would leave a window in which a
    /// guest that read its environment saw an empty one.
    core::Expected<core::Unit, std::string> Init(
        const core::Optional<std::map<std::string, std::string> >& env_vars,
        const core::Optional<std::string>& default_workdir,
        const core::Optional<std::string>& default_user);

    /// Rust `EnvdInstance::post_init` — one `POST /init`.
    core::Expected<core::Unit, std::string> PostInit(
        const core::Optional<std::map<std::string, std::string> >& env_vars,
        const core::Optional<std::string>& default_workdir,
        const core::Optional<std::string>& default_user) const;

    /// Rust `envd::user::resolve_default_user`.
    ///
    /// Reads `/etc/passwd` (and `/etc/group` only when a non-numeric group was
    /// named — an unnecessary read is a round trip into a guest that may not
    /// have the file), resolves, and appends the record when one is needed.
    /// A non-zero exit from the append is an error naming the requested user,
    /// because the resolved name would otherwise be returned for an account
    /// that was never created.
    core::Expected<std::string, std::string> ResolveDefaultUser(const std::string& user);

    /// Rust `envd::user::read_account_file`.
    ///
    /// `GET {base}/files?path=<path>&username=root`. A 404 is an *empty file*,
    /// not an error: an image need not ship `/etc/group`, and refusing there
    /// would reject every such image. Any other client or server status is an
    /// error. Capped at 1 MiB.
    core::Expected<std::string, std::string> ReadAccountFile(const std::string& path) const;

    /// Rust `EnvdApi::metrics` for `EnvdInstance`.
    ///
    /// Bounded by `kMetricsRequestTimeoutMs` rather than the client default:
    /// the sampling sweep must stay on schedule, and one wedged guest is worth
    /// far less than the sweep for every other sandbox on the node.
    core::Expected<SandboxMetric, std::string> Metrics() const;

    /// Rust `HEALTH_PROBE_TIMEOUT` (1s).
    static const int64_t kHealthProbeTimeoutMs = 1000;
    /// Rust `read_account_file`'s `1024 * 1024` guard.
    static const int64_t kAccountFileMaxBytes = 1024 * 1024;
    /// Rust `tokio::time::timeout(Duration::from_secs(30), resolve_default_user(..))`.
    static const int64_t kResolveDefaultUserTimeoutMs = 30 * 1000;
    /// Rust `ProcessOpts::default().with_timeout(Duration::from_secs(10))` on
    /// the passwd append.
    static const int64_t kAddPasswdEntryTimeoutMs = 10 * 1000;
    /// Rust `METRICS_REQUEST_TIMEOUT` (200 ms), applied per guest call.
    static const int64_t kMetricsRequestTimeoutMs = 200;
    /// A `/metrics` body larger than this is refused rather than read: the
    /// response is eight scalars, so anything approaching a megabyte means the
    /// address is answering with something that is not envd.
    static const int64_t kMetricsMaxBytes = 64 * 1024;

 private:
    std::string base_path_;
    std::string grpc_address_;
    core::Optional<EnvdAccessToken> access_token_;
    /// Rust `live: Arc<AtomicBool>`.
    ///
    /// `shared_ptr` rather than a member flag so copies share it (see
    /// `Invalidate`), and `atomic` rather than a plain `bool` because the
    /// thread that tears a sandbox down is not the thread holding the handle:
    /// a non-atomic flag read concurrently with that store is a data race, and
    /// the value a racing reader sees is exactly what decides whether a
    /// request is sent to a reused IP address.
    std::shared_ptr<std::atomic<bool> > live_;
    RootCommandRunner root_command_runner_;
};

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_ENVD_H_
