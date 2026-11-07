// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/source_image.rs — the `tests` module.
//
// What is being pinned is a *plan*: for each overlaybd lower, reuse a blob the
// source registry already holds, or upload one produced on this host. The
// publisher turns that plan into blob uploads and a manifest, so a lower
// mis-classified here does not fail here — it produces a manifest naming a
// blob nobody uploaded, and the image breaks later on a different machine.
// That is why every unmodelled shape is asserted to be *refused* rather than
// approximated.
#include "agentenv/snapshot/image_export/acr_source_image.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/overlaybd/sys.h"
#include "microtest.h"

using namespace agentenv;                             // NOLINT
using namespace agentenv::snapshot::image_export;     // NOLINT
using namespace agentenv::snapshot::repository;       // NOLINT

namespace obd = agentenv::storage::overlaybd;

namespace {

const char* const kSource = "https://registry.example/v2/ns/base/blobs";

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_acrsrc_XXXXXX";
    char* made = ::mkdtemp(tmpl);
    if (made == 0) {
        std::fprintf(stderr, "  FAIL: mkdtemp\n");
        std::exit(1);
    }
    return std::string(made);
}

void RemoveTreeBestEffort(const std::string& dir) {
    const std::string cmd = "rm -rf '" + dir + "'";
    if (::system(cmd.c_str()) != 0) { /* ignored on purpose */ }
}

void WriteFile(const std::string& path, const std::string& contents) {
    MT_EXPECT_TRUE(core::fs::Write(path, contents).ok());
}

/// Rust's `write_image` helper. `lowers` is the raw JSON array text, so a test
/// can express a field the typed config would not let it omit.
std::string WriteImage(const std::string& dir, const std::string& lowers,
                       const std::string& repo_blob_url) {
    const std::string path = dir + "/image.json";
    WriteFile(path, "{\"repoBlobUrl\":\"" + repo_blob_url + "\",\"lowers\":" + lowers +
                        ",\"upper\":{},\"resultFile\":\"\"}");
    return path;
}

std::string Canonical(const std::string& path) {
    const core::Expected<std::string, std::string> out = core::fs::Canonicalize(path);
    MT_EXPECT_TRUE(out.ok());
    return out.ok() ? out.value() : std::string();
}

/// A sparse LSMT layer: data stored at its logical offset, so the stored bytes
/// and the uploaded bytes differ and no descriptor can honestly be recorded
/// up front.
bool BuildSparseLayer(const std::string& path) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> data =
        obd::LocalFile::New(path);
    if (!data.ok()) return false;
    const core::Expected<std::shared_ptr<obd::LsmtFile>, std::string> sparse =
        obd::LsmtFile::Create(data.value(), std::shared_ptr<obd::VirtualFile>(),
                              64ull * 1024 * 1024, true);
    if (!sparse.ok()) return false;
    const std::vector<uint8_t> page(4096, 0xAB);
    if (!sparse.value()->WriteAt(0, &page[0], page.size()).ok()) return false;
    if (!sparse.value()->CloseSeal().ok()) return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// what gets reused and what gets uploaded
// ---------------------------------------------------------------------------

/// The point of the whole exporter: a chained snapshot re-uses every blob the
/// registry already has and uploads only the delta this host produced. If the
/// remote lowers were re-uploaded the push would transfer the entire image on
/// every snapshot.
MT_TEST(a_chained_snapshot_reuses_remote_blobs_and_uploads_only_the_new_delta) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    WriteFile(delta, "delta2");
    const std::string image = WriteImage(
        dir,
        "[{\"digest\":\"sha256:base\",\"size\":123},"
        "{\"digest\":\"sha256:d1\",\"size\":456},"
        "{\"file\":\"" + delta + "\",\"digest\":\"sha256:d2\",\"size\":6}]",
        kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(loaded.ok());
    if (loaded.ok()) {
        std::vector<SourceRegistryLayer> want;
        want.push_back(SourceRegistryLayer::Remote("sha256:base", 123));
        want.push_back(SourceRegistryLayer::Remote("sha256:d1", 456));
        LocalSnapshotDelta local;
        local.path = Canonical(delta);
        local.descriptor = LocalSnapshotDeltaDescriptor::Raw("sha256:d2", 6);
        want.push_back(SourceRegistryLayer::LocalDelta(local));
        MT_EXPECT_TRUE(loaded.value().layers == want);

        // The target is the image-level source, normalised.
        MT_EXPECT_EQ(loaded.value().target.registry, std::string("registry.example"));
        MT_EXPECT_EQ(loaded.value().target.repository, std::string("ns/base"));
    }
    RemoveTreeBestEffort(dir);
}

/// A descriptorless local delta is measured here, not left for the publisher
/// to guess: the manifest has to name a digest and a size, and the only
/// truthful source for them is the file itself.
MT_TEST(a_descriptorless_local_delta_is_measured_from_the_file) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    WriteFile(delta, "delta2");
    const std::string image =
        WriteImage(dir,
                   "[{\"digest\":\"sha256:base\",\"size\":123},"
                   "{\"file\":\"" + delta + "\"}]",
                   kSource);

    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(delta);
    MT_EXPECT_TRUE(described.ok());

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(loaded.ok());
    if (loaded.ok() && described.ok()) {
        MT_EXPECT_EQ(loaded.value().layers.size(), static_cast<std::size_t>(2));
        const SourceRegistryLayer& local = loaded.value().layers[1];
        MT_EXPECT_TRUE(local.is_local_delta());
        MT_EXPECT_TRUE(local.local_delta.descriptor ==
                       LocalSnapshotDeltaDescriptor::Raw(described.value().sha256,
                                                         described.value().size));
    }
    RemoveTreeBestEffort(dir);
}

/// A recorded digest/size wins over measuring. Re-hashing could only disagree
/// with the descriptor already written into the config, and the manifest and
/// the config must name the same blob.
MT_TEST(a_recorded_descriptor_is_trusted_rather_than_recomputed) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    WriteFile(delta, "delta2");
    const std::string image =
        WriteImage(dir,
                   "[{\"digest\":\"sha256:base\",\"size\":123},"
                   "{\"file\":\"" + delta + "\",\"digest\":\"sha256:recorded\",\"size\":6}]",
                   kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(loaded.ok());
    if (loaded.ok()) {
        MT_EXPECT_TRUE(loaded.value().layers[1].local_delta.descriptor ==
                       LocalSnapshotDeltaDescriptor::Raw("sha256:recorded", 6));
    }
    RemoveTreeBestEffort(dir);
}

/// A sparse overlaybd delta is planned *without* a descriptor. The uploaded
/// bytes are the dense rewrite, not the bytes on disk, so hashing the file
/// here would record a digest for content the registry never receives.
MT_TEST(a_sparse_overlaybd_delta_is_planned_for_dense_export_with_no_descriptor) {
    // Where holes are a filesystem heuristic the layer is not recognised as
    // sparse, and the planner correctly falls back to a raw upload. Skipped
    // rather than asserted, matching the gate on the dense-export tests.
    if (!obd::sys::SparseExtentsAreReliable()) {
        MT_EXPECT_TRUE(true);
        return;
    }

    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(BuildSparseLayer(delta));
    const std::string image =
        WriteImage(dir,
                   "[{\"digest\":\"sha256:base\",\"size\":123},"
                   "{\"file\":\"" + delta + "\"}]",
                   kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(loaded.ok());
    if (loaded.ok()) {
        std::vector<SourceRegistryLayer> want;
        want.push_back(SourceRegistryLayer::Remote("sha256:base", 123));
        LocalSnapshotDelta local;
        local.path = Canonical(delta);
        local.descriptor = LocalSnapshotDeltaDescriptor::DenseOverlaybd();
        want.push_back(SourceRegistryLayer::LocalDelta(local));
        MT_EXPECT_TRUE(loaded.value().layers == want);
    }
    RemoveTreeBestEffort(dir);
}

/// An all-remote image is a valid plan with nothing to upload — a re-tag of an
/// unmodified snapshot. The `dir` cache hints on remote lowers are allowed;
/// only a *local* lower may not carry one.
MT_TEST(an_all_remote_image_is_accepted_with_nothing_to_upload) {
    const std::string dir = MakeTempDir();
    const std::string image = WriteImage(
        dir,
        "[{\"digest\":\"sha256:base\",\"size\":123,"
        "\"dir\":\"/var/lib/agentenv/image-cache/commits/sha256-base\"},"
        "{\"digest\":\"sha256:d1\",\"size\":456,"
        "\"dir\":\"/var/lib/agentenv/image-cache/commits/sha256-d1\"}]",
        kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(loaded.ok());
    if (loaded.ok()) {
        std::vector<SourceRegistryLayer> want;
        want.push_back(SourceRegistryLayer::Remote("sha256:base", 123));
        want.push_back(SourceRegistryLayer::Remote("sha256:d1", 456));
        MT_EXPECT_TRUE(loaded.value().layers == want);
    }
    RemoveTreeBestEffort(dir);
}

/// Several local deltas in a row are fine, and each keeps its own identity.
/// They share a basename, so a planner that keyed on the file name rather than
/// the canonical path would collapse them into one upload and publish a
/// manifest missing a layer.
MT_TEST(multiple_local_deltas_after_the_remote_lowers_stay_distinct) {
    const std::string dir = MakeTempDir();
    MT_EXPECT_TRUE(core::fs::CreateDirAll(dir + "/first").ok());
    MT_EXPECT_TRUE(core::fs::CreateDirAll(dir + "/second").ok());
    const std::string first = dir + "/first/snapshot.commit";
    const std::string second = dir + "/second/snapshot.commit";
    WriteFile(first, "delta1");
    WriteFile(second, "delta2");
    const std::string image = WriteImage(
        dir,
        "[{\"digest\":\"sha256:base\",\"size\":123},"
        "{\"file\":\"" + first + "\",\"digest\":\"sha256:first\",\"size\":6},"
        "{\"file\":\"" + second + "\",\"digest\":\"sha256:second\",\"size\":6}]",
        kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(loaded.ok());
    if (loaded.ok()) {
        std::vector<SourceRegistryLayer> want;
        want.push_back(SourceRegistryLayer::Remote("sha256:base", 123));
        LocalSnapshotDelta a;
        a.path = Canonical(first);
        a.descriptor = LocalSnapshotDeltaDescriptor::Raw("sha256:first", 6);
        want.push_back(SourceRegistryLayer::LocalDelta(a));
        LocalSnapshotDelta b;
        b.path = Canonical(second);
        b.descriptor = LocalSnapshotDeltaDescriptor::Raw("sha256:second", 6);
        want.push_back(SourceRegistryLayer::LocalDelta(b));
        MT_EXPECT_TRUE(loaded.value().layers == want);
    }
    RemoveTreeBestEffort(dir);
}

/// The recorded path is canonical, so the publisher opens the file this
/// planner validated. A relative `file=` resolves against the config's
/// directory, and a symlink is followed — two spellings of one delta must not
/// become two uploads.
MT_TEST(the_planned_delta_path_is_canonical) {
    const std::string dir = MakeTempDir();
    const std::string real = dir + "/snapshot.commit";
    WriteFile(real, "delta");
    const std::string link = dir + "/link.commit";
    MT_EXPECT_EQ(::symlink("snapshot.commit", link.c_str()), 0);

    const std::string image =
        WriteImage(dir,
                   "[{\"digest\":\"sha256:base\",\"size\":123},"
                   "{\"file\":\"" + link + "\",\"digest\":\"sha256:d\",\"size\":5}]",
                   kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    // The symlink's own name is not a runtime-generated delta; what decides is
    // the *resolved* name, because canonicalization happens first.
    MT_EXPECT_TRUE(loaded.ok());
    if (loaded.ok()) {
        MT_EXPECT_EQ(loaded.value().layers[1].local_delta.path, Canonical(real));
    }
    RemoveTreeBestEffort(dir);
}

// ---------------------------------------------------------------------------
// what is refused
// ---------------------------------------------------------------------------

/// Without a remote base there is nothing in the registry to build on, so this
/// would be a full image push the planner does not model.
MT_TEST(a_source_with_no_remote_base_is_refused) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    WriteFile(delta, "delta");
    const std::string image =
        WriteImage(dir, "[{\"file\":\"" + delta + "\",\"digest\":\"sha256:delta\",\"size\":5}]",
                   kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(!loaded.ok());
    if (!loaded.ok()) {
        MT_EXPECT_TRUE(loaded.error().kind == RepositoryErrorKind::Unsupported);
        MT_EXPECT_TRUE(loaded.error().ToString().find("at least one remote") !=
                       std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

/// The lowers are the layer stack bottom-up. A remote lower *after* a local
/// delta would put the snapshot's own writes below a registry layer, and
/// publishing that as a linear manifest would silently reorder the stack.
MT_TEST(a_remote_lower_after_a_local_delta_is_refused) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    WriteFile(delta, "delta");
    const std::string image = WriteImage(
        dir,
        "[{\"digest\":\"sha256:base\",\"size\":123},"
        "{\"file\":\"" + delta + "\",\"digest\":\"sha256:delta\",\"size\":5},"
        "{\"digest\":\"sha256:later\",\"size\":456}]",
        kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(!loaded.ok());
    if (!loaded.ok()) {
        MT_EXPECT_TRUE(loaded.error().kind == RepositoryErrorKind::Unsupported);
        MT_EXPECT_TRUE(loaded.error().ToString().find("before local deltas") !=
                       std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

/// A per-layer `repoBlobUrl` means the lowers do not all come from one
/// repository, and the publisher uploads to exactly one.
MT_TEST(a_per_layer_repo_blob_url_is_refused) {
    const std::string dir = MakeTempDir();
    const std::string image = WriteImage(
        dir,
        "[{\"digest\":\"sha256:base\",\"size\":123,"
        "\"repoBlobUrl\":\"https://registry.example/v2/ns/other/blobs\"}]",
        kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(!loaded.ok());
    if (!loaded.ok()) {
        MT_EXPECT_TRUE(loaded.error().kind == RepositoryErrorKind::Unsupported);
        MT_EXPECT_TRUE(loaded.error().ToString().find("per-layer repoBlobUrl") !=
                       std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

/// A per-layer URL does not substitute for the image-level one, even when it
/// names the right place: there would be no single destination to upload to.
MT_TEST(a_missing_image_level_repo_blob_url_is_refused) {
    const std::string dir = MakeTempDir();
    const std::string image =
        WriteImage(dir,
                   "[{\"digest\":\"sha256:base\",\"size\":123,"
                   "\"repoBlobUrl\":\"https://registry.example/v2/ns/base/blobs\"}]",
                   "");

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(!loaded.ok());
    if (!loaded.ok()) {
        MT_EXPECT_TRUE(loaded.error().kind == RepositoryErrorKind::Unsupported);
        MT_EXPECT_TRUE(loaded.error().ToString().find("single image-level") !=
                       std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

/// Only a sandbox's own runtime-generated rootfs delta may be uploaded. Any
/// other local file is something this host merely happens to have — a cached
/// remote blob, a hand-placed layer — and pushing it would publish bytes whose
/// provenance is unknown.
MT_TEST(a_local_lower_that_is_not_a_runtime_generated_delta_is_refused) {
    const std::string dir = MakeTempDir();
    const std::string blob = dir + "/some-cached-layer";
    WriteFile(blob, "not a delta");
    const std::string image =
        WriteImage(dir,
                   "[{\"digest\":\"sha256:base\",\"size\":123},"
                   "{\"file\":\"" + blob + "\",\"digest\":\"sha256:x\",\"size\":11}]",
                   kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(!loaded.ok());
    if (!loaded.ok()) {
        MT_EXPECT_TRUE(loaded.error().kind == RepositoryErrorKind::Unsupported);
        MT_EXPECT_TRUE(loaded.error().ToString().find("runtime-generated delta") !=
                       std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

/// A local lower that is simply gone fails as a backend error, not as an
/// unsupported shape: the config is well-formed and the operation may succeed
/// on a retry, which is a different answer for the caller.
MT_TEST(a_missing_local_delta_fails_as_a_backend_error) {
    const std::string dir = MakeTempDir();
    const std::string image =
        WriteImage(dir,
                   "[{\"digest\":\"sha256:base\",\"size\":123},"
                   "{\"file\":\"" + dir + "/absent/snapshot.commit\","
                   "\"digest\":\"sha256:d\",\"size\":5}]",
                   kSource);

    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(!loaded.ok());
    if (!loaded.ok()) {
        MT_EXPECT_TRUE(loaded.error().kind == RepositoryErrorKind::Backend);
        MT_EXPECT_TRUE(loaded.error().ToString().find("canonicalize local ACR delta") !=
                       std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

/// An unreadable image config is a backend error too, and names the path —
/// this runs behind an API call whose only output is the message.
MT_TEST(an_unreadable_image_config_fails_as_a_backend_error_naming_the_path) {
    const std::string dir = MakeTempDir();
    const std::string image = dir + "/image.json";
    const RepositoryResult<SourceRegistryImage> loaded = LoadSourceRegistryImage(image);
    MT_EXPECT_TRUE(!loaded.ok());
    if (!loaded.ok()) {
        MT_EXPECT_TRUE(loaded.error().kind == RepositoryErrorKind::Backend);
        MT_EXPECT_TRUE(loaded.error().ToString().find(image) != std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

/// The source URL goes through the one `SourceRegistryRepository::Parse`, so
/// its rules apply here: plain HTTP is refused, and the loopback exemption is
/// off unless asked for.
MT_TEST(the_image_level_source_url_is_held_to_the_https_rule) {
    const std::string dir = MakeTempDir();
    const std::string lowers = "[{\"digest\":\"sha256:base\",\"size\":123}]";

    const std::string insecure = WriteImage(dir, lowers, "http://registry.example/v2/app/blobs");
    MT_EXPECT_TRUE(!LoadSourceRegistryImage(insecure).ok());

    const std::string loopback = WriteImage(dir, lowers, "http://127.0.0.1:5000/v2/app/blobs");
    MT_EXPECT_TRUE(!LoadSourceRegistryImage(loopback).ok());
    MT_EXPECT_TRUE(LoadSourceRegistryImage(loopback, true).ok());
    RemoveTreeBestEffort(dir);
}

// ---------------------------------------------------------------------------
// RejectUnsupportedShape, directly
// ---------------------------------------------------------------------------

/// `targetFile` / `targetDigest` / `gzipIndex` describe a converted or
/// gzip-indexed layer the publisher has no code to upload. They are refused
/// before the local/remote split, because they are unsupported either way —
/// including on a lower that would otherwise look like a complete remote one.
MT_TEST(a_converted_or_gzip_indexed_lower_is_refused_whichever_side_it_is_on) {
    const char* const fields[] = {"target_file", "target_digest", "gzip_index"};
    for (std::size_t i = 0; i < 3; ++i) {
        obd::LayerConfig remote;
        remote.digest = "sha256:base";
        remote.size = 123;
        obd::LayerConfig local;
        local.file = "/tmp/snapshot.commit";

        if (i == 0) {
            remote.target_file = local.target_file = "/tmp/t";
        } else if (i == 1) {
            remote.target_digest = local.target_digest = "sha256:t";
        } else {
            remote.gzip_index = local.gzip_index = "/tmp/gz";
        }

        const RepositoryResult<core::Unit> on_remote = RejectUnsupportedShape(3, remote);
        MT_EXPECT_TRUE(!on_remote.ok());
        if (!on_remote.ok()) {
            MT_EXPECT_TRUE(on_remote.error().kind == RepositoryErrorKind::Unsupported);
            // The index is in the message: a four-layer image gives the
            // operator no other way to tell which lower was the problem.
            MT_EXPECT_TRUE(on_remote.error().ToString().find("lower 3") != std::string::npos);
        }
        MT_EXPECT_TRUE(!RejectUnsupportedShape(0, local).ok());
        (void)fields[i];
    }
}

/// A local lower with a `dir` cache hint is ambiguous: `dir` names a
/// downloaded copy of a *remote* blob, so the two together do not say which
/// file is authoritative.
MT_TEST(a_local_lower_with_a_dir_cache_hint_is_refused) {
    obd::LayerConfig layer;
    layer.file = "/tmp/snapshot.commit";
    layer.dir = "/var/lib/agentenv/image-cache/commits/sha256-x";
    const RepositoryResult<core::Unit> out = RejectUnsupportedShape(1, layer);
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        MT_EXPECT_TRUE(out.error().ToString().find("dir cache hint") != std::string::npos);
    }

    // A local lower *without* one is accepted here, and no descriptor is
    // required at this stage — that is the dense-export case.
    obd::LayerConfig bare;
    bare.file = "/tmp/snapshot.commit";
    MT_EXPECT_TRUE(RejectUnsupportedShape(1, bare).ok());
}

/// A remote lower is named in the manifest by digest and size alone; there is
/// no file to measure, so both must already be recorded. A zero size counts as
/// missing: the manifest would advertise an empty blob.
MT_TEST(a_remote_lower_must_already_carry_a_digest_and_a_size) {
    obd::LayerConfig no_digest;
    no_digest.size = 123;
    MT_EXPECT_TRUE(!RejectUnsupportedShape(0, no_digest).ok());
    MT_EXPECT_TRUE(RejectUnsupportedShape(0, no_digest).error().ToString().find(
                       "missing digest") != std::string::npos);

    obd::LayerConfig no_size;
    no_size.digest = "sha256:base";
    MT_EXPECT_TRUE(!RejectUnsupportedShape(0, no_size).ok());
    MT_EXPECT_TRUE(RejectUnsupportedShape(0, no_size).error().ToString().find("missing size") !=
                   std::string::npos);

    obd::LayerConfig complete;
    complete.digest = "sha256:base";
    complete.size = 123;
    MT_EXPECT_TRUE(RejectUnsupportedShape(0, complete).ok());
}

// ---------------------------------------------------------------------------
// the value types
// ---------------------------------------------------------------------------

/// `DenseOverlaybd` carries nothing, so a stale `digest`/`size` left behind by
/// a reassignment must not take part in equality. Without this, two plans that
/// both mean "rewrite this sparse layer during upload" could compare unequal.
MT_TEST(a_dense_descriptor_ignores_the_unused_raw_fields) {
    LocalSnapshotDeltaDescriptor dense = LocalSnapshotDeltaDescriptor::Raw("sha256:x", 7);
    dense = LocalSnapshotDeltaDescriptor::DenseOverlaybd();
    LocalSnapshotDeltaDescriptor fresh = LocalSnapshotDeltaDescriptor::DenseOverlaybd();
    MT_EXPECT_TRUE(dense == fresh);

    // Different variants never compare equal.
    MT_EXPECT_TRUE(dense != LocalSnapshotDeltaDescriptor::Raw("sha256:x", 7));
    // And a remote layer is never a local delta, whatever it carries.
    LocalSnapshotDelta delta;
    delta.path = "/tmp/snapshot.commit";
    MT_EXPECT_TRUE(SourceRegistryLayer::Remote("sha256:x", 7) !=
                   SourceRegistryLayer::LocalDelta(delta));
}

MT_MAIN
