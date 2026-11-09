// SPDX-License-Identifier: MIT
// Rust: src/api/impls/template_helpers.rs — translates an E2B-shaped template
// build request into the internal `TemplateBuildSpec`.
//
// The request DTOs are generated types upstream (`models::TemplateBuildStartV2`
// and friends). There is no generator here, so they are declared as plain data
// in this header; the translation logic below is the part worth porting.
//
// The subtle piece is step numbering. One client step can expand into several
// internal steps — `ENV A=1 B=2` becomes two `Env` steps — but progress is
// reported against *client* step numbers. `StampSourceSteps` back-fills the
// client index over whatever range the expansion produced, so both halves of
// that ENV report as client step 1 rather than 1 and 2.
#ifndef AGENTENV_API_TEMPLATE_HELPERS_H_
#define AGENTENV_API_TEMPLATE_HELPERS_H_

#include <string>
#include <vector>

#include "agentenv/api/dto.h"
#include "agentenv/core/expected.h"
// `BuildLogQuery::level` is a `core::LogRecordLevel` — the serialised
// four-level severity, which is what a build log document stores.
#include "agentenv/core/logging.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/types.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/template/build_spec.h"
// `tpl::BuildLogEntry` is carried by value into the status body. Reused rather
// than mirrored by an API-side copy: upstream's `impl From<BuildLogEntry> for
// models::BuildLogEntry` is field-for-field, and the two serialise to the same
// four keys, so a second encoder here could only drift from the first.
#include "agentenv/template/logs.h"

namespace agentenv {
namespace api {

/// Rust generated `models::TemplateStep`.
struct TemplateStepRequest {
    /// Rust field `r_type` (`type` is reserved). Matched case-insensitively.
    std::string type;
    core::Optional<std::vector<std::string> > args;
    /// Set when the client uploaded a COPY context; unsupported, so a
    /// non-blank value is rejected rather than silently ignored.
    core::Optional<std::string> files_hash;
    core::Optional<bool>        force;

    /// Rust's generated `Deserialize`. `type` is the schema's only required
    /// field, and it is what selects the step kind, so a missing or
    /// wrong-typed one is an error rather than an empty string -- which
    /// `ApplyE2bTemplateStep` would report as an unknown step instead of a
    /// malformed request.
    static core::Expected<TemplateStepRequest, core::AnyError> FromJson(const core::Json& j);
};

/// Rust generated `models::TemplateBuildStartV2`.
struct TemplateBuildStartRequest {
    core::Optional<std::string> from_image;
    core::Optional<std::string> from_template;
    core::Optional<std::string> start_cmd;
    core::Optional<std::string> ready_cmd;
    core::Optional<std::vector<TemplateStepRequest> > steps;
    /// Rust `from_image_registry: Option<models::FromImageRegistry>` — the
    /// credentials for a private base image.
    ///
    /// Held as raw JSON because `RegistryAuth`'s three variants are not
    /// wired into the build path here. Carried rather than dropped so the
    /// handler can *refuse* it: a registry block accepted and ignored would
    /// pull anonymously, which either fails with an authentication error the
    /// caller already supplied credentials for, or silently resolves a
    /// *different, public* image of the same name.
    core::Optional<core::Json>  from_image_registry;
    /// Rust `force: Option<bool>` — rebuild regardless of the layer cache.
    core::Optional<bool>        force;

    /// Rust's generated `Deserialize`.
    static core::Expected<TemplateBuildStartRequest, core::AnyError>
        FromJson(const core::Json& j);
};

/// Rust generated `models::TemplateBuildRequestV3`.
struct TemplateBuildRequestV3 {
    /// Rust `name: Option<String>`, key `name`.
    ///
    /// Optional in the schema but required by every branch of the handler, so
    /// its absence is a 400 rather than a default: upstream picks the error
    /// message by branch ("template name" vs "image name"), which is why this
    /// is not validated here.
    core::Optional<std::string>              name;
    core::Optional<uint32_t>                 cpu_count;
    core::Optional<uint32_t>                 memory_mb;
    core::Optional<std::vector<std::string> > tags;
    /// Rust `image_only: Option<bool>`, key `imageOnly` — an AgentENV
    /// extension that builds a reusable *image* rather than a template.
    ///
    /// Carried rather than dropped even though this port cannot serve it: a
    /// field accepted and ignored would create the template the caller
    /// explicitly asked *not* to have, and point no tags at anything.
    core::Optional<bool>                     image_only;

    /// Rust's generated `Deserialize`. Lenient about types for the same reason
    /// the other request DTOs are: the generated layer upstream rejects a
    /// wrong-typed field before the handler sees it, and there is no generator
    /// here, so a malformed value is reported as a 400 by the route.
    static core::Expected<TemplateBuildRequestV3, core::AnyError>
        FromJson(const core::Json& j);
    core::Json ToJson() const;
};

/// Rust generated `models::TemplateRequestResponseV3` — the 202 body.
///
/// `template_id` and `build_id` are the same value, as upstream sets them:
/// AgentENV keeps one build per template, which is also what the V2 start
/// route asserts (`templateID and buildID must match`).
struct TemplateRequestResponseV3 {
    std::string              template_id;
    std::string              build_id;
    /// Rust passes `true` literally. Not a decision this port makes: there is
    /// no team model here, so reporting anything else would be inventing one.
    bool                     is_public = true;
    std::vector<std::string> names;
    std::vector<std::string> tags;
    std::vector<std::string> aliases;

    core::Json ToJson() const;
    static core::Expected<TemplateRequestResponseV3, core::AnyError>
        FromJson(const core::Json& j);
};

/// Rust generated `models::Template` — one entry of `GET /templates`.
///
/// Built from a `SnapshotRecord` by `FromRecord`, which is Rust's
/// `impl From<SnapshotRecord> for models::Template`.
struct TemplateModel {
    std::string              template_id;
    /// Rust reports `record.id` for both. AgentENV keeps one build per
    /// template, which is also what the V2 start route asserts.
    std::string              build_id;
    uint32_t                 cpu_count     = 0;
    uint32_t                 memory_mb     = 0;
    uint32_t                 disk_size_mb  = 0;
    /// Rust passes `true` literally; there is no team model here to report.
    bool                     is_public     = true;
    std::vector<std::string> names;
    int64_t                  created_at_unix_ms = 0;
    int64_t                  updated_at_unix_ms = 0;
    /// Rust `Nullable::Null` — nothing tracks last-spawn time, so this is
    /// serialised as JSON `null` rather than as an epoch timestamp, which
    /// would claim the template was last used in 1970.
    core::Optional<int64_t>  last_spawned_at_unix_ms;
    /// Rust passes `0` and `1` literally: no spawn counter exists, and a
    /// record *is* one build.
    int64_t                  spawn_count = 0;
    int32_t                  build_count = 1;
    /// Rust `build_record_envd_version` — `"unknown"` until the build commits,
    /// because the version is only known once a sandbox has actually run.
    std::string              envd_version;
    /// Rust `template_build_status`: the build's own status for a template
    /// record, and `Ready` for a sandbox-sourced one, which has no build.
    snapshot::TemplateBuildStatus build_status = snapshot::TemplateBuildStatus::Waiting;

    static TemplateModel FromRecord(const snapshot::SnapshotRecord& record);

    core::Json ToJson() const;
};

/// Rust `template_build_status`.
snapshot::TemplateBuildStatus TemplateBuildStatusOf(const snapshot::SnapshotRecord& record);

/// The wire spelling of a build status — lowercase.
///
/// Deliberately *not* `snapshot::TemplateBuildStatusToString`, which returns
/// `"Waiting"`: that is the record's own serialisation, and the generated API
/// model renames every variant to lowercase. Reusing the record spelling here
/// would put `"Waiting"` in an HTTP body whose schema declares `"waiting"`,
/// which a generated client rejects as an unknown variant.
const char* TemplateBuildStatusWireName(snapshot::TemplateBuildStatus status);

/// Rust generated `models::LogsSource` — which copy of a build's log to read.
///
/// The distinction survives the port even though only one of the two is
/// readable here. Upstream `temporary` is the live in-memory buffer of a build
/// that is still running, kept in `ApiImpl::build_logs`; there is no running
/// build in this port, so that registry has nothing to hold and the buffer is
/// always absent. That makes `temporary` an empty answer rather than a wrong
/// one -- and collapsing the two spellings would instead hand a caller the
/// *persisted* log of a previous build when they asked for the live one.
enum class LogsSource {
    Temporary,
    Persistent,
};

/// Rust's two sets of log query parameters, which overlap:
/// `TemplatesTemplateIdBuildsBuildIdStatusGetQueryParams` (`logsOffset`,
/// `limit`, `level`) and `...LogsGetQueryParams` (`cursor`, `limit`, `level`,
/// `direction`, `source`). One struct because `query_build_logs` upstream is
/// one function taking all of them.
struct BuildLogQuery {
    /// Rust `logsOffset` — an index *after* level filtering, which is why it
    /// is applied last rather than as a slice of the stored document.
    std::size_t offset = 0;
    /// Rust `cursor` — an inclusive timestamp in unix milliseconds. Absent
    /// means "from whichever end `direction` starts at".
    core::Optional<int64_t> cursor;
    /// Rust `limit.unwrap_or(100)`.
    std::size_t limit = 100;
    /// Rust `direction == Some(LogsDirectionBackward)`.
    bool backward = false;
    /// Rust `level` — the *minimum* severity, so `warn` also returns errors.
    core::Optional<core::LogRecordLevel> level;
    core::Optional<LogsSource>           source;

    /// Parses the status route's three parameters from raw query strings.
    ///
    /// Each taken as the raw text so an absent parameter and an empty one stay
    /// distinguishable, and a malformed one is reported rather than defaulted:
    /// `limit=abc` silently becoming 100 would answer 200 with a page the
    /// caller did not ask for.
    static core::Expected<BuildLogQuery, core::AnyError> ForStatus(
        const std::string& logs_offset, const std::string& limit, const std::string& level);

    /// Parses the logs route's five.
    static core::Expected<BuildLogQuery, core::AnyError> ForLogs(
        const std::string& cursor, const std::string& limit, const std::string& level,
        const std::string& direction, const std::string& source);
};

/// Rust generated `models::BuildStatusReason`.
struct BuildStatusReasonModel {
    std::string                 message;
    core::Optional<std::string> step;
    /// Rust `Option<Vec<models::BuildLogEntry>>`, filled only when the reason
    /// names a step. Absent and empty are different on the wire: absent means
    /// nobody looked, empty means the step logged nothing above `warn`.
    core::Optional<std::vector<tpl::BuildLogEntry> > log_entries;

    core::Json ToJson() const;
};

/// Rust generated `models::TemplateBuildInfo` — the body of
/// `GET /templates/{templateID}/builds/{buildID}/status`.
///
/// Named `...Model` for the same reason `TemplateModel` is: `snapshot::`
/// already has a `TemplateBuildInfo`, which is the *record's* build block
/// (status, timestamps, error reason) rather than this HTTP body. The two are
/// one field of each other, and upstream only gets away with the collision
/// because the generated models live in their own crate.
///
/// `imageDigest` is not declared. Upstream sets it only on the imageOnly
/// branch of `From<BuildRecord>`, from an `ImageBuildRecord`; `imageOnly`
/// requests are refused here, so no such record can exist and the field would
/// be one nothing ever writes.
struct TemplateBuildInfoModel {
    /// Rust `logs: Vec<String>` — "Deprecated; always empty, use logEntries".
    /// Still written, because the schema makes it required: a client reading
    /// `logs.length` must not find the key missing.
    std::vector<std::string>        logs;
    std::vector<tpl::BuildLogEntry> log_entries;
    std::string                     template_id;
    /// Rust reports `record.id` for both, as `models::Template` does.
    std::string                     build_id;
    snapshot::TemplateBuildStatus   status = snapshot::TemplateBuildStatus::Waiting;
    core::Optional<BuildStatusReasonModel> reason;

    /// Rust `impl From<SnapshotRecord> for models::TemplateBuildInfo`.
    static TemplateBuildInfoModel FromRecord(const snapshot::SnapshotRecord& record);

    core::Json ToJson() const;
};

/// Rust enum `TemplateBuildStartBaseSource`.
class TemplateBuildStartBaseSource {
 public:
    enum Kind {
        DefaultImage,
        Image,
        Template,
    };

    static TemplateBuildStartBaseSource MakeDefaultImage();
    static TemplateBuildStartBaseSource MakeImage(const std::string& image);
    static TemplateBuildStartBaseSource MakeTemplate(const snapshot::SnapshotAlias& alias);

    Kind kind() const { return kind_; }
    /// Valid only when `kind() == Image`.
    const std::string& image() const { return image_; }
    /// Valid only when `kind() == Template`.
    const snapshot::SnapshotAlias& alias() const { return *alias_; }

    bool operator==(const TemplateBuildStartBaseSource& o) const;
    bool operator!=(const TemplateBuildStartBaseSource& o) const { return !(*this == o); }

 private:
    TemplateBuildStartBaseSource() : kind_(DefaultImage) {}

    Kind                                     kind_;
    std::string                              image_;
    core::Optional<snapshot::SnapshotAlias>  alias_;
};

/// Rust `resolve_resources` — falls back to the machine defaults from the
/// global config, then rejects a zero in either dimension.
///
/// `disk_size_mib` is deliberately left at 0: a template build sizes its disk
/// from the image, not from the request.
core::Expected<sandbox::SandboxResources, ApiError>
    ResolveTemplateResources(const TemplateBuildRequestV3& body);

/// Rust `template_build_record_from_v3_request`.
core::Expected<snapshot::SnapshotRecord, ApiError>
    TemplateBuildRecordFromV3Request(const TemplateBuildRequestV3& body,
                                     const core::SnapshotId& id, const std::string& alias);

/// Rust `template_build_spec_from_start_request`.
core::Expected<tpl::TemplateBuildSpec, ApiError>
    TemplateBuildSpecFromStartRequest(const TemplateBuildStartRequest& body,
                                      const core::Optional<snapshot::SnapshotAlias>& alias,
                                      const sandbox::SandboxResources& resources);

/// Rust `template_build_start_base_source` — `fromImage` and `fromTemplate`
/// are mutually exclusive; neither means the configured default image.
core::Expected<TemplateBuildStartBaseSource, ApiError>
    TemplateBuildStartBaseSourceFrom(const TemplateBuildStartRequest& body);

/// Rust `apply_e2b_template_step`. Exposed for tests; the spec is mutated in
/// place, matching the C++ builder's reference-returning style.
core::Expected<core::Unit, ApiError> ApplyE2bTemplateStep(tpl::TemplateBuildSpec* spec,
                                                          const TemplateStepRequest& step);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_TEMPLATE_HELPERS_H_
