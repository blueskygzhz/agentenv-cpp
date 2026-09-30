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
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/types.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/template/build_spec.h"

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
};

/// Rust generated `models::TemplateBuildStartV2`.
struct TemplateBuildStartRequest {
    core::Optional<std::string> from_image;
    core::Optional<std::string> from_template;
    core::Optional<std::string> start_cmd;
    core::Optional<std::string> ready_cmd;
    core::Optional<std::vector<TemplateStepRequest> > steps;
};

/// Rust generated `models::TemplateBuildRequestV3`.
struct TemplateBuildRequestV3 {
    core::Optional<uint32_t>                 cpu_count;
    core::Optional<uint32_t>                 memory_mb;
    core::Optional<std::vector<std::string> > tags;
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
