// SPDX-License-Identifier: MIT
// Rust: src/api/impls/template_helpers.rs `mod tests`, plus coverage for the
// step translation the upstream tests reach only through the handlers.
#include "agentenv/api/template_helpers.h"

#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT

namespace {

using agentenv::snapshot::SnapshotAlias;
using agentenv::tpl::TemplateBuildStep;
using agentenv::tpl::TemplateBuildStepKind;

TemplateStepRequest Step(const std::string& type) {
    TemplateStepRequest step;
    step.type = type;
    return step;
}

TemplateStepRequest StepWith(const std::string& type, const std::string& a0) {
    TemplateStepRequest step;
    step.type = type;
    std::vector<std::string> args;
    args.push_back(a0);
    step.args = args;
    return step;
}

TemplateStepRequest StepWith(const std::string& type, const std::string& a0,
                             const std::string& a1) {
    TemplateStepRequest step;
    step.type = type;
    std::vector<std::string> args;
    args.push_back(a0);
    args.push_back(a1);
    step.args = args;
    return step;
}

/// Applies one step to a fresh spec.
core::Expected<core::Unit, ApiError> Apply(tpl::TemplateBuildSpec* spec,
                                          const TemplateStepRequest& step) {
    return ApplyE2bTemplateStep(spec, step);
}

SnapshotAlias Alias(const std::string& value) {
    const core::Expected<SnapshotAlias, std::string> parsed = SnapshotAlias::Parse(value);
    MT_EXPECT_TRUE(parsed.ok());
    return parsed.value();
}

}  // namespace

// ---- base source (Rust's own tests) ----------------------------------------

MT_TEST(template_start_base_source_defaults_when_not_specified) {
    const TemplateBuildStartRequest body;
    const core::Expected<TemplateBuildStartBaseSource, ApiError> source =
        TemplateBuildStartBaseSourceFrom(body);
    MT_EXPECT_TRUE(source.ok());
    MT_EXPECT_TRUE(source.value() == TemplateBuildStartBaseSource::MakeDefaultImage());
}

MT_TEST(template_start_base_source_rejects_image_and_template_together) {
    TemplateBuildStartRequest body;
    body.from_image    = std::string("ubuntu:24.04");
    body.from_template = std::string("base-template");

    const core::Expected<TemplateBuildStartBaseSource, ApiError> source =
        TemplateBuildStartBaseSourceFrom(body);
    MT_EXPECT_TRUE(!source.ok());
    MT_EXPECT_EQ(source.error().code, 400);
    MT_EXPECT_EQ(source.error().message,
                 std::string("cannot specify both fromImage and fromTemplate"));
}

MT_TEST(template_start_base_source_parses_template_alias) {
    TemplateBuildStartRequest body;
    body.from_template = std::string("base-template");

    const core::Expected<TemplateBuildStartBaseSource, ApiError> source =
        TemplateBuildStartBaseSourceFrom(body);
    MT_EXPECT_TRUE(source.ok());
    MT_EXPECT_TRUE(source.value() ==
                   TemplateBuildStartBaseSource::MakeTemplate(Alias("base-template")));
}

MT_TEST(template_start_base_source_keeps_the_image_reference_verbatim) {
    TemplateBuildStartRequest body;
    // An image reference is not an alias: the tag must survive.
    body.from_image = std::string("ubuntu:24.04");

    const core::Expected<TemplateBuildStartBaseSource, ApiError> source =
        TemplateBuildStartBaseSourceFrom(body);
    MT_EXPECT_TRUE(source.ok());
    MT_EXPECT_TRUE(source.value().kind() == TemplateBuildStartBaseSource::Image);
    MT_EXPECT_EQ(source.value().image(), std::string("ubuntu:24.04"));
}

MT_TEST(template_start_base_source_treats_blank_as_absent) {
    TemplateBuildStartRequest body;
    body.from_image    = std::string("   ");
    body.from_template = std::string("\t\n");
    // Both blank, so this is not the "both specified" conflict — it is the
    // default-image case.
    const core::Expected<TemplateBuildStartBaseSource, ApiError> source =
        TemplateBuildStartBaseSourceFrom(body);
    MT_EXPECT_TRUE(source.ok());
    MT_EXPECT_TRUE(source.value().kind() == TemplateBuildStartBaseSource::DefaultImage);
}

MT_TEST(template_start_base_source_trims_the_reference) {
    TemplateBuildStartRequest body;
    body.from_template = std::string("  base-template  ");
    const core::Expected<TemplateBuildStartBaseSource, ApiError> source =
        TemplateBuildStartBaseSourceFrom(body);
    MT_EXPECT_TRUE(source.ok());
    MT_EXPECT_EQ(source.value().alias().ToString(), std::string("base-template"));
}

MT_TEST(template_start_base_source_rejects_invalid_template_alias) {
    TemplateBuildStartRequest body;
    body.from_template = std::string("bad/alias");
    const core::Expected<TemplateBuildStartBaseSource, ApiError> source =
        TemplateBuildStartBaseSourceFrom(body);
    MT_EXPECT_TRUE(!source.ok());
    MT_EXPECT_EQ(source.error().code, 400);
}

// ---- step numbering (Rust's own test) --------------------------------------

MT_TEST(template_expanded_env_pairs_share_their_client_step_number) {
    TemplateBuildStartRequest body;
    std::vector<TemplateStepRequest> steps;

    TemplateStepRequest env;
    env.type = "ENV";
    std::vector<std::string> env_args;
    env_args.push_back("A");
    env_args.push_back("1");
    env_args.push_back("B");
    env_args.push_back("2");
    env.args = env_args;
    steps.push_back(env);
    steps.push_back(StepWith("RUN", "false"));
    body.steps = steps;

    const core::Expected<tpl::TemplateBuildSpec, ApiError> spec =
        TemplateBuildSpecFromStartRequest(body, core::Optional<SnapshotAlias>(),
                                          sandbox::SandboxResources());
    MT_EXPECT_TRUE(spec.ok());

    // One ENV request with two pairs expands to two internal steps; both must
    // report as client step 1, and the RUN as client step 2.
    const std::vector<TemplateBuildStep>& applied = spec.value().Steps();
    MT_EXPECT_EQ(applied.size(), static_cast<std::size_t>(3));
    MT_EXPECT_TRUE(applied[0].source_step.has_value());
    MT_EXPECT_EQ(*applied[0].source_step, static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(applied[1].source_step.has_value());
    MT_EXPECT_EQ(*applied[1].source_step, static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(applied[2].source_step.has_value());
    MT_EXPECT_EQ(*applied[2].source_step, static_cast<std::size_t>(2));
}

MT_TEST(template_spec_carries_alias_resources_and_startup_overrides) {
    TemplateBuildStartRequest body;
    body.start_cmd = std::string("/start.sh");
    body.ready_cmd = std::string("/ready.sh");

    sandbox::SandboxResources resources;
    resources.cpu_count  = 4;
    resources.memory_mib = 2048;

    const core::Expected<tpl::TemplateBuildSpec, ApiError> spec =
        TemplateBuildSpecFromStartRequest(
            body, core::Optional<SnapshotAlias>(Alias("my-template")), resources);
    MT_EXPECT_TRUE(spec.ok());
    MT_EXPECT_TRUE(spec.value().ResourcesRef().has_value());
    MT_EXPECT_EQ(spec.value().ResourcesRef()->cpu_count, static_cast<uint32_t>(4));
    MT_EXPECT_EQ(spec.value().ResourcesRef()->memory_mib, static_cast<uint32_t>(2048));
    // `disk_size_mib` stays 0: a template build sizes its disk from the image.
    MT_EXPECT_EQ(spec.value().ResourcesRef()->disk_size_mib, static_cast<uint32_t>(0));
    MT_EXPECT_TRUE(spec.value().StartCmdRef().has_value());
    MT_EXPECT_EQ(*spec.value().StartCmdRef(), std::string("/start.sh"));
    MT_EXPECT_TRUE(spec.value().ReadyCmdRef().has_value());
    MT_EXPECT_TRUE(spec.value().OverridesStartup());
}

MT_TEST(template_spec_with_no_steps_is_empty) {
    const TemplateBuildStartRequest body;
    const core::Expected<tpl::TemplateBuildSpec, ApiError> spec =
        TemplateBuildSpecFromStartRequest(body, core::Optional<SnapshotAlias>(),
                                          sandbox::SandboxResources());
    MT_EXPECT_TRUE(spec.ok());
    MT_EXPECT_EQ(spec.value().StepCount(), static_cast<std::size_t>(0));
}

MT_TEST(template_spec_propagates_a_step_failure) {
    TemplateBuildStartRequest body;
    std::vector<TemplateStepRequest> steps;
    steps.push_back(StepWith("RUN", "true"));
    steps.push_back(Step("RUN"));  // missing argument
    body.steps = steps;

    const core::Expected<tpl::TemplateBuildSpec, ApiError> spec =
        TemplateBuildSpecFromStartRequest(body, core::Optional<SnapshotAlias>(),
                                          sandbox::SandboxResources());
    MT_EXPECT_TRUE(!spec.ok());
    MT_EXPECT_EQ(spec.error().code, 400);
}

// ---- step translation ------------------------------------------------------

MT_TEST(template_step_run) {
    tpl::TemplateBuildSpec spec;
    MT_EXPECT_TRUE(Apply(&spec, StepWith("RUN", "echo hi")).ok());
    MT_EXPECT_EQ(spec.StepCount(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(spec.Steps()[0].kind == TemplateBuildStepKind::Run);
    MT_EXPECT_EQ(spec.Steps()[0].value, std::string("echo hi"));
}

MT_TEST(template_step_type_is_case_insensitive) {
    tpl::TemplateBuildSpec spec;
    MT_EXPECT_TRUE(Apply(&spec, StepWith("run", "echo hi")).ok());
    MT_EXPECT_TRUE(Apply(&spec, StepWith("WorkDir", "/srv")).ok());
    MT_EXPECT_EQ(spec.StepCount(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(spec.Steps()[0].kind == TemplateBuildStepKind::Run);
    MT_EXPECT_TRUE(spec.Steps()[1].kind == TemplateBuildStepKind::Workdir);
}

MT_TEST(template_step_arg_is_an_alias_for_env) {
    tpl::TemplateBuildSpec spec;
    MT_EXPECT_TRUE(Apply(&spec, StepWith("ARG", "KEY", "value")).ok());
    MT_EXPECT_EQ(spec.StepCount(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(spec.Steps()[0].kind == TemplateBuildStepKind::Env);
    MT_EXPECT_EQ(spec.Steps()[0].key, std::string("KEY"));
    MT_EXPECT_EQ(spec.Steps()[0].value, std::string("value"));
}

MT_TEST(template_step_env_trims_the_key_but_keeps_the_value) {
    tpl::TemplateBuildSpec spec;
    // A value's surrounding whitespace can be meaningful; a key's cannot.
    MT_EXPECT_TRUE(Apply(&spec, StepWith("ENV", "  KEY  ", "  value  ")).ok());
    MT_EXPECT_EQ(spec.Steps()[0].key, std::string("KEY"));
    MT_EXPECT_EQ(spec.Steps()[0].value, std::string("  value  "));
}

MT_TEST(template_step_env_rejects_odd_or_empty_args) {
    tpl::TemplateBuildSpec spec;
    const core::Expected<core::Unit, ApiError> empty = Apply(&spec, Step("ENV"));
    MT_EXPECT_TRUE(!empty.ok());
    MT_EXPECT_EQ(empty.error().code, 400);
    MT_EXPECT_TRUE(empty.error().message.find("key/value") != std::string::npos);
    // The message quotes the original spelling, not the upper-cased one.
    MT_EXPECT_TRUE(Apply(&spec, Step("env")).error().message.find("env ") == 0);

    MT_EXPECT_TRUE(!Apply(&spec, StepWith("ENV", "ONLY_KEY")).ok());
}

MT_TEST(template_step_env_rejects_a_blank_key) {
    tpl::TemplateBuildSpec spec;
    const core::Expected<core::Unit, ApiError> blank =
        Apply(&spec, StepWith("ENV", "   ", "value"));
    MT_EXPECT_TRUE(!blank.ok());
    MT_EXPECT_TRUE(blank.error().message.find("non-empty key") != std::string::npos);
}

MT_TEST(template_step_workdir_user_expose_volume) {
    tpl::TemplateBuildSpec spec;
    MT_EXPECT_TRUE(Apply(&spec, StepWith("WORKDIR", "/srv")).ok());
    MT_EXPECT_TRUE(Apply(&spec, StepWith("USER", "app")).ok());
    MT_EXPECT_TRUE(Apply(&spec, StepWith("EXPOSE", "8080")).ok());
    MT_EXPECT_TRUE(Apply(&spec, StepWith("VOLUME", "/data")).ok());

    MT_EXPECT_EQ(spec.StepCount(), static_cast<std::size_t>(4));
    MT_EXPECT_TRUE(spec.Steps()[0].kind == TemplateBuildStepKind::Workdir);
    MT_EXPECT_EQ(spec.Steps()[0].value, std::string("/srv"));
    MT_EXPECT_TRUE(spec.Steps()[1].kind == TemplateBuildStepKind::User);
    MT_EXPECT_TRUE(spec.Steps()[2].kind == TemplateBuildStepKind::ExposedPort);
    MT_EXPECT_TRUE(spec.Steps()[3].kind == TemplateBuildStepKind::Volume);
}

MT_TEST(template_single_argument_steps_reject_blank_or_missing) {
    tpl::TemplateBuildSpec spec;
    MT_EXPECT_TRUE(!Apply(&spec, Step("RUN")).ok());
    MT_EXPECT_TRUE(!Apply(&spec, StepWith("RUN", "   ")).ok());
    MT_EXPECT_TRUE(!Apply(&spec, StepWith("WORKDIR", "")).ok());
    MT_EXPECT_TRUE(!Apply(&spec, StepWith("USER", "\t")).ok());
    MT_EXPECT_TRUE(!Apply(&spec, StepWith("EXPOSE", " ")).ok());
    MT_EXPECT_TRUE(!Apply(&spec, StepWith("VOLUME", " ")).ok());
    // Nothing was appended by any of the rejected steps.
    MT_EXPECT_EQ(spec.StepCount(), static_cast<std::size_t>(0));
}

MT_TEST(template_step_label_requires_pairs) {
    tpl::TemplateBuildSpec spec;
    MT_EXPECT_TRUE(Apply(&spec, StepWith("LABEL", "k", "v")).ok());
    MT_EXPECT_TRUE(spec.Steps()[0].kind == TemplateBuildStepKind::Label);
    MT_EXPECT_EQ(spec.Steps()[0].key, std::string("k"));

    MT_EXPECT_TRUE(!Apply(&spec, Step("LABEL")).ok());
    MT_EXPECT_TRUE(!Apply(&spec, StepWith("LABEL", "only")).ok());
    const core::Expected<core::Unit, ApiError> blank =
        Apply(&spec, StepWith("LABEL", " ", "v"));
    MT_EXPECT_TRUE(!blank.ok());
    MT_EXPECT_TRUE(blank.error().message.find("non-empty key") != std::string::npos);
}

MT_TEST(template_step_rejects_files_hash) {
    tpl::TemplateBuildSpec spec;
    TemplateStepRequest step = StepWith("RUN", "echo hi");
    step.files_hash          = std::string("deadbeef");

    // A real hash means the client staged files for a COPY, which would
    // silently not happen if this were accepted.
    const core::Expected<core::Unit, ApiError> applied = Apply(&spec, step);
    MT_EXPECT_TRUE(!applied.ok());
    MT_EXPECT_EQ(applied.error().code, 400);
    MT_EXPECT_TRUE(applied.error().message.find("filesHash") != std::string::npos);
}

MT_TEST(template_step_blank_files_hash_is_ignored) {
    tpl::TemplateBuildSpec spec;
    TemplateStepRequest step = StepWith("RUN", "echo hi");
    step.files_hash          = std::string("   ");
    // A blank hash means "no upload", so the step proceeds.
    MT_EXPECT_TRUE(Apply(&spec, step).ok());
    MT_EXPECT_EQ(spec.StepCount(), static_cast<std::size_t>(1));
}

MT_TEST(template_step_rejects_copy_add_and_unknown) {
    tpl::TemplateBuildSpec spec;
    const core::Expected<core::Unit, ApiError> copy = Apply(&spec, StepWith("COPY", "a", "b"));
    MT_EXPECT_TRUE(!copy.ok());
    MT_EXPECT_TRUE(copy.error().message.find("not supported yet") != std::string::npos);
    MT_EXPECT_TRUE(!Apply(&spec, StepWith("ADD", "a", "b")).ok());

    const core::Expected<core::Unit, ApiError> unknown = Apply(&spec, Step("ENTRYPOINT"));
    MT_EXPECT_TRUE(!unknown.ok());
    MT_EXPECT_EQ(unknown.error().code, 400);
    MT_EXPECT_TRUE(unknown.error().message.find("ENTRYPOINT") != std::string::npos);
    MT_EXPECT_TRUE(unknown.error().message.find("is not supported") != std::string::npos);
}

// ---- resources -------------------------------------------------------------

MT_TEST(template_resources_use_request_values) {
    TemplateBuildRequestV3 body;
    body.cpu_count = static_cast<uint32_t>(8);
    body.memory_mb = static_cast<uint32_t>(4096);

    const core::Expected<sandbox::SandboxResources, ApiError> resources =
        ResolveTemplateResources(body);
    MT_EXPECT_TRUE(resources.ok());
    MT_EXPECT_EQ(resources.value().cpu_count, static_cast<uint32_t>(8));
    MT_EXPECT_EQ(resources.value().memory_mib, static_cast<uint32_t>(4096));
    MT_EXPECT_EQ(resources.value().disk_size_mib, static_cast<uint32_t>(0));
}

MT_TEST(template_resources_reject_zero) {
    TemplateBuildRequestV3 zero_cpu;
    zero_cpu.cpu_count = static_cast<uint32_t>(0);
    const core::Expected<sandbox::SandboxResources, ApiError> cpu =
        ResolveTemplateResources(zero_cpu);
    MT_EXPECT_TRUE(!cpu.ok());
    MT_EXPECT_EQ(cpu.error().code, 400);
    MT_EXPECT_EQ(cpu.error().message,
                 std::string("cpuCount and memoryMB must be greater than 0"));

    TemplateBuildRequestV3 zero_mem;
    zero_mem.memory_mb = static_cast<uint32_t>(0);
    MT_EXPECT_TRUE(!ResolveTemplateResources(zero_mem).ok());
}

MT_TEST(template_resources_fall_back_to_machine_defaults) {
    // An absent field takes the configured machine default rather than 0,
    // which the zero-check would otherwise reject.
    const TemplateBuildRequestV3 body;
    const core::Expected<sandbox::SandboxResources, ApiError> resources =
        ResolveTemplateResources(body);
    MT_EXPECT_TRUE(resources.ok());
    MT_EXPECT_TRUE(resources.value().cpu_count > 0);
    MT_EXPECT_TRUE(resources.value().memory_mib > 0);
}

// ---- v3 record -------------------------------------------------------------

MT_TEST(template_v3_record_is_waiting_with_alias_and_resources) {
    TemplateBuildRequestV3 body;
    body.cpu_count = static_cast<uint32_t>(2);
    body.memory_mb = static_cast<uint32_t>(1024);

    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        TemplateBuildRecordFromV3Request(body, core::SnapshotId::Fresh(), "my-template");
    MT_EXPECT_TRUE(record.ok());
    MT_EXPECT_TRUE(record.value().alias.has_value());
    MT_EXPECT_EQ(record.value().alias->ToString(), std::string("my-template"));
    MT_EXPECT_EQ(record.value().resources.cpu_count, static_cast<uint32_t>(2));
    // A waiting template has nothing committed yet.
    MT_EXPECT_TRUE(!record.value().committed.has_value());
}

MT_TEST(template_v3_record_rejects_a_tagged_name) {
    const TemplateBuildRequestV3 body;
    // A `name:tag` reference would silently build the untagged template.
    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        TemplateBuildRecordFromV3Request(body, core::SnapshotId::Fresh(), "my-template:v1");
    MT_EXPECT_TRUE(!record.ok());
    MT_EXPECT_EQ(record.error().code, 400);
    MT_EXPECT_TRUE(record.error().message.find("tags are not supported") != std::string::npos);
}

MT_TEST(template_v3_record_rejects_non_empty_tags) {
    TemplateBuildRequestV3 body;
    std::vector<std::string> tags;
    tags.push_back("v1");
    body.tags = tags;

    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        TemplateBuildRecordFromV3Request(body, core::SnapshotId::Fresh(), "my-template");
    MT_EXPECT_TRUE(!record.ok());
    MT_EXPECT_EQ(record.error().code, 400);

    // An *empty* tag list is accepted: it carries no unsupported request.
    TemplateBuildRequestV3 empty_tags;
    empty_tags.tags = std::vector<std::string>();
    MT_EXPECT_TRUE(
        TemplateBuildRecordFromV3Request(empty_tags, core::SnapshotId::Fresh(), "my-template")
            .ok());
}

MT_TEST(template_v3_record_rejects_an_invalid_alias) {
    const TemplateBuildRequestV3 body;
    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        TemplateBuildRecordFromV3Request(body, core::SnapshotId::Fresh(), "bad alias");
    MT_EXPECT_TRUE(!record.ok());
    MT_EXPECT_EQ(record.error().code, 400);
}

MT_TEST(template_v3_record_propagates_a_resource_failure) {
    TemplateBuildRequestV3 body;
    body.cpu_count = static_cast<uint32_t>(0);
    const core::Expected<snapshot::SnapshotRecord, ApiError> record =
        TemplateBuildRecordFromV3Request(body, core::SnapshotId::Fresh(), "my-template");
    MT_EXPECT_TRUE(!record.ok());
    MT_EXPECT_EQ(record.error().message,
                 std::string("cpuCount and memoryMB must be greater than 0"));
}

int main() { return microtest::RunAll(); }
