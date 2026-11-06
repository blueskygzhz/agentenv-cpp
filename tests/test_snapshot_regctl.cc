// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/regctl.rs
//
// Driven against a real fake `regctl` executable, not an injected functor.
// `BlobPipe` is almost entirely fork/pipe/dup2/waitpid, so stubbing the
// process layer would leave the part most likely to be wrong untested: here
// the kernel really does move bytes between two children, and the receiving
// child really does hash them.
#include "agentenv/snapshot/image_export/regctl.h"

#include <string>

#include "agentenv/core/digest.h"
#include "fake_regctl.h"
#include "microtest.h"

using namespace agentenv;                          // NOLINT
using namespace agentenv::snapshot::image_export;  // NOLINT
using namespace agentenv::snapshot::repository;     // NOLINT

namespace {

const char* kSourceRepo = "reg.example/team/app";
const char* kTargetRepo = "reg.example/team/newapp";
const char* kOtherRepo = "other.example/team/app";

std::string DigestOf(const std::string& bytes) { return core::Sha256Digest(bytes); }

// ---- manifest head --------------------------------------------------------

/// A missing ref is absent, not an error: a first export has nothing there,
/// and that is what tells the caller to upload rather than to compare.
MT_TEST(regctl_manifest_head_absent_for_unknown_ref) {
    const testing::FakeRegctl fake;
    const Regctl regctl(fake.binary());

    const core::Expected<core::Optional<std::string>, RepositoryError> head =
        regctl.ManifestHead("reg.example/team/app:v1");
    MT_EXPECT_TRUE(head.ok());
    MT_EXPECT_TRUE(!head.value().has_value());
}

MT_TEST(regctl_manifest_head_returns_digest) {
    const testing::FakeRegctl fake;
    fake.SeedManifest("reg.example/team/app:v1", "sha256:abc");
    const Regctl regctl(fake.binary());

    const core::Expected<core::Optional<std::string>, RepositoryError> head =
        regctl.ManifestHead("reg.example/team/app:v1");
    MT_EXPECT_TRUE(head.ok() && head.value().has_value());
    // Trimmed: the fake writes a trailing newline, as regctl does, and an
    // untrimmed digest would never compare equal to a computed one.
    MT_EXPECT_EQ(*head.value(), std::string("sha256:abc"));
}

// ---- blob head ------------------------------------------------------------

/// The probe that keeps a re-export from pushing gigabytes the target has.
MT_TEST(regctl_blob_head_reports_presence) {
    const testing::FakeRegctl fake;
    const std::string digest = DigestOf("layer bytes");
    fake.SeedBlob(kTargetRepo, digest, "layer bytes");
    const Regctl regctl(fake.binary());

    MT_EXPECT_TRUE(regctl.BlobHead(kTargetRepo, digest).value());
    MT_EXPECT_TRUE(!regctl.BlobHead(kTargetRepo, DigestOf("absent")).value());
}

// ---- blob put -------------------------------------------------------------

MT_TEST(regctl_blob_put_uploads_payload) {
    const testing::FakeRegctl fake;
    const std::string bytes = "payload bytes";
    const std::string digest = DigestOf(bytes);
    const std::string path = fake.dir() + "/payload";
    std::ofstream(path.c_str(), std::ios::binary).write(bytes.data(),
                                                        static_cast<std::streamsize>(bytes.size()));
    const Regctl regctl(fake.binary());

    MT_EXPECT_TRUE(regctl.BlobPut(kTargetRepo, digest, path).ok());
    MT_EXPECT_EQ(fake.ReadBlob(kTargetRepo, digest), bytes);
    MT_EXPECT_TRUE(fake.Logged("blob put " + std::string(kTargetRepo) + " --digest " + digest));
}

/// The payload is reopened per attempt, so a retry restarts at offset zero.
/// Reusing the descriptor would upload the tail of a half-sent blob and then
/// fail the digest check -- which reads as corruption, not as a retry.
MT_TEST(regctl_blob_put_retries_from_the_start) {
    const testing::FakeRegctl fake;
    const std::string bytes = "payload bytes";
    const std::string digest = DigestOf(bytes);
    const std::string path = fake.dir() + "/payload";
    std::ofstream(path.c_str(), std::ios::binary).write(bytes.data(),
                                                        static_cast<std::streamsize>(bytes.size()));
    fake.FailTransiently("bput", 1);
    const Regctl regctl(fake.binary());

    MT_EXPECT_TRUE(regctl.BlobPut(kTargetRepo, digest, path).ok());
    // The whole payload arrived, not just what was left after the first try.
    MT_EXPECT_EQ(fake.ReadBlob(kTargetRepo, digest), bytes);
}

// ---- blob copy ------------------------------------------------------------

/// Within one registry the bytes never leave it: a cross-repo mount.
MT_TEST(regctl_blob_copy_mounts_within_a_registry) {
    const testing::FakeRegctl fake;
    const std::string bytes = "same-registry layer";
    const std::string digest = DigestOf(bytes);
    fake.SeedBlob(kSourceRepo, digest, bytes);
    const Regctl regctl(fake.binary());

    MT_EXPECT_TRUE(regctl.BlobCopy(kSourceRepo, kTargetRepo, digest).ok());
    MT_EXPECT_EQ(fake.ReadBlob(kTargetRepo, digest), bytes);
    MT_EXPECT_TRUE(fake.Logged("blob copy " + std::string(kSourceRepo) + " " + kTargetRepo +
                               " " + digest));
}

/// A source blob that is not there is a missing *artifact*, not a backend
/// fault: the caller asked to copy something absent, and the preceding
/// `blob head` already proved the target lacked it.
MT_TEST(regctl_blob_copy_missing_source_is_artifact_not_found) {
    const testing::FakeRegctl fake;
    const Regctl regctl(fake.binary());

    const core::Expected<core::Unit, RepositoryError> copied =
        regctl.BlobCopy(kSourceRepo, kTargetRepo, DigestOf("absent"));
    MT_EXPECT_TRUE(!copied.ok());
    MT_EXPECT_TRUE(copied.error().kind == RepositoryErrorKind::ArtifactNotFound);
    // Exactly one attempt: a 404 cannot become a different answer, so the
    // retry budget is not spent on it.
    MT_EXPECT_EQ(fake.ArgvLines().size(), static_cast<std::size_t>(1));
}

// ---- blob pipe ------------------------------------------------------------

/// The real thing: two children joined by an OS pipe, bytes never entering
/// this process. The receiving child hashes what it got, so an assertion on
/// the stored blob is also an assertion that the transfer was complete.
MT_TEST(regctl_blob_pipe_streams_across_registries) {
    const testing::FakeRegctl fake;
    const std::string bytes = "cross-registry layer";
    const std::string digest = DigestOf(bytes);
    fake.SeedBlob(kOtherRepo, digest, bytes);
    const Regctl regctl(fake.binary());

    MT_EXPECT_TRUE(regctl.BlobPipe(kOtherRepo, kTargetRepo, digest).ok());
    MT_EXPECT_EQ(fake.ReadBlob(kTargetRepo, digest), bytes);
    MT_EXPECT_TRUE(fake.Logged("blob get " + std::string(kOtherRepo) + " " + digest));
    MT_EXPECT_TRUE(fake.Logged("blob put " + std::string(kTargetRepo) + " --digest " + digest));
}

/// Larger than a pipe buffer, which is the case the ordering exists for:
/// `put` is spawned first so the reader is already draining. Started the
/// other way round, `get` would block until `put` happened to be scheduled.
MT_TEST(regctl_blob_pipe_handles_a_blob_larger_than_the_pipe_buffer) {
    const testing::FakeRegctl fake;
    // 1 MiB against a 64 KiB default pipe buffer, so the writer must block
    // and be unblocked by the reader rather than by luck.
    const std::string bytes(1024 * 1024, 'x');
    const std::string digest = DigestOf(bytes);
    fake.SeedBlob(kOtherRepo, digest, bytes);
    const Regctl regctl(fake.binary());

    MT_EXPECT_TRUE(regctl.BlobPipe(kOtherRepo, kTargetRepo, digest).ok());
    MT_EXPECT_EQ(fake.ReadBlob(kTargetRepo, digest).size(), bytes.size());
    MT_EXPECT_TRUE(fake.ReadBlob(kTargetRepo, digest) == bytes);
}

/// An absent source is terminal and attributed to the source, not retried.
MT_TEST(regctl_blob_pipe_missing_source_is_artifact_not_found) {
    const testing::FakeRegctl fake;
    const Regctl regctl(fake.binary());

    const core::Expected<core::Unit, RepositoryError> piped =
        regctl.BlobPipe(kOtherRepo, kTargetRepo, DigestOf("absent"));
    MT_EXPECT_TRUE(!piped.ok());
    MT_EXPECT_TRUE(piped.error().kind == RepositoryErrorKind::ArtifactNotFound);
    MT_EXPECT_TRUE(piped.error().ToString().find(kOtherRepo) != std::string::npos);
}

// ---- manifest put ---------------------------------------------------------

/// The content type is passed explicitly, because regctl otherwise guesses
/// from the body -- and a guess of the Docker v2 media type would rewrite the
/// bytes, changing the digest the caller just compared against the registry.
MT_TEST(regctl_manifest_put_pins_the_oci_content_type) {
    const testing::FakeRegctl fake;
    const std::string body = "{\"schemaVersion\":2}";
    const std::string path = fake.dir() + "/manifest.json";
    std::ofstream(path.c_str(), std::ios::binary).write(body.data(),
                                                        static_cast<std::streamsize>(body.size()));
    const Regctl regctl(fake.binary());

    MT_EXPECT_TRUE(regctl.ManifestPut("reg.example/team/app:v1", path).ok());
    MT_EXPECT_EQ(fake.StoredManifestBody("reg.example/team/app:v1"), body);
    MT_EXPECT_TRUE(fake.Logged(std::string("manifest put --content-type ") +
                               kRegctlOciManifestContentType + " reg.example/team/app:v1"));
    // The fake hashes what it stored, so this is the digest a registry would
    // report for the bytes that actually arrived.
    MT_EXPECT_EQ(fake.StoredManifestDigest("reg.example/team/app:v1"), DigestOf(body));
}

}  // namespace

MT_MAIN
