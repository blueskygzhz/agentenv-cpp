// SPDX-License-Identifier: MIT
// Rust: src/sandbox/custom_extension/{client,mod}.rs
//
// The custom extension is an external HTTP service configured via
// `[custom_extension].url`. Its only current capability is sandbox lifecycle
// hooks, invoked by the sandbox runtime (the Firecracker backend):
//
// - `POST {url}/sandbox-hook/start-fresh` -- before a fresh sandbox boots
//   (may return extra boot args),
// - `POST {url}/sandbox-hook/start-resume` -- before a sandbox resumes from a
//   snapshot,
// - `POST {url}/sandbox-hook/patch-params` -- applies an extension-defined
//   patch to the custom extension params and returns the updated full
//   params; a failure rejects the patch,
// - `POST {url}/sandbox-hook/stop` -- when a sandbox stops, before its
//   network slot is released (best-effort: failures are only logged).
//
// Sandbox ids are reused across pause/resume, so each start hook carries a
// fresh `SandboxInstanceId`, and the stop hook carries the id of the instance
// being torn down. Except for the stop hook, any failure (connection error,
// timeout, non-2xx) fails the sandbox operation; a 4xx/5xx answer is kept as
// a `HookStatusError` in the error chain so the API layer can surface
// extension 4xx responses as client errors and 5xx as server errors.
#ifndef AGENTENV_SANDBOX_CUSTOM_EXTENSION_H_
#define AGENTENV_SANDBOX_CUSTOM_EXTENSION_H_

#include <cstdint>
#include <memory>
#include <string>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/http.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace sandbox {
namespace custom_extension {

/// Rust `CustomExtensionParams` (`serde_json::Map<String, Value>`), carried as
/// its JSON text so the caller's bytes reach the extension unchanged.
struct Params {
    std::string json_bytes;
};

/// Rust `custom_extension_params_is_empty`: absent or an empty map. The
/// optional is the absent case; here an empty text, `null` or an object with
/// no members count as empty.
bool ParamsIsEmpty(const Params& p);
bool ParamsIsEmpty(const core::Optional<Params>& p);

/// Rust struct `SandboxInstanceId` -- a fresh UUIDv7 per sandbox start (fresh
/// boot or resume). Together with the sandbox id it disambiguates lifecycle
/// hooks across pause/resume, where the sandbox id is reused.
struct SandboxInstanceId {
    core::Uuid value;
    static SandboxInstanceId New();  // Rust: Uuid::now_v7()
    std::string ToString() const { return value.ToString(); }
};

/// Rust hook endpoints under `POST {url}/sandbox-hook/...`.
namespace hook {
static const char* const kStartFresh  = "start-fresh";
static const char* const kStartResume = "start-resume";
static const char* const kPatchParams = "patch-params";
static const char* const kStop        = "stop";
}  // namespace hook

/// Rust `reqwest::StatusCode` Display: `"403 Forbidden"`.
std::string StatusCodeDisplay(int status);

/// Rust struct `HookStatusError` -- a hook call the extension answered with
/// a 4xx/5xx status. Rides in the `AnyError` chain (`AnyError::Find`).
class HookStatusError : public core::ErrorPayload {
 public:
    HookStatusError(std::string operation, int status, std::string content)
        : operation_(std::move(operation)), status_(status), content_(std::move(content)) {}

    /// Whether the extension answered with a 4xx status.
    bool IsClientError() const { return status_ >= 400 && status_ < 500; }
    /// Whether the extension answered with a 5xx status.
    bool IsServerError() const { return status_ >= 500 && status_ < 600; }

    const std::string& Operation() const { return operation_; }
    int Status() const { return status_; }
    const std::string& Content() const { return content_; }

    /// `custom extension {operation} hook failed with status {status}: {content}`.
    std::string ToString() const;

    /// Rust `anyhow::Error::new(HookStatusError { .. })`.
    core::AnyError ToAnyError() const;

 private:
    std::string operation_;
    int status_;
    std::string content_;
};

/// The hook status error in `err` chain, or NULL (Rust
/// `chain().find_map(downcast_ref::<HookStatusError>)`).
const HookStatusError* FindHookStatusError(const core::AnyError& err);

/// Rust struct `CustomExtensionClient`. Abstract so the orchestrator and the
/// runtime can be driven by a fake in tests; `HttpClient` is the real one.
class Client {
 public:
    virtual ~Client() {}

    /// Rust `CustomExtensionClient::global()` -- the process-wide client, or
    /// NULL when `[custom_extension].url` is unset (or the config is not
    /// loaded yet; nothing is cached in that case).
    static std::shared_ptr<Client> Global();

    /// Rust `hook_start_fresh` -- returns the extra kernel boot args, if any.
    virtual core::Expected<core::Optional<std::string>, core::AnyError> HookStartFresh(
        const core::SandboxId& sandbox_id, const SandboxInstanceId& instance_id,
        const std::string& network_namespace_path, const std::string& host_interaction_ip,
        const core::Optional<Params>& params) = 0;

    /// Rust `hook_start_resume`.
    virtual core::Expected<core::Unit, core::AnyError> HookStartResume(
        const core::SandboxId& sandbox_id, const SandboxInstanceId& instance_id,
        const std::string& network_namespace_path, const std::string& host_interaction_ip,
        const core::Optional<Params>& params) = 0;

    /// Rust `hook_patch_params` -- the patch document goes through verbatim;
    /// the hook returns the updated full params (`None` = empty params). The
    /// orchestrator treats the returned value as authoritative.
    virtual core::Expected<core::Optional<Params>, core::AnyError> HookPatchParams(
        const core::SandboxId& sandbox_id, const Params& patch) = 0;

    /// Rust `hook_stop` -- best-effort: failures are logged, never returned.
    virtual void HookStop(const core::SandboxId& sandbox_id,
                          const SandboxInstanceId& instance_id) = 0;
};

/// Rust `CustomExtensionClient` over the generated OpenAPI client
/// (`custom_extension_client::apis::default_api`).
class HttpClient : public Client {
 public:
    /// Rust `build_from_config`: NULL when `url` is unset or blank. The base
    /// path is the trimmed URL without trailing slashes. `timeout_ms` bounds
    /// the connect and every read (reqwest bounds the whole request).
    ///
    /// Only `http://` is supported: this build has no TLS client, so an
    /// `https://` URL disables the integration with a warning, as Rust does
    /// when its HTTP client cannot be built.
    static std::shared_ptr<HttpClient> FromConfig(const core::Optional<std::string>& url,
                                                  uint64_t timeout_ms);

    core::Expected<core::Optional<std::string>, core::AnyError> HookStartFresh(
        const core::SandboxId& sandbox_id, const SandboxInstanceId& instance_id,
        const std::string& network_namespace_path, const std::string& host_interaction_ip,
        const core::Optional<Params>& params) override;

    core::Expected<core::Unit, core::AnyError> HookStartResume(
        const core::SandboxId& sandbox_id, const SandboxInstanceId& instance_id,
        const std::string& network_namespace_path, const std::string& host_interaction_ip,
        const core::Optional<Params>& params) override;

    core::Expected<core::Optional<Params>, core::AnyError> HookPatchParams(
        const core::SandboxId& sandbox_id, const Params& patch) override;

    void HookStop(const core::SandboxId& sandbox_id,
                  const SandboxInstanceId& instance_id) override;

    const core::http::Target& target() const { return target_; }
    const std::string& path_prefix() const { return path_prefix_; }

 private:
    HttpClient(const core::http::Target& target, std::string path_prefix,
               const core::http::Timeouts& timeouts)
        : target_(target), path_prefix_(std::move(path_prefix)), timeouts_(timeouts) {}

    /// One `POST {base}/sandbox-hook/{operation}` with a JSON body. A
    /// transport failure is the generated client `Error::Reqwest`.
    core::Expected<core::http::Response, std::string> Post(const char* operation,
                                                           const std::string& body) const;

    core::http::Target target_;
    std::string path_prefix_;
    core::http::Timeouts timeouts_;
};

/// Rust struct `CustomExtensionHookGuard` -- RAII owner of the custom
/// extension lifecycle of one sandbox runtime instance.
///
/// `StartFresh` / `StartResume` generate a fresh instance id and record it
/// *before* the hook is delivered, so a start whose response was lost (e.g. a
/// client-side timeout after the extension processed it) still gets a
/// matching stop on teardown; an extension that never processed the start
/// ignores that stop. `Stop` delivers the stop hook synchronously and clears
/// the id; destroying a guard that still holds an id fires the stop hook
/// fire-and-forget on the shared executor (never blocking the destructor).
class HookGuard {
 public:
    HookGuard(std::shared_ptr<Client> client, core::SandboxId sandbox_id);
    ~HookGuard();

    HookGuard(const HookGuard&) = delete;
    HookGuard& operator=(const HookGuard&) = delete;

    /// Rust `start_fresh` -- returns the extra kernel boot args, if any.
    core::Expected<core::Optional<std::string>, core::AnyError> StartFresh(
        const std::string& network_namespace_path, const std::string& host_interaction_ip,
        const core::Optional<Params>& params);

    /// Rust `start_resume`.
    core::Expected<core::Unit, core::AnyError> StartResume(
        const std::string& network_namespace_path, const std::string& host_interaction_ip,
        const core::Optional<Params>& params);

    /// Rust `stop` -- deliver the stop hook now (awaited) and clear the id.
    void Stop();

    /// The id established by the last start, or none (not started / stopped).
    const core::Optional<SandboxInstanceId>& InstanceId() const { return instance_id_; }

 private:
    std::shared_ptr<Client> client_;
    core::SandboxId sandbox_id_;
    core::Optional<SandboxInstanceId> instance_id_;
};

}  // namespace custom_extension
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_CUSTOM_EXTENSION_H_
