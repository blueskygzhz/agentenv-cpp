// SPDX-License-Identifier: MIT
// Rust: src/template/builder.rs — `execute_and_publish`, `build_failure_reason`
// and the three entry points that feed them.
//
// Driven over the real OSS repository on an HTTP-level stub (the same fixture
// test_snapshot_publish.cc uses) rather than a recording fake, because the
// assertions worth making here are about what ends up in the *committed
// record* — the resolved context, the measured disk size, the fields
// deliberately left out. A fake that merely remembered the metadata would
// assert this port against itself.
//
// The build itself arrives as an injected functor. That is upstream's own
// seam: `execute_in_background(context, execute)` takes the executing closure
// as a parameter, and the only production caller passes
// `TemplateBuildRunner::new().execute`. Which is also why this half could be
// ported first — the sandbox-driving half still needs
// `SandboxBackend::capture_to_dir`.
#include "agentenv/template/builder.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/oss/repository.h"
#include "microtest.h"
#include "oss_client_fixture.h"

using namespace agentenv;                        // NOLINT
using namespace agentenv::tpl;                   // NOLINT
using namespace agentenv::snapshot;              // NOLINT
using namespace agentenv::snapshot::repository;  // NOLINT
using namespace agentenv::testing::oss_fixture;  // NOLINT

namespace {

const char* kId = "11111111-2222-3333-4444-555555555555";

core::SnapshotId Id(const char* text) {
    const core::Expected<core::SnapshotId, std::string> id = core::SnapshotId::Parse(text);
    MT_EXPECT_TRUE(id.ok());
    return id.ok() ? id.value() : core::SnapshotId();
}

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/aenv-tpl-publish-XXXXXX";
        const char* made = ::mkdtemp(tmpl);
        MT_EXPECT_TRUE(made != NULL);
        path = made == NULL ? std::string() : made;
    }
    ~TempDir() {
        if (path.empty()) return;
        const std::string cmd = "rm -rf '" + path + "'";
        (void)std::system(cmd.c_str());
    }
    std::string At(const std::string& name) const { return core::fs::Join(path, name); }
};

void WriteFile(const std::string& path, const std::string& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    MT_EXPECT_TRUE(file != NULL);
    if (file == NULL) return;
    if (!bytes.empty()) {
        MT_EXPECT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size());
    }
    std::fclose(file);
}

std::string LocalLowerConfig(const std::string& file, const std::string& digest, uint64_t size) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(size));
    return "{\"repoBlobUrl\":\"\",\"lowers\":[{\"file\":\"" + file + "\",\"digest\":\"" + digest +
           "\",\"size\":" + buf + "}],\"upper\":{},\"resultFile\":\"\",\"download\":{}}";
}

/// Layer uploads go through the multipart uploader, not the HTTP transport; a
/// null one reports `Unsupported` and every publish here would fail for the
/// wrong reason.
struct StubUploader : public oss::ObjectUploader {
    StubObjectServer* server;
    explicit StubUploader(StubObjectServer* s) : server(s) {}

    RepositoryResult<core::Unit> UploadFile(const std::string& object_url,
                                            const std::string& path) {
        const std::string marker = "/agentenv/";
        const std::string::size_type at = object_url.find(marker);
        if (at == std::string::npos) {
            return core::make_unexpected(
                RepositoryError::Backend("stub upload", "unexpected object url " + object_url));
        }
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (file == NULL) {
            return core::make_unexpected(RepositoryError::Backend("stub upload", path));
        }
        std::string bytes;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            bytes.append(buffer, read);
        }
        std::fclose(file);
        server->bodies[object_url.substr(at + 1)] = bytes;
        return core::Unit();
    }
};

struct Fixture {
    StubObjectServer server;
    std::unique_ptr<oss::OssSnapshotRepository> repository;
    TempDir dir;

    Fixture() {
        const RepositoryResult<oss::OssClient*> client =
            oss::OssClient::NewWithTransport(Normalized(GoodConfig()), server.Fn());
        MT_EXPECT_TRUE(client.ok());
        repository.reset(new oss::OssSnapshotRepository(
            std::shared_ptr<oss::OssClient>(client.ok() ? client.value() : 0),
            std::shared_ptr<oss::ObjectUploader>(new StubUploader(&server))));
    }

    SnapshotRepository& Repo() { return *repository; }

    /// A manifest whose rootfs, memory and VM state all exist on disk.
    /// `rootfs_virtual_size` is the knob the disk-size rounding is asserted on.
    sandbox::SandboxSnapshotManifest Manifest(uint64_t rootfs_virtual_size) {
        const std::string rootfs_lower = dir.At("rootfs.commit");
        WriteFile(rootfs_lower, "rootfs-bytes");
        const std::string rootfs_config = dir.At("rootfs-image.json");
        WriteFile(rootfs_config, LocalLowerConfig(rootfs_lower, "sha256:rootfs", 12));

        const std::string memory_lower = dir.At("memory.commit");
        WriteFile(memory_lower, "memory-bytes");
        const std::string memory_config = dir.At("mem-image.json");
        WriteFile(memory_config, LocalLowerConfig(memory_lower, "sha256:memory", 12));

        const std::string vm_state = dir.At("vm_state.bin");
        WriteFile(vm_state, "vm-state");

        const core::Expected<sandbox::SandboxSnapshotManifest, std::string> manifest =
            sandbox::SandboxSnapshotManifest::New(sandbox::kFirecrackerBackend, vm_state,
                                                  memory_config, 1 << 20, rootfs_config,
                                                  rootfs_virtual_size,
                                                  std::vector<sandbox::ExtraDrive>());
        MT_EXPECT_TRUE(manifest.ok());
        return manifest.ok() ? manifest.value() : sandbox::SandboxSnapshotManifest();
    }

    core::Optional<SnapshotRecord> Stored() {
        const std::string key = DocumentKey::SnapshotRecord(Id(kId)).Path();
        if (server.bodies.find(key) == server.bodies.end()) {
            return core::Optional<SnapshotRecord>();
        }
        const core::Expected<core::Json, core::AnyError> json =
            core::Json::Parse(server.bodies[key]);
        MT_EXPECT_TRUE(json.ok());
        const core::Expected<SnapshotRecord, std::string> record =
            SnapshotRecord::FromJson(json.value());
        return record.ok() ? core::Optional<SnapshotRecord>(record.value())
                           : core::Optional<SnapshotRecord>();
    }
};

/// A context as `prepare_fresh_context` would have produced it, with the
/// resources left carrying `disk_size_mib = 0` — which is the state the build
/// is supposed to fill in.
TemplateBuildContext Context() {
    TemplateBuildContext context;
    context.build_snapshot_id = Id(kId);
    context.initial_context.workdir = "/start";
    context.resources = TemplateBuilder::DefaultSandboxResources();
    MT_EXPECT_EQ(context.resources.disk_size_mib, static_cast<uint32_t>(0));
    context.virtualization_mode = core::VirtualizationMode::Kvm;
    return context;
}

/// What the build is supposed to have resolved by the time it returns.
TemplateBuildExecution Execution(Fixture* f, uint64_t rootfs_virtual_size) {
    TemplateBuildExecution execution;
    execution.manifest = f->Manifest(rootfs_virtual_size);
    execution.build_context.workdir = "/resolved";
    execution.build_context.user = core::Optional<std::string>("builder");
    execution.runtime_versions.envd_version = "envd-1.2.3";
    return execution;
}

TemplateBuildExecuteFn Returning(const TemplateBuildExecution& execution) {
    return [execution](const TemplateBuildContext&)
               -> core::Expected<TemplateBuildExecution, core::AnyError> { return execution; };
}

TemplateBuildExecuteFn Failing(const core::AnyError& error) {
    return [error](const TemplateBuildContext&)
               -> core::Expected<TemplateBuildExecution, core::AnyError> {
        return core::make_unexpected(error);
    };
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---- the happy path ---------------------------------------------------------

MT_TEST(publish_records_the_context_the_steps_left_not_the_one_they_started_from) {
    Fixture f;
    const TemplateBuildContext context = Context();
    const TemplateBuildExecution execution = Execution(&f, 1 << 20);

    const TemplatePipelineResult<SnapshotRecord> record = TemplateBuilder().ExecuteAndPublish(
        f.Repo(), context, "build snapshot artifacts from requested base", Returning(execution));
    MT_EXPECT_TRUE(record.ok());

    const core::Optional<SnapshotRecord> stored = f.Stored();
    MT_EXPECT_TRUE(stored.has_value());
    MT_EXPECT_TRUE(stored->committed.has_value());
    // The *resolved* context: WORKDIR/ENV/USER a step set must be what a
    // sandbox resumes with. Publishing `context.initial_context` instead would
    // silently discard every step's effect on the environment.
    MT_EXPECT_TRUE(stored->committed->context.workdir == "/resolved");
    MT_EXPECT_TRUE(stored->committed->context.user.has_value());
    MT_EXPECT_TRUE(*stored->committed->context.user == "builder");
    MT_EXPECT_TRUE(stored->committed->runtime_versions.envd_version == "envd-1.2.3");
    MT_EXPECT_TRUE(stored->committed->virtualization_mode == core::VirtualizationMode::Kvm);
}

MT_TEST(the_published_source_is_template_not_sandbox) {
    Fixture f;
    const TemplatePipelineResult<SnapshotRecord> record = TemplateBuilder().ExecuteAndPublish(
        f.Repo(), Context(), "op", Returning(Execution(&f, 1 << 20)));
    MT_EXPECT_TRUE(record.ok());
    MT_EXPECT_TRUE(record.value().source.kind == SnapshotSourceKind::Template);
}

MT_TEST(custom_extension_params_are_not_inherited_into_a_template) {
    // Upstream states this deliberately: the extension's config is a
    // per-sandbox user setting, not part of the built image's content.
    // Inheriting it would bake one caller's runtime parameters into a template
    // every other caller then gets.
    Fixture f;
    MT_EXPECT_TRUE(TemplateBuilder()
                       .ExecuteAndPublish(f.Repo(), Context(), "op",
                                          Returning(Execution(&f, 1 << 20)))
                       .ok());
    const core::Optional<SnapshotRecord> stored = f.Stored();
    MT_EXPECT_TRUE(stored.has_value());
    MT_EXPECT_TRUE(!stored->committed->custom_extension_params.has_value());
    // Nor any volume snapshot: a build VM gets no volumes attached.
    MT_EXPECT_TRUE(stored->committed->volume_snapshots.empty());
}

// ---- the measured disk size -------------------------------------------------

MT_TEST(disk_size_is_rounded_up_from_the_measured_rootfs) {
    const uint64_t kMib = 1024 * 1024;
    struct Case {
        uint64_t virtual_size;
        uint32_t expected_mib;
    };
    // Rounding **up** is the point: a rootfs of one byte over a mebibyte needs
    // two, and rounding down would publish a template whose declared disk
    // cannot hold the image it was built from.
    const Case cases[] = {
        {kMib, 1},
        {kMib + 1, 2},
        {2 * kMib - 1, 2},
        {4 * kMib, 4},
        {1, 1},
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Fixture f;
        const TemplatePipelineResult<SnapshotRecord> record = TemplateBuilder().ExecuteAndPublish(
            f.Repo(), Context(), "op", Returning(Execution(&f, cases[i].virtual_size)));
        MT_EXPECT_TRUE(record.ok());
        MT_EXPECT_EQ(record.value().resources.disk_size_mib, cases[i].expected_mib);
        // The other two resources come from the context, untouched.
        MT_EXPECT_EQ(record.value().resources.cpu_count,
                     TemplateBuilder::DefaultSandboxResources().cpu_count);
    }
}

MT_TEST(an_absurd_rootfs_size_saturates_rather_than_wrapping) {
    // A wrapped value would be a *small* disk size for an enormous image —
    // the one outcome worse than refusing the publish.
    Fixture f;
    const TemplatePipelineResult<SnapshotRecord> record = TemplateBuilder().ExecuteAndPublish(
        f.Repo(), Context(), "op", Returning(Execution(&f, 0xFFFFFFFFFFFFFFFFULL)));
    MT_EXPECT_TRUE(record.ok());
    MT_EXPECT_EQ(record.value().resources.disk_size_mib, static_cast<uint32_t>(0xFFFFFFFFU));
}

// ---- failure attribution ----------------------------------------------------

MT_TEST(a_step_failure_keeps_its_step_number_through_the_build_boundary) {
    // The whole reason `TemplateBuildFailure` is an `ErrorPayload`: the e2b
    // SDKs read `reason.step` as an integer index into their own stack traces,
    // so losing it across the erased-error boundary points the user at the
    // wrong entry.
    const TemplateBuildFailure failure =
        TemplateBuildFailure::WithStep("npm install exited 1", "3");
    const snapshot::TemplateBuildErrorReason reason =
        TemplateBuilder::BuildFailureReason(AnyErrorFrom(failure));
    MT_EXPECT_TRUE(reason.message == "npm install exited 1");
    MT_EXPECT_TRUE(reason.step.has_value());
    MT_EXPECT_TRUE(*reason.step == "3");

    // And it survives having context layered on top, which is what
    // `execute_and_publish` does before reporting.
    const snapshot::TemplateBuildErrorReason wrapped = TemplateBuilder::BuildFailureReason(
        AnyErrorFrom(failure).context("build snapshot artifacts from requested base"));
    MT_EXPECT_TRUE(wrapped.step.has_value());
    MT_EXPECT_TRUE(*wrapped.step == "3");
}

MT_TEST(a_sandbox_lifecycle_failure_gets_the_generic_reason) {
    // `start`, `capture_to_dir` and `stop` carry no payload: there is no step
    // to blame, and inventing one would be worse than the generic text.
    const snapshot::TemplateBuildErrorReason reason =
        TemplateBuilder::BuildFailureReason(core::err("firecracker exited before boot"));
    MT_EXPECT_TRUE(reason.message == "template build failed while running the build sandbox");
    MT_EXPECT_TRUE(!reason.step.has_value());
}

MT_TEST(a_failed_build_publishes_nothing_and_names_the_operation) {
    Fixture f;
    const TemplateBuildFailure failure = TemplateBuildFailure::WithStep("RUN false", "2");
    const TemplatePipelineResult<SnapshotRecord> record = TemplateBuilder().ExecuteAndPublish(
        f.Repo(), Context(), "build snapshot artifacts from requested base",
        Failing(AnyErrorFrom(failure)));

    MT_EXPECT_TRUE(!record.ok());
    MT_EXPECT_TRUE(record.error().kind == TemplatePipelineErrorKind::Build);
    // The attributed reason, not the generic one.
    MT_EXPECT_TRUE(record.error().build.system_reason.message == "RUN false");
    MT_EXPECT_TRUE(record.error().build.step().has_value());
    MT_EXPECT_TRUE(*record.error().build.step() == "2");
    // The operation string says which entry point was taken, so an error read
    // in isolation still locates the call.
    MT_EXPECT_TRUE(record.error().build.source.has_value());
    MT_EXPECT_TRUE(Contains(*record.error().build.source,
                            "build snapshot artifacts from requested base"));
    MT_EXPECT_TRUE(Contains(*record.error().build.source, "RUN false"));

    // Nothing reached the repository: no record, and no blobs.
    MT_EXPECT_TRUE(!f.Stored().has_value());
    MT_EXPECT_TRUE(f.server.bodies.empty());
}

MT_TEST(a_repository_failure_is_reported_as_a_repository_error) {
    Fixture f;
    // Publishing twice: the second is refused by the committed gate, which is
    // a repository error and must not be relabelled as a build failure — the
    // build succeeded both times.
    MT_EXPECT_TRUE(TemplateBuilder()
                       .ExecuteAndPublish(f.Repo(), Context(), "op",
                                          Returning(Execution(&f, 1 << 20)))
                       .ok());
    const TemplatePipelineResult<SnapshotRecord> again = TemplateBuilder().ExecuteAndPublish(
        f.Repo(), Context(), "op", Returning(Execution(&f, 1 << 20)));
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(again.error().kind == TemplatePipelineErrorKind::Repository);
}

// ---- the entry points -------------------------------------------------------

MT_TEST(a_prepare_failure_never_runs_the_build) {
    Fixture f;
    bool ran = false;
    const TemplateBuildExecuteFn tripwire =
        [&ran](const TemplateBuildContext&)
            -> core::Expected<TemplateBuildExecution, core::AnyError> {
        ran = true;
        return core::make_unexpected(core::err("must not be reached"));
    };

    // An empty spec has no rootfs base, which `prepare_fresh_context` refuses.
    // A build that cannot be prepared must not boot a VM to discover that.
    const TemplatePipelineResult<SnapshotRecord> record = TemplateBuilder().BuildAndPublishWithId(
        f.Repo(), Id(kId), TemplateBuildSpec(), tripwire);
    MT_EXPECT_TRUE(!record.ok());
    MT_EXPECT_TRUE(record.error().kind == TemplatePipelineErrorKind::Build);
    MT_EXPECT_TRUE(!ran);
    MT_EXPECT_TRUE(f.server.bodies.empty());
}

MT_TEST(both_entry_points_route_through_prepare_before_the_build) {
    // `build_and_publish` differs from `..._with_id` only in generating the
    // id, so the behaviour to pin is that it reaches the same validation —
    // not the id, which is not observable from outside a successful prepare.
    Fixture f;
    bool ran = false;
    const TemplateBuildExecuteFn tripwire =
        [&ran](const TemplateBuildContext&)
            -> core::Expected<TemplateBuildExecution, core::AnyError> {
        ran = true;
        return core::make_unexpected(core::err("must not be reached"));
    };

    const TemplatePipelineResult<SnapshotRecord> fresh =
        TemplateBuilder().BuildAndPublish(f.Repo(), TemplateBuildSpec(), tripwire);
    MT_EXPECT_TRUE(!fresh.ok());
    MT_EXPECT_TRUE(fresh.error().kind == TemplatePipelineErrorKind::Build);

    // The snapshot-base entry point refuses an empty spec too: a build on a
    // committed snapshot still needs steps and a validated base.
    const TemplatePipelineResult<SnapshotRecord> from_snapshot =
        TemplateBuilder().BuildFromSnapshotAndPublish(
            f.Repo(), TemplateBuildSpec(), Id(kId),
            SnapshotRecord::TemplateWaiting(Id(kId), core::Optional<SnapshotAlias>(),
                                            sandbox::SandboxResources()),
            tripwire);
    MT_EXPECT_TRUE(!from_snapshot.ok());
    MT_EXPECT_TRUE(from_snapshot.error().kind == TemplatePipelineErrorKind::Build);

    MT_EXPECT_TRUE(!ran);
    MT_EXPECT_TRUE(f.server.bodies.empty());
}

MT_MAIN
