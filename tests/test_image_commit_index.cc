// SPDX-License-Identifier: MIT
// Tests for image::CommitIndex and the overlaybd commit-cache helpers.
// Mirrors src/image/commit_index.rs #[cfg(test)] plus a SHA-256 known-answer
// check against the reference.rs vector.
#include "microtest.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "agentenv/core/digest.h"
#include "agentenv/image/commit_index.h"

using namespace agentenv;

namespace {

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_ci_XXXXXX";
    char* p = ::mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

void WriteFile(const std::string& path, const std::string& data) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
    std::istreambuf_iterator<char>());
}

uint64_t Ino(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_ino);
}

}  // namespace

// Sanity: the self-contained SHA-256 matches the reference.rs vector.
MT_TEST(sha256_matches_reference_vector) {
    MT_EXPECT_TRUE(
        core::Sha256Digest(std::string("artifact")) ==
  "sha256:c7c5c1d70c5dec4416ab6158afd0b223ef40c29b1dc1f97ed9428b94d4cadb1c");
    // Empty-string vector (FIPS 180-4).
    MT_EXPECT_TRUE(
  core::Sha256Hex(std::string("")) ==
     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

MT_TEST(digest_slug_uses_algo_prefix) {
  MT_EXPECT_TRUE(image::DigestSlug("sha256:abc123") == "sha256-abc123");
}

MT_TEST(digest_slug_is_bounded) {
    std::string digest = "sha256:" + std::string(160 * 2, 'a');
    MT_EXPECT_TRUE(image::DigestSlug(digest).size() == 160);
}

MT_TEST(seed_commit_file_copies_and_verifies_digest) {
    std::string dir = MakeTempDir();
    MT_EXPECT_TRUE(!dir.empty());
    std::string source = dir + "/snapshot.commit";
    WriteFile(source, "sealed delta");
    std::string digest = core::Sha256Digest(std::string("sealed delta"));

    auto cached = image::SeedCommitFile(dir + "/commits", source, digest, 12);
    MT_EXPECT_TRUE(cached.ok());

  std::string expected = dir + "/commits/" + image::DigestSlug(digest) +
       "/overlaybd.commit";
    MT_EXPECT_TRUE(cached.value() == expected);
    MT_EXPECT_TRUE(ReadFile(cached.value()) == "sealed delta");
}

MT_TEST(seed_commit_file_rejects_digest_mismatch) {
    std::string dir = MakeTempDir();
    std::string source = dir + "/snapshot.commit";
WriteFile(source, "sealed delta");

    auto err = image::SeedCommitFile(dir + "/commits", source, "sha256:0000", 12);
    MT_EXPECT_TRUE(!err.ok());
    MT_EXPECT_TRUE(err.error().find("digest mismatch") != std::string::npos);
}

MT_TEST(accept_existing_commit_file_checks_size) {
    std::string dir = MakeTempDir();
    std::string destination = dir + "/overlaybd.commit";
 WriteFile(destination, "sealed delta");

    auto accepted = image::AcceptExistingCommitFile(destination, "sha256:existing", 12);
  MT_EXPECT_TRUE(accepted.ok());
    MT_EXPECT_TRUE(accepted.value() == destination);

  auto err = image::AcceptExistingCommitFile(destination, "sha256:existing", 11);
    MT_EXPECT_TRUE(!err.ok());
    MT_EXPECT_TRUE(err.error().find("size mismatch") != std::string::npos);
}

MT_TEST(seed_commit_file_trusted_descriptor_links_without_hashing) {
    std::string dir = MakeTempDir();
    std::string source = dir + "/snapshot.commit";
    WriteFile(source, "sealed delta");

    auto cached = image::SeedCommitFileTrustedDescriptor(
      dir + "/commits", source, "sha256:trusted", 12);
    MT_EXPECT_TRUE(cached.ok());

  std::string expected = dir + "/commits/" + image::DigestSlug("sha256:trusted") +
      "/overlaybd.commit";
    MT_EXPECT_TRUE(cached.value() == expected);
    // Hard-linked: same inode as the source.
    MT_EXPECT_TRUE(Ino(source) == Ino(cached.value()));
    MT_EXPECT_TRUE(ReadFile(cached.value()) == "sealed delta");
}

MT_TEST(seed_commit_file_trusted_descriptor_rejects_size_mismatch) {
    std::string dir = MakeTempDir();
    std::string source = dir + "/snapshot.commit";
    WriteFile(source, "sealed delta");

    auto err = image::SeedCommitFileTrustedDescriptor(
        dir + "/commits", source, "sha256:trusted", 11);
    MT_EXPECT_TRUE(!err.ok());
    MT_EXPECT_TRUE(err.error().find("size mismatch") != std::string::npos);
}

MT_TEST(oci_index_context_checks_parent_commit) {
 image::CommitIndex index = image::CommitIndex::OciLayer(
     "sha256:oci", "sha256:commit", 123, "converter", 64, false,
        core::Optional<std::string>(std::string("sha256:parent")));

    MT_EXPECT_TRUE(index.MatchesOciContext(
        "sha256:oci", "converter", 64, false,
     core::Optional<std::string>(std::string("sha256:parent"))));
    MT_EXPECT_TRUE(!index.MatchesOciContext(
      "sha256:oci", "converter", 64, false, core::Optional<std::string>()));

    std::string root = "/cache/indexes";
    std::string without_parent =
        image::IndexPath(root, "sha256:oci", "converter", 64, false,
        core::Optional<std::string>());
    std::string with_parent = image::IndexPath(
        root, "sha256:oci", "converter", 64, false,
     core::Optional<std::string>(std::string("sha256:parent")));
    MT_EXPECT_TRUE(with_parent.find(root + "/sha256-oci/") == 0);
    MT_EXPECT_TRUE(without_parent != with_parent);
}

MT_TEST(commit_index_json_roundtrip) {
    image::CommitIndex index = image::CommitIndex::OciLayer(
        "sha256:oci", "sha256:commit", 123, "converter", 64, true,
   core::Optional<std::string>(std::string("sha256:parent")));

    std::string dir = MakeTempDir();
    std::string path = dir + "/nested/overlaybd.commit.json";
    auto w = index.Write(path);
    MT_EXPECT_TRUE(w.ok());

    auto read = image::CommitIndex::Read(path);
    MT_EXPECT_TRUE(read.ok());
    MT_EXPECT_TRUE(read.value().has_value());
    MT_EXPECT_TRUE(*read.value() == index);

    // Absent file -> Ok(None).
    auto missing = image::CommitIndex::Read(dir + "/does-not-exist.json");
  MT_EXPECT_TRUE(missing.ok());
    MT_EXPECT_TRUE(!missing.value().has_value());
}

MT_MAIN
