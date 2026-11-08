// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/overlaybd_snapshot.rs — the capture half.
//
// What these tests are really about is the terminal/recoverable split. The
// restack seals the live upper, and every step after that point leaves the
// running sandbox describing a stack it no longer has; a caller that read one
// of those failures as recoverable and resumed the guest would be running a VM
// whose block device silently lost the sealed writes. So for each failure mode
// below the assertion is not merely "it failed" but *which kind* of failure it
// reported.
#include "agentenv/sandbox/firecracker/overlaybd_restack.h"

#include <cstdio>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/config.h"
#include "microtest.h"

using namespace agentenv;                        // NOLINT
using namespace agentenv::sandbox;               // NOLINT
using namespace agentenv::sandbox::firecracker;  // NOLINT

namespace {

namespace obd = agentenv::storage::overlaybd;

// A scratch directory per test, removed on scope exit.
struct Scratch {
    std::string root;

    Scratch() {
        const core::Expected<std::string, std::string> made =
            core::fs::CreateTempDir("aenv-restack-");
        MT_EXPECT_TRUE(made.ok());
        // Canonicalized because `LoadImageConfig` rebases and normalizes the
        // paths it reads. `TMPDIR` with a trailing slash yields a root holding
        // `//`, and the comparisons below would then be against a spelling
        // nothing under test ever produces.
        const core::Expected<std::string, std::string> resolved =
            core::fs::Canonicalize(made.value());
        MT_EXPECT_TRUE(resolved.ok());
        root = resolved.value();
    }
    ~Scratch() { core::fs::RemoveDirAll(root); }

    std::string At(const std::string& leaf) const { return core::fs::Join(root, leaf); }

    void WriteFile(const std::string& path, const std::string& body) const {
        const core::Optional<std::string> parent = core::fs::Parent(path);
        if (parent) MT_EXPECT_TRUE(core::fs::CreateDirAll(*parent).ok());
        MT_EXPECT_TRUE(core::fs::Write(path, body).ok());
    }

   private:
    Scratch(const Scratch&);
    Scratch& operator=(const Scratch&);
};

obd::LayerConfig Lower(const std::string& file) {
    obd::LayerConfig layer;
    layer.file = file;
    return layer;
}

// A live runtime config: some lowers plus a writable upper, which is what makes
// it restackable.
void WriteLiveConfig(const Scratch& scratch, const std::string& path,
                     const std::vector<obd::LayerConfig>& lowers,
                     const std::string& upper_data) {
    obd::ImageConfig config;
    config.lowers = lowers;
    // Both halves or neither: `ValidateImageConfig` rejects a partial upper, so
    // an index without data would fail the *load* and never reach the check
    // under test.
    if (!upper_data.empty()) {
        config.upper.data = upper_data;
        config.upper.index = upper_data + ".index";
    }
    scratch.WriteFile(path, obd::ImageConfigToJsonPretty(config));
}

// Parsed as written, *not* through `LoadImageConfig`.
//
// `LoadImageConfig` rebases every local path against the config's directory,
// which turns `./result.txt` into an absolute path. These tests assert on the
// bytes that were written, so going through the loader would check the
// loader's rewriting rather than what the code under test produced.
obd::ImageConfig LoadConfig(const std::string& path) {
    const core::Expected<std::string, std::string> text = core::fs::ReadToString(path);
    MT_EXPECT_TRUE(text.ok());
    const core::Expected<obd::ImageConfig, std::string> parsed = obd::ParseImageConfig(text.value());
    MT_EXPECT_TRUE(parsed.ok());
    return parsed.value();
}

// Records what the daemon was asked to do and replays a scripted answer.
struct FakeRestacker {
    std::vector<std::string> calls;
    // When set, the seal "succeeds" and a file of this size is produced.
    bool produce_layer = true;
    uint64_t layer_bytes = 128;
    // What to report back. Absent descriptor models an older daemon.
    bool report_descriptor = true;
    uint64_t descriptor_size = 128;
    std::string descriptor_digest = "sha256:abcd";
    // When set, the seal fails with this classification instead.
    bool fail = false;
    bool fail_terminal = false;
    // Removed once the seal has "happened", to drive the post-seal failures.
    std::string unlink_after_seal;

    SnapshotRestacker Fn() {
        FakeRestacker* self = this;
        return [self](const std::string& output_layer_path)
                   -> SandboxCaptureResult<core::Optional<RestackedLayer> > {
            self->calls.push_back(output_layer_path);
            if (self->fail) {
                return core::make_unexpected(
                    self->fail_terminal ? SandboxCaptureError::Terminal("daemon refused")
                                        : SandboxCaptureError::Recoverable("daemon refused"));
            }
            if (self->produce_layer) {
                // The daemon **appends** into its output file. Modelled that
                // way on purpose: a fake that truncated would make a missing
                // stale-file cleanup invisible, since the leftover bytes would
                // be overwritten rather than prepended to the sealed layer.
                uint64_t existing = 0;
                const core::Expected<uint64_t, std::string> had =
                    core::fs::FileSize(output_layer_path);
                if (had.ok()) existing = had.value();
                MT_EXPECT_TRUE(
                    core::fs::CreateSizedFile(output_layer_path, existing + self->layer_bytes)
                        .ok());
            }
            if (!self->unlink_after_seal.empty()) {
                MT_EXPECT_TRUE(core::fs::RemoveFile(self->unlink_after_seal).ok());
            }
            if (!self->report_descriptor) return core::Optional<RestackedLayer>();
            RestackedLayer descriptor;
            descriptor.digest = self->descriptor_digest;
            descriptor.size   = self->descriptor_size;
            return core::Optional<RestackedLayer>(descriptor);
        };
    }
};

// Adoption and compaction are injected; these record use without needing real
// overlaybd layers.
struct FakeStager {
    std::vector<std::string> adopted;
    std::vector<std::string> compacted;
    bool compact_fails = false;

    LayerAdopter Adopt() {
        FakeStager* self = this;
        return [self](const std::string& source,
                      const std::string& destination) -> core::Expected<core::Unit, std::string> {
            self->adopted.push_back(source + " -> " + destination);
            const core::Optional<std::string> parent = core::fs::Parent(destination);
            if (parent) {
                const core::Expected<core::Unit, std::string> made =
                    core::fs::CreateDirAll(*parent);
                if (!made.ok()) return core::make_unexpected(made.error());
            }
            return core::fs::Write(destination, "adopted");
        };
    }

    LayerCompactor Compact() {
        FakeStager* self = this;
        return [self](const std::vector<obd::LayerConfig>& layers, const std::string& output_path)
                   -> core::Expected<core::Optional<std::string>, std::string> {
            self->compacted.push_back(output_path);
            if (self->compact_fails) {
                return core::make_unexpected(std::string("compaction refused"));
            }
            if (layers.empty()) return core::Optional<std::string>();
            const core::Expected<core::Unit, std::string> written =
                core::fs::Write(output_path, "compacted");
            if (!written.ok()) return core::make_unexpected(written.error());
            return core::Optional<std::string>(output_path);
        };
    }
};

// No runtime-owned roots: every lower counts as durable, so staging neither
// adopts nor compacts and the lower list passes through. That isolates the
// tests below from the budget logic, which has its own coverage.
std::vector<std::string> NoRuntimeRoots() { return std::vector<std::string>(); }

}  // namespace

// ---- RestackTargetUpperDataPath -------------------------------------------

MT_TEST(restack_target_upper_data_reads_the_live_upper) {
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));

    const core::Expected<std::string, std::string> resolved = RestackTargetUpperDataPath(live);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value(), scratch.At("live/upper.data"));
}

MT_TEST(restack_target_upper_data_refuses_a_read_only_stack) {
    // Without a writable upper there is nothing to seal. Returning some
    // plausible path instead would have the daemon seal a read-only stack.
    //
    // A lower is needed to make the config loadable at all: a stack with
    // neither lowers nor a writable upper describes no device.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower(scratch.At("layers/base.commit")));
    WriteLiveConfig(scratch, live, lowers, "");

    const core::Expected<std::string, std::string> resolved = RestackTargetUpperDataPath(live);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_TRUE(resolved.error().find("requires writable upper.data") != std::string::npos);
}

MT_TEST(restack_target_upper_data_reports_a_missing_config) {
    Scratch scratch;
    const core::Expected<std::string, std::string> resolved =
        RestackTargetUpperDataPath(scratch.At("absent/image.json"));
    MT_EXPECT_TRUE(!resolved.ok());
}

// ---- LocalLayerConfigWithDescriptor ---------------------------------------

MT_TEST(restack_local_layer_carries_the_descriptor_when_there_is_one) {
    RestackedLayer descriptor;
    descriptor.digest = "sha256:feed";
    descriptor.size   = 4096;

    const obd::LayerConfig with =
        LocalLayerConfigWithDescriptor("/layers/a.commit", core::Optional<RestackedLayer>(descriptor));
    MT_EXPECT_EQ(with.file, std::string("/layers/a.commit"));
    MT_EXPECT_EQ(with.digest, std::string("sha256:feed"));
    MT_EXPECT_EQ(with.size, static_cast<uint64_t>(4096));
}

MT_TEST(restack_local_layer_without_a_descriptor_claims_no_digest) {
    // A layer that only exists on this node has neither digest nor size.
    // Inventing them would make it look fetchable from the repository.
    const obd::LayerConfig without =
        LocalLayerConfigWithDescriptor("/layers/a.commit", core::Optional<RestackedLayer>());
    MT_EXPECT_EQ(without.file, std::string("/layers/a.commit"));
    MT_EXPECT_TRUE(without.digest.empty());
    MT_EXPECT_EQ(without.size, static_cast<uint64_t>(0));
}

// ---- CanonicalizedRuntimeOwnedRoots ---------------------------------------

MT_TEST(restack_runtime_owned_roots_resolve_both_paths) {
    Scratch scratch;
    MT_EXPECT_TRUE(core::fs::CreateDirAll(scratch.At("managed")).ok());
    MT_EXPECT_TRUE(core::fs::CreateDirAll(scratch.At("persisted")).ok());

    const std::vector<std::string> roots = CanonicalizedRuntimeOwnedRoots(
        scratch.At("managed/./"), scratch.At("persisted"));
    MT_EXPECT_EQ(roots.size(), static_cast<std::size_t>(2));
    // Canonicalized: the `./` is gone.
    MT_EXPECT_TRUE(roots[0].find("/./") == std::string::npos);
    MT_EXPECT_TRUE(roots[1].find("persisted") != std::string::npos);
}

MT_TEST(restack_runtime_owned_roots_keep_an_unresolvable_path_verbatim) {
    // The managed-snapshot base may not exist on a node that has not taken a
    // snapshot yet. Dropping it would make every lower look durable, and the
    // snapshot would then reference files the runtime deletes.
    const std::vector<std::string> roots =
        CanonicalizedRuntimeOwnedRoots("/nonexistent/managed", "/nonexistent/persisted");
    MT_EXPECT_EQ(roots.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(roots[0], std::string("/nonexistent/managed"));
    MT_EXPECT_EQ(roots[1], std::string("/nonexistent/persisted"));
}

// ---- LinkOrCopyRuntimeLayer -----------------------------------------------

MT_TEST(restack_adopting_a_layer_hard_links_it) {
    Scratch scratch;
    scratch.WriteFile(scratch.At("src/a.commit"), "layer bytes");

    MT_EXPECT_TRUE(
        LinkOrCopyRuntimeLayer(scratch.At("src/a.commit"), scratch.At("dst/0000/a.commit")).ok());
    const core::Expected<std::string, std::string> body =
        core::fs::ReadToString(scratch.At("dst/0000/a.commit"));
    MT_EXPECT_TRUE(body.ok());
    MT_EXPECT_EQ(body.value(), std::string("layer bytes"));
}

MT_TEST(restack_adopting_a_layer_onto_itself_is_a_no_op) {
    Scratch scratch;
    scratch.WriteFile(scratch.At("src/a.commit"), "layer bytes");

    MT_EXPECT_TRUE(
        LinkOrCopyRuntimeLayer(scratch.At("src/a.commit"), scratch.At("src/a.commit")).ok());
    // Still there: the unconditional remove-then-link would have deleted it.
    MT_EXPECT_TRUE(core::fs::Exists(scratch.At("src/a.commit")));
}

MT_TEST(restack_adopting_over_a_stale_destination_still_hard_links) {
    // A leftover from an earlier attempt must not make the adoption fail with
    // EEXIST, so the step stays idempotent across retries.
    //
    // Asserting the *bytes* would not pin this down: with the stale file left
    // in place the hard link fails with EEXIST and the `cp` fallback produces
    // exactly the same contents, just at the cost of a full copy of a layer
    // that can run to gigabytes. What distinguishes the two is sharing -- a
    // hard link is the same inode, so a later write through the source is
    // visible through the destination.
    Scratch scratch;
    scratch.WriteFile(scratch.At("src/a.commit"), "new bytes");
    scratch.WriteFile(scratch.At("dst/a.commit"), "stale bytes");

    MT_EXPECT_TRUE(
        LinkOrCopyRuntimeLayer(scratch.At("src/a.commit"), scratch.At("dst/a.commit")).ok());
    const core::Expected<std::string, std::string> body =
        core::fs::ReadToString(scratch.At("dst/a.commit"));
    MT_EXPECT_TRUE(body.ok());
    MT_EXPECT_EQ(body.value(), std::string("new bytes"));

    // `Write` truncates in place, keeping the inode, so this is only visible
    // through the destination if the two names share one.
    MT_EXPECT_TRUE(core::fs::Write(scratch.At("src/a.commit"), "written through source").ok());
    const core::Expected<std::string, std::string> shared =
        core::fs::ReadToString(scratch.At("dst/a.commit"));
    MT_EXPECT_TRUE(shared.ok());
    MT_EXPECT_EQ(shared.value(), std::string("written through source"));
}

// ---- StageOverlaybdSnapshotFromLiveRuntime --------------------------------

MT_TEST(restack_staging_drops_the_upper_and_points_at_a_local_result) {
    // A persisted snapshot is read-only. Carrying the live upper across would
    // have every resume from the snapshot share a writable file with the
    // source sandbox.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower(scratch.At("layers/base.commit")));
    WriteLiveConfig(scratch, live, lowers, scratch.At("live/upper.data"));

    FakeStager stager;
    const core::Expected<std::string, std::string> staged =
        StageOverlaybdSnapshotFromLiveRuntime(live, scratch.At("out"),
                                              core::Optional<std::string>(), NoRuntimeRoots(),
                                              stager.Compact(), stager.Adopt());
    MT_EXPECT_TRUE(staged.ok());
    MT_EXPECT_EQ(staged.value(), scratch.At("out/image.json"));

    const obd::ImageConfig written = LoadConfig(staged.value());
    MT_EXPECT_TRUE(written.upper.data.empty());
    MT_EXPECT_TRUE(written.upper.index.empty());
    MT_EXPECT_EQ(written.result_file, std::string("./result.txt"));
    MT_EXPECT_EQ(written.lowers.size(), static_cast<std::size_t>(1));
}

MT_TEST(restack_staging_adopts_the_sealed_layer_as_the_newest_lower) {
    Scratch scratch;
    const std::string sealed = scratch.At("out/snapshot.commit");
    scratch.WriteFile(sealed, "sealed");

    // The daemon has already appended the sealed layer to the live config,
    // which is the state staging expects to find.
    const std::string live = scratch.At("live/image.json");
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower(scratch.At("layers/base.commit")));
    lowers.push_back(Lower(sealed));
    WriteLiveConfig(scratch, live, lowers, scratch.At("live/upper.data"));

    FakeStager stager;
    const core::Expected<std::string, std::string> staged =
        StageOverlaybdSnapshotFromLiveRuntime(live, scratch.At("out"),
                                              core::Optional<std::string>(sealed),
                                              NoRuntimeRoots(), stager.Compact(),
                                              stager.Adopt());
    MT_EXPECT_TRUE(staged.ok());

    const obd::ImageConfig written = LoadConfig(staged.value());
    MT_EXPECT_EQ(written.lowers.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(written.lowers[1].file, sealed);
}

MT_TEST(restack_staging_refuses_a_live_config_whose_newest_lower_is_not_the_sealed_layer) {
    // The daemon having sealed the layer is what put it at the top of the
    // stack. A mismatch means this is not the config that was just restacked,
    // and inheriting the wrong layer would publish a stack that silently omits
    // the captured writes.
    Scratch scratch;
    const std::string sealed = scratch.At("out/snapshot.commit");
    scratch.WriteFile(sealed, "sealed");

    const std::string live = scratch.At("live/image.json");
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower(scratch.At("layers/someone-elses.commit")));
    WriteLiveConfig(scratch, live, lowers, scratch.At("live/upper.data"));

    FakeStager stager;
    const core::Expected<std::string, std::string> staged =
        StageOverlaybdSnapshotFromLiveRuntime(live, scratch.At("out"),
                                              core::Optional<std::string>(sealed),
                                              NoRuntimeRoots(), stager.Compact(),
                                              stager.Adopt());
    MT_EXPECT_TRUE(!staged.ok());
    MT_EXPECT_TRUE(staged.error().find("expected latest lower") != std::string::npos);
}

MT_TEST(restack_staging_refuses_an_empty_stack_when_a_layer_was_sealed) {
    Scratch scratch;
    const std::string sealed = scratch.At("out/snapshot.commit");
    scratch.WriteFile(sealed, "sealed");

    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));

    FakeStager stager;
    const core::Expected<std::string, std::string> staged =
        StageOverlaybdSnapshotFromLiveRuntime(live, scratch.At("out"),
                                              core::Optional<std::string>(sealed),
                                              NoRuntimeRoots(), stager.Compact(),
                                              stager.Adopt());
    MT_EXPECT_TRUE(!staged.ok());
    MT_EXPECT_TRUE(staged.error().find("newly claimed sealed upper") != std::string::npos);
}

MT_TEST(restack_staging_writes_atomically_over_an_existing_config) {
    // The staged path may already hold a config from a previous attempt.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("out/image.json"), "{\"stale\": true}");

    FakeStager stager;
    const core::Expected<std::string, std::string> staged =
        StageOverlaybdSnapshotFromLiveRuntime(live, scratch.At("out"),
                                              core::Optional<std::string>(), NoRuntimeRoots(),
                                              stager.Compact(), stager.Adopt());
    MT_EXPECT_TRUE(staged.ok());
    const obd::ImageConfig written = LoadConfig(staged.value());
    MT_EXPECT_EQ(written.result_file, std::string("./result.txt"));
}

// ---- RestackSnapshotOverlaybdDevice: the read-only path --------------------

MT_TEST(restack_a_read_only_device_never_seals) {
    // There is no writable upper, so the daemon must not be asked to seal
    // anything: a seal on a read-only stack is the one call that could damage
    // a sandbox this operation is supposed to leave untouched.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower(scratch.At("layers/base.commit")));
    WriteLiveConfig(scratch, live, lowers, "");

    FakeRestacker restacker;
    FakeStager stager;
    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), true, live, scratch.At("out"), "snapshot.commit", NoRuntimeRoots(),
        stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_TRUE(restacker.calls.empty());
    MT_EXPECT_EQ(out.value(), scratch.At("out/image.json"));
}

MT_TEST(restack_a_read_only_device_reports_a_staging_failure_as_recoverable) {
    // Nothing was sealed, so the sandbox is exactly as it was and the caller
    // may keep running it. Reporting terminal here would condemn a healthy
    // sandbox over a failed snapshot.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower(scratch.At("layers/runtime.commit")));
    WriteLiveConfig(scratch, live, lowers, "");

    FakeRestacker restacker;
    FakeStager stager;
    stager.compact_fails = true;

    // One runtime-owned root plus a budget of 1 forces compaction, which fails.
    std::vector<std::string> roots;
    roots.push_back(scratch.At("layers"));
    std::vector<obd::LayerConfig> many;
    for (int i = 0; i < 40; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "layers/runtime-%d.commit", i);
        many.push_back(Lower(scratch.At(name)));
    }
    WriteLiveConfig(scratch, live, many, "");

    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), true, live, scratch.At("out"), "snapshot.commit", roots,
        stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(!out.error().IsTerminal());
}

// ---- RestackSnapshotOverlaybdDevice: the writable path --------------------

MT_TEST(restack_a_writable_device_seals_into_the_snapshot_dir) {
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    FakeStager stager;
    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), false, live, scratch.At("out"), "snapshot.commit", NoRuntimeRoots(),
        stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(restacker.calls.size(), static_cast<std::size_t>(1));
    // Same filesystem here, so the daemon sealed straight into the snapshot
    // directory rather than beside the live upper.
    MT_EXPECT_EQ(restacker.calls[0], scratch.At("out/snapshot.commit"));
}

MT_TEST(restack_appends_the_sealed_layer_to_the_live_config) {
    // This is what makes the on-disk live config agree with the device again:
    // the daemon reopened a fresh empty upper above the sealed layer, so
    // without the append a later resume would lose every write it holds.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower(scratch.At("layers/base.commit")));
    WriteLiveConfig(scratch, live, lowers, scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    FakeStager stager;
    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), false, live, scratch.At("out"), "snapshot.commit", NoRuntimeRoots(),
        stager.Compact(), stager.Adopt());
    MT_EXPECT_TRUE(out.ok());

    const obd::ImageConfig after = LoadConfig(live);
    MT_EXPECT_EQ(after.lowers.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(after.lowers[1].file, scratch.At("out/snapshot.commit"));
    // The descriptor the daemon reported travels with it, so the layer can be
    // matched against a published blob later.
    MT_EXPECT_EQ(after.lowers[1].digest, std::string("sha256:abcd"));
    MT_EXPECT_EQ(after.lowers[1].size, static_cast<uint64_t>(128));
}

MT_TEST(restack_keeps_the_live_upper_writable_after_the_seal) {
    // The append must not disturb the upper: the guest is still writing to it.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    FakeStager stager;
    MT_EXPECT_TRUE(RestackSnapshotOverlaybdDevice(restacker.Fn(), false, live, scratch.At("out"),
                                                  "snapshot.commit", NoRuntimeRoots(),
                                                  stager.Compact(), stager.Adopt())
                       .ok());

    const obd::ImageConfig after = LoadConfig(live);
    MT_EXPECT_EQ(after.upper.data, scratch.At("live/upper.data"));
}

MT_TEST(restack_clears_a_stale_layer_file_before_sealing) {
    // The daemon appends to its output file. A leftover from an earlier failed
    // attempt would be prepended to the new layer's contents.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");
    scratch.WriteFile(scratch.At("out/snapshot.commit"), "leftover from a failed attempt");

    FakeRestacker restacker;
    restacker.layer_bytes = 7;
    restacker.descriptor_size = 7;
    FakeStager stager;
    MT_EXPECT_TRUE(RestackSnapshotOverlaybdDevice(restacker.Fn(), false, live, scratch.At("out"),
                                                  "snapshot.commit", NoRuntimeRoots(),
                                                  stager.Compact(), stager.Adopt())
                       .ok());

    const core::Expected<uint64_t, std::string> size =
        core::fs::FileSize(scratch.At("out/snapshot.commit"));
    MT_EXPECT_TRUE(size.ok());
    MT_EXPECT_EQ(size.value(), static_cast<uint64_t>(7));
}

MT_TEST(restack_propagates_a_recoverable_seal_refusal) {
    // The daemon answered explicitly, which establishes it left the live
    // runtime intact; the sandbox is still safe to resume.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    restacker.fail = true;
    FakeStager stager;
    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), false, live, scratch.At("out"), "snapshot.commit", NoRuntimeRoots(),
        stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(!out.error().IsTerminal());
}

MT_TEST(restack_propagates_a_terminal_seal_failure) {
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    restacker.fail = true;
    restacker.fail_terminal = true;
    FakeStager stager;
    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), false, live, scratch.At("out"), "snapshot.commit", NoRuntimeRoots(),
        stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().IsTerminal());
}

MT_TEST(restack_a_descriptor_size_mismatch_is_terminal) {
    // The seal happened, so the sandbox is already compromised; and the file
    // on disk is not the layer the daemon says it sealed, so the snapshot
    // cannot be trusted either.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    restacker.layer_bytes = 64;
    restacker.descriptor_size = 128;  // disagrees with what was written
    FakeStager stager;
    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), false, live, scratch.At("out"), "snapshot.commit", NoRuntimeRoots(),
        stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().IsTerminal());
    MT_EXPECT_TRUE(out.error().Message().find("size mismatch") != std::string::npos);
}

MT_TEST(restack_a_size_mismatch_keeps_the_sealed_layer) {
    // Deliberate: the live device may still reference the layer, so deleting
    // it would break the running sandbox on top of the failed snapshot.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    restacker.layer_bytes = 64;
    restacker.descriptor_size = 128;
    FakeStager stager;
    MT_EXPECT_TRUE(!RestackSnapshotOverlaybdDevice(restacker.Fn(), false, live, scratch.At("out"),
                                                   "snapshot.commit", NoRuntimeRoots(),
                                                   stager.Compact(), stager.Adopt())
                        .ok());
    MT_EXPECT_TRUE(core::fs::Exists(scratch.At("out/snapshot.commit")));
}

MT_TEST(restack_accepts_a_daemon_that_reports_no_descriptor) {
    // An older daemon omits the field. Absent is a value the protocol defines,
    // so the size cross-check is skipped rather than the snapshot failed.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    restacker.report_descriptor = false;
    restacker.layer_bytes = 64;
    FakeStager stager;
    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), false, live, scratch.At("out"), "snapshot.commit", NoRuntimeRoots(),
        stager.Compact(), stager.Adopt());
    MT_EXPECT_TRUE(out.ok());

    // The appended lower then carries no digest, which is what a local-only
    // layer honestly is.
    const obd::ImageConfig after = LoadConfig(live);
    MT_EXPECT_EQ(after.lowers.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(after.lowers[0].digest.empty());
}

MT_TEST(restack_a_failed_live_config_rewrite_is_terminal) {
    // The append is what makes the live config agree with the device again.
    // If it cannot be written, the two disagree permanently: the device has a
    // sealed layer the config does not name, so resuming that sandbox would
    // silently lose every write the layer holds. The caller has to be told it
    // must not keep the sandbox, which is what terminal means.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    // The config disappears after the seal, so reloading it for the append
    // fails -- the same position a permission or I/O error would put us in.
    restacker.unlink_after_seal = live;
    FakeStager stager;

    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), false, live, scratch.At("out"), "snapshot.commit", NoRuntimeRoots(),
        stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().IsTerminal());
    MT_EXPECT_TRUE(out.error().Message().find("rewrite live runtime config") !=
                   std::string::npos);
}

MT_TEST(restack_a_staging_failure_after_a_seal_is_terminal) {
    // Rust `LiveOverlaybdSnapshotState::finish_staging`. The same staging
    // failure is recoverable on the read-only path and terminal here, because
    // here the live upper has already been sealed.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    std::vector<obd::LayerConfig> many;
    for (int i = 0; i < 40; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "layers/runtime-%d.commit", i);
        many.push_back(Lower(scratch.At(name)));
    }
    WriteLiveConfig(scratch, live, many, scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    std::vector<std::string> roots;
    roots.push_back(scratch.At("layers"));

    FakeRestacker restacker;
    FakeStager stager;
    stager.compact_fails = true;

    const SandboxCaptureResult<std::string> out = RestackSnapshotOverlaybdDevice(
        restacker.Fn(), false, live, scratch.At("out"), "snapshot.commit", roots,
        stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().IsTerminal());
}

// ---- RestackSnapshotOverlaybdRootfs ---------------------------------------

MT_TEST(restack_rootfs_uses_the_rootfs_subdir_and_snapshot_commit) {
    // The names are part of the on-disk layout a published snapshot is read
    // back through, so they are not free to drift.
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    WriteLiveConfig(scratch, live, std::vector<obd::LayerConfig>(), scratch.At("live/upper.data"));
    scratch.WriteFile(scratch.At("live/upper.data"), "upper");

    FakeRestacker restacker;
    FakeStager stager;
    const SandboxCaptureResult<std::string> out =
        RestackSnapshotOverlaybdRootfs(restacker.Fn(), false, live, scratch.At("snap"),
                                       NoRuntimeRoots(), stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value(), scratch.At("snap/rootfs/image.json"));
    MT_EXPECT_EQ(restacker.calls.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(restacker.calls[0], scratch.At("snap/rootfs/snapshot.commit"));
}

MT_TEST(restack_rootfs_passes_the_read_only_flag_through) {
    Scratch scratch;
    const std::string live = scratch.At("live/image.json");
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower(scratch.At("layers/base.commit")));
    WriteLiveConfig(scratch, live, lowers, "");

    FakeRestacker restacker;
    FakeStager stager;
    const SandboxCaptureResult<std::string> out =
        RestackSnapshotOverlaybdRootfs(restacker.Fn(), true, live, scratch.At("snap"),
                                       NoRuntimeRoots(), stager.Compact(), stager.Adopt());

    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_TRUE(restacker.calls.empty());
}

int main() { return microtest::RunAll(); }
