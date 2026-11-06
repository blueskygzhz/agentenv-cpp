// SPDX-License-Identifier: MIT
// Rust: src/sandbox/envd.rs
//
// The HTTP control plane of the guest agent handle: readiness, `POST /init`,
// the account-file read the default-user resolution stands on, and the shared
// liveness flag that stops a stale handle from reaching a reused address.
//
// Not here: the two gRPC channels (`process_client` / `filesystem_client`) and
// `EnvdApi::{metrics,logs}`. Their liveness gate is ported (see
// `ProcessClient`); the transports are tonic-generated services with no C++
// equivalent in this tree.
#include "agentenv/sandbox/envd.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>

#include "agentenv/core/http.h"
#include "agentenv/core/time.h"

namespace agentenv {
namespace sandbox {

namespace {
int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
void sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, nullptr);
}
}  // namespace

const char* const kEnvdLogDirectory = "/var/log/agentenv/envd";
const char* const kEnvdStderrPath = "/var/log/agentenv/envd.stderr.log";

// `std::min` takes its arguments by const reference, which odr-uses the
// constant below. An in-class initialiser alone is only a compile-time value;
// without this definition the reference has nothing to bind to and the link
// fails.
const int64_t EnvdInstance::kHealthProbeTimeoutMs;
const int64_t EnvdInstance::kAccountFileMaxBytes;
const int64_t EnvdInstance::kResolveDefaultUserTimeoutMs;
const int64_t EnvdInstance::kAddPasswdEntryTimeoutMs;

EnvdInstance::EnvdInstance(std::string base_path)
    : base_path_(base_path),
      grpc_address_(base_path),
      live_(new std::atomic<bool>(true)) {}

EnvdInstance::EnvdInstance(std::string base_path,
                           const core::Optional<EnvdAccessToken>& access_token)
    : base_path_(base_path),
      grpc_address_(base_path),
      access_token_(access_token),
      live_(new std::atomic<bool>(true)) {}

void EnvdInstance::Invalidate() { live_->store(false, std::memory_order_release); }

core::Expected<core::Unit, std::string> EnvdInstance::EnsureLive() const {
    if (live_->load(std::memory_order_acquire)) return core::Unit{};
    return core::make_unexpected(std::string("sandbox runtime is no longer active"));
}

core::Expected<core::Unit, std::string> EnvdInstance::ProcessClient() const {
    // Rust: `self.ensure_live()?` *then* `ProcessClient::connect(..)`. The gate
    // runs first, so an invalidated runtime reports that rather than the
    // transport -- see the header for why the two must stay distinct.
    const core::Expected<core::Unit, std::string> live = EnsureLive();
    if (!live.ok()) return live;
    return core::make_unexpected(
        std::string("envd process client is not available in this build (needs the envd "
                    "process gRPC service)"));
}

core::Expected<core::Unit, std::string> EnvdInstance::FilesystemClient() const {
    const core::Expected<core::Unit, std::string> live = EnsureLive();
    if (!live.ok()) return live;
    return core::make_unexpected(
        std::string("envd filesystem client is not available in this build (needs the envd "
                    "filesystem gRPC service)"));
}

void EnvdInstance::SetRootCommandRunner(const RootCommandRunner& runner) {
    root_command_runner_ = runner;
}

namespace {

/// `http://<host>:<port>[/prefix]` -> target + path prefix. Only the form
/// `init_envd_instance` builds is accepted.
bool ParseHttpBase(const std::string& base, core::http::Target* target, std::string* prefix) {
    const std::string scheme = "http://";
    if (base.compare(0, scheme.size(), scheme) != 0) return false;
    const std::string rest = base.substr(scheme.size());
    const std::string::size_type slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    *prefix = slash == std::string::npos ? std::string() : rest.substr(slash);
    while (!prefix->empty() && (*prefix)[prefix->size() - 1] == '/') {
        prefix->erase(prefix->size() - 1);
    }
    const std::string::size_type colon = authority.rfind(':');
    std::string host = authority;
    unsigned long port = 80;
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string digits = authority.substr(colon + 1);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        port = std::strtoul(digits.c_str(), NULL, 10);
        if (port == 0 || port > 65535) return false;
    }
    if (host.empty()) return false;
    *target = core::http::Target::Tcp(host, static_cast<uint16_t>(port));
    return true;
}

/// `serde_urlencoded`, which is what reqwest's `.query(&[..])` serialises
/// with: the unreserved set plus `*` passes, a space becomes `+`, and every
/// other byte becomes uppercase `%XX`.
///
/// This is not cosmetic. A guest path is a caller-influenced value spliced
/// into a query string, so an unencoded `&` or `#` would end the parameter
/// early and envd would read a *different* path than the one asked for --
/// which, on this call path, is the file the default user is derived from.
std::string FormUrlEncode(const std::string& value) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (std::string::size_type i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '*';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

EnvdInstance::HealthProbe EnvdInstance::HttpHealthProbe() const {
    const std::string base = base_path_;
    return [base](int32_t probe_timeout_ms) -> bool {
        core::http::Target target;
        std::string prefix;
        if (!ParseHttpBase(base, &target, &prefix)) return false;
        core::http::Timeouts timeouts;
        timeouts.connect_ms = probe_timeout_ms;
        timeouts.read_ms = probe_timeout_ms;
        core::http::Request request;
        request.method = "GET";
        request.path = prefix + "/health";
        const core::Expected<core::http::Response, std::string> response =
            core::http::HttpClient(target, timeouts).Request(request);
        // reqwest `!is_client_error() && !is_server_error()`.
        return response.ok() && response.value().status < 400;
    };
}

core::Expected<core::Unit, std::string>
EnvdInstance::WaitForReady(int64_t timeout_ms, int64_t retry_interval_ms) {
    return WaitForReady(timeout_ms, retry_interval_ms, HttpHealthProbe());
}

core::Expected<core::Unit, std::string>
EnvdInstance::WaitForReady(int64_t timeout_ms, int64_t retry_interval_ms,
                           const HealthProbe& probe) {
    const int64_t start = now_ms();

    for (;;) {
        const int64_t elapsed = now_ms() - start;
        if (elapsed >= timeout_ms) {
            return core::make_unexpected(std::string("timed out waiting for envd"));
        }

        const int64_t remaining = timeout_ms - elapsed;
        const int64_t probe_timeout =
            std::min<int64_t>(kHealthProbeTimeoutMs, remaining);

        bool healthy = false;
        if (probe) {
            healthy = probe(static_cast<int32_t>(probe_timeout));
        }
        if (healthy) {
            return core::Unit{};
        }

        // Rust: remaining = timeout.saturating_sub(elapsed); if zero -> timeout.
        const int64_t remaining_after = timeout_ms - (now_ms() - start);
        if (remaining_after <= 0) {
            return core::make_unexpected(std::string("timed out waiting for envd"));
        }
        sleep_ms(std::min<int64_t>(retry_interval_ms, remaining_after));
    }
}

// ---- POST /init ------------------------------------------------------------

core::Json InitPostRequest::ToJson() const {
    core::JsonObject body;
    // serde `skip_serializing_if = "Option::is_none"`: an unset field is
    // absent, never `null`.
    if (env_vars.has_value()) {
        core::JsonObject vars;
        for (std::map<std::string, std::string>::const_iterator it = env_vars->begin();
             it != env_vars->end(); ++it) {
            vars[it->first] = core::Json(it->second);
        }
        body["envVars"] = core::Json(vars);
    }
    if (access_token.has_value()) body["accessToken"] = core::Json(*access_token);
    if (timestamp.has_value()) body["timestamp"] = core::Json(*timestamp);
    if (default_user.has_value()) body["defaultUser"] = core::Json(*default_user);
    if (default_workdir.has_value()) body["defaultWorkdir"] = core::Json(*default_workdir);
    return core::Json(body);
}

core::Expected<core::Unit, std::string> EnvdInstance::PostInit(
    const core::Optional<std::map<std::string, std::string> >& env_vars,
    const core::Optional<std::string>& default_workdir,
    const core::Optional<std::string>& default_user) const {
    core::http::Target target;
    std::string prefix;
    if (!ParseHttpBase(base_path_, &target, &prefix)) {
        return core::make_unexpected(std::string("envd base path is not an http:// URL: ") +
                                     base_path_);
    }

    InitPostRequest request;
    request.env_vars = env_vars;
    request.default_workdir = default_workdir;
    request.default_user = default_user;
    if (access_token_.has_value()) {
        request.access_token = access_token_->Expose();
    }
    // Rust `chrono::Utc::now().fixed_offset()`, serialised by chrono's serde
    // impl.
    //
    // Deviation, representational only: chrono writes a numeric `+00:00`
    // offset and omits trailing zeros in the fraction, where this writes `Z`
    // and nine digits. Both are RFC 3339 and envd parses either; the field's
    // *purpose* is to carry the wall clock into a guest that has none after a
    // restore, and that value is identical.
    request.timestamp = core::FormatRfc3339Nanos(core::SystemTime::Now());

    core::http::Request http_request;
    http_request.method = "POST";
    http_request.path = prefix + "/init";
    http_request.headers["Content-Type"] = "application/json";
    if (access_token_.has_value()) {
        http_request.headers["X-Access-Token"] = access_token_->Expose();
    }
    http_request.body = request.ToJson().ToString();

    const core::Expected<core::http::Response, std::string> response =
        core::http::HttpClient(target).Request(http_request);
    if (!response.ok()) return core::make_unexpected(response.error());
    // reqwest `!is_client_error() && !is_server_error()`.
    if (response.value().status >= 400) {
        return core::make_unexpected(std::string("envd init failed with status ") +
                                     std::to_string(response.value().status) + ": " +
                                     response.value().body);
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string> EnvdInstance::Init(
    const core::Optional<std::map<std::string, std::string> >& env_vars,
    const core::Optional<std::string>& default_workdir,
    const core::Optional<std::string>& default_user) {
    core::Optional<std::string> resolved_user = default_user;
    if (default_user.has_value() && EnvdUserNeedsResolution(*default_user)) {
        // Authenticate this runtime first, including after restore. Account
        // setup runs before the sandbox becomes available.
        //
        // The arguments are not a placeholder: `/` and `root` are what make
        // the following `/files` reads succeed, and passing the caller's env
        // here instead would publish the sandbox's configuration under a
        // default user that does not exist yet.
        const core::Expected<core::Unit, std::string> bootstrap =
            PostInit(core::Optional<std::map<std::string, std::string> >(),
                     core::Optional<std::string>(std::string("/")),
                     core::Optional<std::string>(std::string("root")));
        if (!bootstrap.ok()) return bootstrap;

        const core::Expected<std::string, std::string> name =
            ResolveDefaultUser(*default_user);
        if (!name.ok()) return core::make_unexpected(name.error());
        resolved_user = core::Optional<std::string>(name.value());
    }
    return PostInit(env_vars, default_workdir, resolved_user);
}

// ---- GET /files (account files) --------------------------------------------

core::Expected<std::string, std::string> EnvdInstance::ReadAccountFile(
    const std::string& path) const {
    core::http::Target target;
    std::string prefix;
    if (!ParseHttpBase(base_path_, &target, &prefix)) {
        return core::make_unexpected(std::string("envd base path is not an http:// URL: ") +
                                     base_path_);
    }

    core::http::Request request;
    request.method = "GET";
    // Rust `files_api::files_get(cfg, Some(path), Some("root"), None, None)` --
    // the two absent parameters are the signed-URL pair and must stay absent,
    // or envd would validate a signature that was never produced.
    request.path = prefix + "/files?path=" + FormUrlEncode(path) + "&username=root";
    if (access_token_.has_value()) {
        request.headers["X-Access-Token"] = access_token_->Expose();
    }

    const core::Expected<core::http::Response, std::string> response =
        core::http::HttpClient(target).Request(request);
    if (!response.ok()) {
        return core::make_unexpected(std::string("read guest ") + path +
                                     " to resolve Dockerfile USER");
    }
    // Rust matches `Error::ResponseError` with `status == NOT_FOUND` -> empty.
    // An image need not ship `/etc/group`.
    if (response.value().status == 404) return std::string();
    if (response.value().status >= 400) {
        return core::make_unexpected(std::string("read guest ") + path +
                                     " to resolve Dockerfile USER");
    }
    // Rust enforces the cap while streaming, so a hostile guest cannot make
    // the server buffer more than 1 MiB. Deviation: this client has no
    // streaming body, so the whole response is already in memory when the cap
    // is applied. The *result* is identical -- an oversized file is refused
    // with the same message -- but the memory bound is the response size
    // rather than 1 MiB.
    if (static_cast<int64_t>(response.value().body.size()) > kAccountFileMaxBytes) {
        return core::make_unexpected(std::string("guest ") + path +
                                     " exceeds the 1 MiB account-file limit");
    }
    return response.value().body;
}

// ---- GET /metrics ----------------------------------------------------------

core::Expected<SandboxMetric, std::string> EnvdInstance::Metrics() const {
    // `ensure_live` first, exactly as upstream. Not an optimisation: a handle
    // taken before this sandbox was torn down must not reach the address a
    // different VM has since been given, and that check is the only thing
    // standing between a stale copy and another tenant's guest.
    const core::Expected<core::Unit, std::string> live = EnsureLive();
    if (!live.ok()) return core::make_unexpected(live.error());

    core::http::Target target;
    std::string prefix;
    if (!ParseHttpBase(base_path_, &target, &prefix)) {
        return core::make_unexpected(std::string("envd base path is not an http:// URL: ") +
                                     base_path_);
    }

    // Upstream bounds this with `tokio::time::timeout`, which cancels the
    // request. Nothing here can cancel a blocking call, so the budget is
    // applied to the socket instead -- same wall-clock bound per call, and the
    // sweep's schedule is what the bound exists to protect.
    core::http::Timeouts timeouts;
    timeouts.connect_ms = static_cast<int32_t>(kMetricsRequestTimeoutMs);
    timeouts.read_ms = static_cast<int32_t>(kMetricsRequestTimeoutMs);

    core::http::Request request;
    request.method = "GET";
    request.path = prefix + "/metrics";
    if (access_token_.has_value()) {
        request.headers["X-Access-Token"] = access_token_->Expose();
    }

    const core::Expected<core::http::Response, std::string> response =
        core::http::HttpClient(target, timeouts).Request(request);
    if (!response.ok()) {
        return core::make_unexpected(std::string("sample guest metrics: ") + response.error());
    }
    if (response.value().status >= 400) {
        char status[16];
        std::snprintf(status, sizeof(status), "%d", response.value().status);
        return core::make_unexpected(std::string("sample guest metrics: HTTP ") + status);
    }
    if (static_cast<int64_t>(response.value().body.size()) > kMetricsMaxBytes) {
        return core::make_unexpected(
            std::string("guest metrics response is implausibly large"));
    }

    const core::Expected<core::Json, core::AnyError> json =
        core::Json::Parse(response.value().body);
    if (!json.ok()) {
        return core::make_unexpected(std::string("decode guest metrics: ") +
                                     json.error().chain());
    }
    // Validation lives in `SandboxMetric::FromEnvdJson`, which is also what
    // rejects the plausible-looking zeros an `Option` field decodes to.
    return SandboxMetric::FromEnvdJson(json.value());
}

core::Expected<std::string, std::string> EnvdInstance::ResolveDefaultUser(
    const std::string& user) {
    const int64_t deadline = now_ms() + kResolveDefaultUserTimeoutMs;
    // Rust bounds the whole resolution with `tokio::time::timeout`, which
    // *cancels* the request in flight. Nothing here can cancel a blocking
    // call, so the budget is checked between steps; the bound therefore
    // becomes the budget plus the one request already in flight.
    const auto expired = [&]() { return now_ms() >= deadline; };

    const core::Expected<std::string, std::string> passwd = ReadAccountFile("/etc/passwd");
    if (!passwd.ok()) return passwd;

    // Rust reads `/etc/group` only when the named group is non-numeric: a
    // numeric gid needs no lookup, and the extra round trip would be made
    // into a guest whose image may not ship the file at all.
    std::string groups;
    const std::string::size_type colon = user.find(':');
    if (colon != std::string::npos) {
        const std::string group = user.substr(colon + 1);
        bool numeric = !group.empty();
        for (std::string::size_type i = 0; i < group.size(); ++i) {
            if (group[i] < '0' || group[i] > '9') { numeric = false; break; }
        }
        if (!numeric) {
            if (expired()) {
                return core::make_unexpected(
                    std::string("timed out resolving Dockerfile USER"));
            }
            const core::Expected<std::string, std::string> read = ReadAccountFile("/etc/group");
            if (!read.ok()) return read;
            groups = read.value();
        }
    }

    const core::Expected<ResolvedUser, std::string> resolved =
        ResolveEnvdUser(user, passwd.value(), groups);
    if (!resolved.ok()) return core::make_unexpected(resolved.error());

    if (!resolved.value().passwd_entry.has_value()) return resolved.value().name;

    // envd requires a name even when Docker permits a UID without an account.
    // Add an identity without changing existing accounts.
    if (!root_command_runner_) {
        return core::make_unexpected(
            std::string("cannot create the guest account for Dockerfile USER ") + user +
            ": appending to /etc/passwd needs the envd process client, which is not "
            "available in this build");
    }
    if (expired()) {
        return core::make_unexpected(std::string("timed out resolving Dockerfile USER"));
    }

    std::vector<std::string> args;
    args.push_back("sh");
    args.push_back("-c");
    args.push_back(kAddPasswdEntryScript);
    args.push_back("sh");
    args.push_back(kEnvdBusyboxPath);
    args.push_back("/etc/passwd");
    args.push_back(*resolved.value().passwd_entry);

    ProcessOpts opts;
    opts.WithCwd("/").WithTimeoutMs(kAddPasswdEntryTimeoutMs);

    const core::Expected<ProcessOutput, std::string> output =
        root_command_runner_(kEnvdBusyboxPath, args, opts);
    if (!output.ok()) return core::make_unexpected(output.error());
    if (output.value().exit_code != 0) {
        return core::make_unexpected(std::string("failed to prepare Dockerfile USER ") + user +
                                     ": " + output.value().stderr_data);
    }
    return resolved.value().name;
}

// ---- guest log file names --------------------------------------------------

bool IsEnvdLogFilename(const std::string& name) {
    if (name == "current") return true;
    // daemontools' rotated name: `@` + 24 hex digits + `.s`, exactly 27 bytes.
    // The length, the anchor and the hex class together are what stop a
    // guest-chosen name from carrying `/` or `..` into the fetch path.
    if (name.size() != 27) return false;
    if (name[0] != '@') return false;
    if (name.compare(25, 2, ".s") != 0) return false;
    for (std::string::size_type i = 1; i < 25; ++i) {
        const char c = name[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

}  // namespace sandbox
}  // namespace agentenv
