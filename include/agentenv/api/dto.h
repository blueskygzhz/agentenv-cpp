// SPDX-License-Identifier: MIT
// Rust: src/api/dto/*.rs  (subset of E2B-compatible schema in openapi.yml)
#ifndef AGENTENV_API_DTO_H_
#define AGENTENV_API_DTO_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace api {

/// Rust generated `models::SandboxVolumeMount`. Both fields are required
/// there (plain `String`, not `Option`), so a mount missing either one is a
/// body that does not deserialize.
///
/// Lives here rather than in `sandbox_params.h` because `CreateSandboxReq`
/// carries a list of these, and `sandbox_params.h` includes *this* header.
struct SandboxVolumeMountModel {
    std::string name;
    std::string path;
};

/// POST /sandboxes  request body.
struct CreateSandboxReq {
    std::string template_id;
    std::vector<std::string> env_vars;
    int64_t     timeout_ms = 300000;
    /// Rust `secure: body.secure == Some(true)` — only a JSON `true` turns on
    /// envd access-token enforcement.
    bool        secure = false;
    /// Rust `volume_mounts: Option<Vec<models::SandboxVolumeMount>>`, JSON key
    /// `volumeMounts`.
    ///
    /// Optional rather than a plain vector because upstream's
    /// `sandboxes_post` branches on `is_some()`, not on emptiness: an absent
    /// list means "restore whatever volumes the snapshot captured", while a
    /// present empty list means "this sandbox has no volumes". Collapsing the
    /// two would make a restore indistinguishable from an explicit refusal of
    /// every volume.
    core::Optional<std::vector<SandboxVolumeMountModel> > volume_mounts;

    core::Json ToJson() const;
    static core::Expected<CreateSandboxReq, core::AnyError> FromJson(const core::Json& j);
};

/// POST /sandboxes  response body.
struct CreateSandboxResp {
    std::string sandbox_id;
    std::string template_id;
    std::string state;
    int64_t     created_at_ms = 0;
    /// Rust `models::Sandbox::envd_access_token` — present only for a secure
    /// sandbox. The client cannot reach envd without it.
    core::Optional<std::string> envd_access_token;
    /// Rust `traffic_access_token: Some(Nullable::Present|Null)` — the token
    /// when public traffic is disallowed, `null` otherwise.
    core::Optional<std::string> traffic_access_token;

    core::Json ToJson() const;
    static core::Expected<CreateSandboxResp, core::AnyError> FromJson(const core::Json& j);
};

/// GET /sandboxes/{id}  response body.
struct GetSandboxResp {
    std::string sandbox_id;
    std::string state;
    std::string template_id;
    int64_t     created_at_ms = 0;
    int64_t     started_at_ms = 0;
    /// Rust `SandboxDetail::envd_access_token` (detail only; the list leaves
    /// it unset, as Rust's `ListedSandbox` does).
    core::Optional<std::string> envd_access_token;

    core::Json ToJson() const;
    static core::Expected<GetSandboxResp, core::AnyError> FromJson(const core::Json& j);
};

/// GET /sandboxes  response body.
struct ListSandboxesResp {
    std::vector<GetSandboxResp> sandboxes;
    core::Json ToJson() const;
    static core::Expected<ListSandboxesResp, core::AnyError> FromJson(const core::Json& j);
};

/// POST /sandboxes/{id}/exec  request body.
struct ExecReq {
    std::vector<std::string> cmd;
    std::vector<std::string> env_vars;
    int32_t timeout_sec = 60;

    core::Json ToJson() const;
    static core::Expected<ExecReq, core::AnyError> FromJson(const core::Json& j);
};

/// POST /sandboxes/{id}/exec  response body.
struct ExecResp {
    int32_t     exit_code = 0;
    std::string stdout_output;
    std::string stderr_output;

    core::Json ToJson() const;
    static core::Expected<ExecResp, core::AnyError> FromJson(const core::Json& j);
};

/// Generic error body.
struct ErrorResp {
    std::string code;
    std::string message;
    core::Json ToJson() const;
};

/// Rust generated `models::Error` — the `{code, message}` body every handler
/// returns on failure. `code` is the HTTP status, mirroring
/// `models::Error::new(400, ...)` upstream.
struct ApiError {
    int         code = 500;
    std::string message;

    static ApiError Make(int code, const std::string& message);

    bool operator==(const ApiError& o) const {
        return code == o.code && message == o.message;
    }
    bool operator!=(const ApiError& o) const { return !(*this == o); }

    core::Json ToJson() const;
};

/// POST /sandboxes/{id}/fork  request body.
///
/// Rust `models::SandboxForkRequest`. Both fields are optional, and the
/// defaults differ in kind: an absent `count` means one child, while an absent
/// `timeout` means *inherit the source's*, which is not the same as no timeout.
struct ForkSandboxReq {
    /// Rust `count: Option<u32>`, validated to 1..=100.
    core::Optional<uint32_t> count;
    /// Rust `timeout: Option<u32>` in **seconds**.
    core::Optional<uint32_t> timeout_secs;

    core::Json ToJson() const;
    static core::Expected<ForkSandboxReq, core::AnyError> FromJson(const core::Json& j);
};

/// One entry of the fork response.
///
/// Rust `models::SandboxForkResult`: exactly one of the two is set. A fork of
/// N children reports N of these, because each child succeeds or fails on its
/// own -- collapsing them into one status would lose the children that worked.
struct ForkSandboxResultEntry {
    core::Optional<GetSandboxResp> sandbox;
    core::Optional<ApiError>       error;

    core::Json ToJson() const;
};

/// POST /sandboxes/{id}/fork  response body (HTTP 201).
struct ForkSandboxResp {
    std::vector<ForkSandboxResultEntry> results;
    core::Json ToJson() const;
};

/// POST /volumes  request body.
///
/// Rust `models::NewVolume`. Only `name` is required; every default is applied
/// by the handler rather than here, because two of them are not literals --
/// an absent `sizeMb` means `DEFAULT_VOLUME_SIZE_MB` and an absent `mode`
/// means exclusive, and both belong with the validation that reads them.
///
/// `image` and `from_volume` are alternative sources. Upstream resolves
/// `image` only when `from_volume` is absent, so the two are not rejected as
/// a conflict here either.
struct NewVolumeReq {
    std::string name;
    /// Rust `mode: Option<String>` — the API spelling (`"ro"` / `"exclusive"`),
    /// not the serde spelling of `VolumeMode`.
    core::Optional<std::string> mode;
    core::Optional<uint64_t>    size_mb;
    core::Optional<std::string> image;
    core::Optional<std::string> from_volume;

    core::Json ToJson() const;
    static core::Expected<NewVolumeReq, core::AnyError> FromJson(const core::Json& j);
};

/// GET /volumes  response.
///
/// Rust returns the records as the body and the continuation token as the
/// `x-next-token` *header*, so the two are kept apart here rather than merged
/// into one object: a client paging by header would never see a body field.
struct VolumePageResp {
    /// A JSON array of projected volume records (`VolumeRecordToJson`).
    core::Json records;
    core::Optional<std::string> next_token;
};

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_DTO_H_
