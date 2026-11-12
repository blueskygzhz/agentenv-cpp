// SPDX-License-Identifier: MIT
// Rust: src/api/generated/src/models.rs  (subset of the E2B-compatible schema
// in src/api/openapi.yml, from which those models are generated). Upstream has
// no hand-written `dto` module; this header stands in for the generated one.
#ifndef AGENTENV_API_DTO_H_
#define AGENTENV_API_DTO_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// `AttachedDriveRequest` (Rust `models::AttachedDrive`) is declared there,
// next to `ResolveAttachedDrives` which consumes it. Included rather than
// re-declared so the create bodies can hand what they parsed straight to the
// resolver; a second identical wire struct would not convert.
#include "agentenv/api/attached_drives.h"
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

/// Rust generated `models::NewColdSandbox`, restricted to the resource fields.
///
/// Here rather than in `sandbox_params.h` for the same reason
/// `SandboxVolumeMountModel` is: `CreateColdSandboxReq` below carries one, and
/// `sandbox_params.h` includes *this* header. `ColdStartResources`, which
/// validates it, stays declared there.
struct NewColdSandboxResources {
    core::Optional<uint32_t> cpu_count;
    core::Optional<uint32_t> memory_mb;
    core::Optional<uint32_t> disk_size_mb;
};

/// Rust generated `models::SandboxNetworkConfig`.
///
/// In this header, not `sandbox_params.h`, for the same reason as the two
/// above: `CreateSandboxReq` and `CreateColdSandboxReq` carry one, and
/// `sandbox_params.h` includes *this* header. `NetworkPolicyFromCreate`, which
/// consumes it, stays declared there.
struct SandboxNetworkConfigModel {
    core::Optional<std::vector<std::string> > allow_out;
    core::Optional<std::vector<std::string> > deny_out;
    core::Optional<bool>                      allow_public_traffic;
};

/// POST /sandboxes  request body (Rust generated `models::NewSandbox`).
///
/// The JSON keys are upstream's `#[serde(rename)]` spellings, which are *not*
/// uniform: `templateID` and `envVars` are camelCase while
/// `allow_internet_access` is snake_case. `NewColdSandbox` spells that same
/// field `allowInternetAccess`. Reproduced rather than regularised, because
/// this header claims E2B wire compatibility and a client speaks the schema's
/// spelling, not a tidier one.
struct CreateSandboxReq {
    /// Rust `template_id: String`, key `templateID` — the only required field.
    std::string template_id;
    /// Rust `env_vars`, key `envVars`.
    std::vector<std::string> env_vars;
    /// Rust `timeout: Option<u32>`, key `timeout`, in **seconds**.
    ///
    /// Optional, and the schema's `default: 15` is deliberately *not* applied
    /// here: the generated model carries no `#[serde(default)]`, so an omitted
    /// timeout reaches `duration_from_secs(None)` as `None` and the node's
    /// configured default applies. Baking 15 in would override that default
    /// with a value no operator chose.
    core::Optional<int64_t> timeout_secs;
    /// Rust `secure: body.secure == Some(true)` — only a JSON `true` turns on
    /// envd access-token enforcement.
    bool        secure = false;
    /// Rust `auto_pause: Option<bool>`, key `autoPause`. Only an explicit
    /// `false` selects `SandboxTimeoutAction::Delete`; absent and `true` both
    /// mean Pause.
    core::Optional<bool> auto_pause;
    /// Rust `auto_resume: Option<SandboxAutoResumeConfig>`, key `autoResume`,
    /// read as `is_some_and(|cfg| cfg.enabled)`.
    ///
    /// Flattened to the inner `enabled` flag because that is the only member
    /// the schema defines and the only one upstream reads. The nesting is
    /// still honoured on the wire: `{"autoResume": {"enabled": true}}`.
    core::Optional<bool> auto_resume;
    /// Rust `allow_internet_access: Option<bool>` — note the snake_case key,
    /// which is upstream's spelling for *this* body only. A tri-state: absent
    /// means "inherit the node default", which is distinct from an explicit
    /// allow or deny.
    core::Optional<bool> allow_internet_access;
    /// Rust `network: Option<SandboxNetworkConfig>`.
    core::Optional<SandboxNetworkConfigModel> network;
    /// Rust `metadata: Option<HashMap<String, String>>`.
    std::map<std::string, std::string> metadata;
    /// Rust `custom_extension_params: Option<CustomExtensionParams>`, key
    /// `customExtensionParams`.
    ///
    /// Held as raw JSON rather than as `sandbox::custom_extension::Params` so
    /// this header need not include the whole custom-extension client. The
    /// conversion happens in the handler, which is where upstream's
    /// `params_model_to_map` runs too.
    core::Optional<core::Json> custom_extension_params;
    /// Rust `volume_mounts: Option<Vec<models::SandboxVolumeMount>>`, JSON key
    /// `volumeMounts`.
    ///
    /// Optional rather than a plain vector because upstream's
    /// `sandboxes_post` branches on `is_some()`, not on emptiness: an absent
    /// list means "restore whatever volumes the snapshot captured", while a
    /// present empty list means "this sandbox has no volumes". The schema says
    /// so outright -- "providing this field, including an empty array, returns
    /// HTTP 400" -- so collapsing the two would turn a documented 400 into a
    /// silent restore.
    core::Optional<std::vector<SandboxVolumeMountModel> > volume_mounts;
    /// Rust `attached_drives: Option<AttachedDrives>`, key `attachedDrives`.
    ///
    /// Optional for the same reason `volume_mounts` is, and the schema states
    /// it in the same words: providing the field "including an empty array"
    /// refuses the snapshot's captured drives, while omitting it restores
    /// them. Upstream passes `attached_drives.is_some()` to
    /// `check_snapshot_attached_drives`, so collapsing absent into empty would
    /// turn a documented 400 into a silent restore.
    ///
    /// Note that upstream's `NewSandbox` does *not* carry this field --
    /// `NewSandboxV2` does, and `create_sandbox_from_snapshot` takes it as a
    /// separate argument that `sandboxes_post` passes as `None`. It is carried
    /// here because this port has no V2 route, so this is the only body that
    /// could ever deliver it; the handler's behaviour for an absent value is
    /// identical to upstream's `sandboxes_post`.
    core::Optional<std::vector<AttachedDriveRequest> > attached_drives;

    // `mcp` is absent because nothing in this port implements MCP; a field
    // accepted and ignored would report success for a sandbox that never got
    // the servers the caller asked for.

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

/// POST /sandboxes-cold  request body (Rust generated `models::NewColdSandbox`).
///
/// A separate route, and a separate body, because `POST /sandboxes` genuinely
/// does not take an image: upstream's `NewSandbox` schema carries only
/// `templateID`, and its own description points callers here to "create a
/// sandbox directly from an external OCI image". Adding an `image` field to
/// `CreateSandboxReq` would have diverged from the schema rather than filled a
/// gap in it.
struct CreateColdSandboxReq {
    /// Rust `image: String` — required, and the only required field. An OCI
    /// reference such as `python:3.11-slim`.
    std::string image;
    /// Rust's flattened resource fields. Held as the same struct
    /// `ColdStartResources` validates, so the validation cannot be applied to
    /// a different set of values than the ones parsed.
    NewColdSandboxResources resources;
    std::vector<std::string> env_vars;
    /// Rust `timeout: Option<i64>`, in **seconds** (`duration_from_secs`).
    /// Unset applies the node default, which is why this is not an `int64_t`
    /// with a sentinel: zero is a legal explicit "no timeout" upstream.
    core::Optional<int64_t> timeout_secs;
    bool secure = false;
    /// Rust `auto_pause: Option<bool>` — only an explicit `false` turns the
    /// timeout action into Delete. Absent means Pause, as does `true`.
    core::Optional<bool> auto_pause;
    /// Rust `auto_resume`, key `autoResume`, flattened to its `enabled` member
    /// exactly as `CreateSandboxReq::auto_resume` is.
    core::Optional<bool> auto_resume;
    /// Rust `allow_internet_access: Option<bool>` — key `allowInternetAccess`
    /// here. `NewSandbox` spells the same field `allow_internet_access`; the
    /// inconsistency is upstream's and both spellings are reproduced.
    core::Optional<bool> allow_internet_access;
    /// Rust `network: Option<SandboxNetworkConfig>`.
    core::Optional<SandboxNetworkConfigModel> network;
    std::map<std::string, std::string> metadata;
    /// Rust `custom_extension_params`, key `customExtensionParams`. Raw JSON,
    /// for the reason given on `CreateSandboxReq`'s copy of this field.
    core::Optional<core::Json> custom_extension_params;
    /// Rust `volume_mounts: Option<Vec<models::SandboxVolumeMount>>`.
    core::Optional<std::vector<SandboxVolumeMountModel> > volume_mounts;
    /// Rust `attached_drives: Option<AttachedDrives>`, key `attachedDrives`.
    /// A cold boot has no captured drives to refuse, so absent and `[]` mean
    /// the same thing here; the optional is kept only so the two create bodies
    /// carry the field identically.
    core::Optional<std::vector<AttachedDriveRequest> > attached_drives;
    /// Rust `extra_boot_args: Option<String>`, appended to the guest kernel
    /// command line.
    core::Optional<std::string> extra_boot_args;

    core::Json ToJson() const;
    static core::Expected<CreateColdSandboxReq, core::AnyError> FromJson(const core::Json& j);
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

/// Rust `models::SandboxSnapshotRequest` — POST /sandboxes/{id}/snapshots body.
struct CreateSnapshotReq {
    /// Rust `name` — an optional alias for the published snapshot. Absent is
    /// distinct from empty: an absent name publishes without an alias, while
    /// an empty one is a malformed alias and is rejected.
    core::Optional<std::string> name;

    core::Json ToJson() const;
    static core::Expected<CreateSnapshotReq, core::AnyError> FromJson(const core::Json& j);
};

/// Rust `models::SnapshotInfo` — the published snapshot, as the create and
/// read handlers both report it.
struct SnapshotInfoResp {
    std::string snapshot_id;
    /// Rust `names` — the alias as a one-element list, empty when there is
    /// none. A list because upstream's schema says so; a snapshot carries at
    /// most one alias.
    std::vector<std::string> names;
    uint32_t cpu_count = 0;
    uint64_t memory_mb = 0;
    uint64_t disk_size_mb = 0;
    int64_t created_at_unix_ms = 0;
    int64_t updated_at_unix_ms = 0;
    /// Rust `image_ref` — set only when the rootfs was published to a
    /// registry, so its absence is meaningful rather than a missing field.
    core::Optional<std::string> image_ref;

    core::Json ToJson() const;
    static core::Expected<SnapshotInfoResp, core::AnyError> FromJson(const core::Json& j);
};

// No `ExecReq`/`ExecResp`: upstream has no `exec` endpoint to model. See the
// note in `server.cc` where the route used to be registered -- running a
// command in a sandbox reaches envd through the proxy, so there is no control
// plane request body for it, and these two described a route only this port
// ever served.

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
