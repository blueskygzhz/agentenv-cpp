// SPDX-License-Identifier: MIT
// Rust: src/snapshot/manager.rs — `SnapshotManager::publish_p2p_artifacts`,
//       called from `publish_with_recording` right after the commit.
//
// This was the announce half of snapshot P2P. The resolve half was wired (a
// node resolving a snapshot asks peers before the repository), and nothing
// ever *became* a peer: `SnapshotP2pArtifact::Publish` and its four
// constructors were fully ported and reachable only from tests, so on a
// cluster with `[p2p] enabled = true` every node fetched every memory dump
// and rootfs layer from the repository forever.
#include "microtest.h"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/snapshot/p2p.h"
#include "agentenv/snapshot/publish.h"

using namespace agentenv;
using namespace agentenv::snapshot;

namespace {

/// Records every publish the fan-out performs.
class RecordingTransport : public p2p::P2pTransport {
 public:
    p2p::P2pResult<bool> LookupWithHints(const p2p::P2pArtifactKey&,
                                         const std::vector<p2p::P2pArtifactProviderHint>&,
                                         p2p::P2pArtifactDescriptor*) override {
        return false;
    }
    p2p::P2pResult<uint64_t> FetchWithOptions(const p2p::P2pArtifactDescriptor&,
                                              const std::string&,
                                              p2p::P2pFetchOptions) override {
        return core::make_unexpected(p2p::P2pError::MakeDisabled());
    }
    p2p::P2pResult<std::vector<uint8_t> > FetchBytesWithOptions(
        const p2p::P2pArtifactDescriptor&, p2p::P2pFetchOptions) override {
        return core::make_unexpected(p2p::P2pError::MakeDisabled());
    }
    p2p::P2pResult<std::shared_ptr<p2p::P2pByteStream> > FetchByteRange(
        const p2p::P2pArtifactDescriptor&, uint64_t, size_t) override {
        return core::make_unexpected(p2p::P2pError::MakeDisabled());
    }
    p2p::P2pResult<bool> Unpublish(const p2p::P2pArtifactKey&) override { return false; }

    p2p::P2pResult<core::Unit> Publish(const p2p::P2pPublishRequest& request) override {
        std::lock_guard<std::mutex> guard(mu_);
        keys_.push_back(request.key);
        metadata_[request.key] = request.metadata_json;
        if (request.source.kind == p2p::P2pPublishSource::Kind::Bytes) {
            bytes_[request.key] =
                std::string(request.source.bytes.begin(), request.source.bytes.end());
        } else {
            paths_[request.key] = request.source.path;
        }
        if (fail_keys_.count(request.key) != 0) {
            return core::make_unexpected(p2p::P2pError::MakeDisabled());
        }
        return core::Unit{};
    }

    /// Makes one key's publish fail, to prove the rest still go out.
    void FailKey(const std::string& key) {
        std::lock_guard<std::mutex> guard(mu_);
        fail_keys_.insert(key);
    }

    std::vector<std::string> Keys() const {
        std::lock_guard<std::mutex> guard(mu_);
        return keys_;
    }
    bool Announced(const std::string& key) const {
        std::lock_guard<std::mutex> guard(mu_);
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) return true;
        }
        return false;
    }
    int CountOf(const std::string& key) const {
        std::lock_guard<std::mutex> guard(mu_);
        int n = 0;
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) ++n;
        }
        return n;
    }
    std::string PathOf(const std::string& key) const {
        std::lock_guard<std::mutex> guard(mu_);
        const std::map<std::string, std::string>::const_iterator it = paths_.find(key);
        return it == paths_.end() ? std::string() : it->second;
    }
    std::string BytesOf(const std::string& key) const {
        std::lock_guard<std::mutex> guard(mu_);
        const std::map<std::string, std::string>::const_iterator it = bytes_.find(key);
        return it == bytes_.end() ? std::string() : it->second;
    }

 private:
    mutable std::mutex                 mu_;
    std::vector<std::string>           keys_;
    std::map<std::string, std::string> paths_;
    std::map<std::string, std::string> bytes_;
    std::map<std::string, std::string> metadata_;
    std::set<std::string>              fail_keys_;
};

struct TempRoot {
    std::string path;
    TempRoot() {
        char tmpl[] = "/tmp/aenv-pub-p2p-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

void WriteFile(const std::string& path, const std::string& contents) {
    MT_EXPECT_TRUE(core::fs::Write(path, contents).ok());
}

const char* const kId = "11111111-2222-3333-4444-555555555555";

core::SnapshotId Id() {
    const core::Expected<core::SnapshotId, std::string> id = core::SnapshotId::Parse(kId);
    MT_EXPECT_TRUE(id.ok());
    return id.value();
}

/// A committed publication whose manifest points at real files under `root`.
SnapshotPublication MakePublication(const TempRoot& root,
                                    sandbox::SandboxSnapshotManifest* manifest_out) {
    WriteFile(root.path + "/vm_state.bin", "vm-state-bytes");

    sandbox::SandboxSnapshotManifest manifest;
    manifest.backend                   = "firecracker";
    manifest.vm_state.path             = root.path + "/vm_state.bin";
    manifest.memory.image_config_path  = root.path + "/memory-image.json";
    manifest.memory.virtual_size       = 512u * 1024 * 1024;
    manifest.rootfs.image_config_path  = root.path + "/rootfs-image.json";
    manifest.rootfs.virtual_size       = 1024u * 1024 * 1024;
    *manifest_out                      = manifest;

    SnapshotPublication publication;
    publication.record.id = Id();
    CommittedSnapshot committed;
    publication.record.committed = core::Optional<CommittedSnapshot>(committed);
    return publication;
}

}  // namespace

// ============================================================
// the two early returns
// ============================================================

MT_TEST(a_null_transport_announces_nothing) {
    // P2P is optional, so a node without a transport publishes normally and
    // simply skips the announcement. Must not crash on the null.
    TempRoot root;
    sandbox::SandboxSnapshotManifest manifest;
    const SnapshotPublication publication = MakePublication(root, &manifest);
    PublishSnapshotP2pArtifacts(std::shared_ptr<p2p::P2pTransport>(), publication, manifest);
}

MT_TEST(an_uncommitted_record_announces_nothing) {
    // A record with no `committed` names no layers, and the snapshot is not
    // resumable yet: advertising it would offer peers bytes that resolve to
    // nothing.
    TempRoot root;
    sandbox::SandboxSnapshotManifest manifest;
    SnapshotPublication publication = MakePublication(root, &manifest);
    publication.record.committed = core::Optional<CommittedSnapshot>();

    std::shared_ptr<RecordingTransport> transport(new RecordingTransport());
    PublishSnapshotP2pArtifacts(transport, publication, manifest);
    MT_EXPECT_EQ(transport->Keys().size(), static_cast<std::size_t>(0));
}

// ============================================================
// the two fixed artifacts
// ============================================================

MT_TEST(the_vm_state_and_manifest_are_announced_under_fixed_keys) {
    TempRoot root;
    sandbox::SandboxSnapshotManifest manifest;
    const SnapshotPublication publication = MakePublication(root, &manifest);

    std::shared_ptr<RecordingTransport> transport(new RecordingTransport());
    PublishSnapshotP2pArtifacts(transport, publication, manifest);

    const std::string prefix = std::string("snapshot/v1/artifacts/") + kId + "/";
    MT_EXPECT_TRUE(transport->Announced(prefix + "vm_state.bin"));
    MT_EXPECT_TRUE(transport->Announced(prefix + "firecracker-manifest.json"));

    // The vm state is offered from the *capture* path, not a repository copy:
    // on an OSS backend the repository copy is always remote, which is the
    // case this whole mechanism exists to avoid.
    MT_EXPECT_EQ(transport->PathOf(prefix + "vm_state.bin"),
                 root.path + "/vm_state.bin");
}

MT_TEST(the_announced_manifest_is_compact_json) {
    // Upstream announces `serde_json::to_vec(&manifest)`, while the copy
    // `StoreSnapshot` writes into the repository is pretty-printed. The two
    // are deliberately not byte-identical; a consumer parses this one.
    TempRoot root;
    sandbox::SandboxSnapshotManifest manifest;
    const SnapshotPublication publication = MakePublication(root, &manifest);

    std::shared_ptr<RecordingTransport> transport(new RecordingTransport());
    PublishSnapshotP2pArtifacts(transport, publication, manifest);

    const std::string announced = transport->BytesOf(
        std::string("snapshot/v1/artifacts/") + kId + "/firecracker-manifest.json");
    MT_EXPECT_TRUE(!announced.empty());
    MT_EXPECT_TRUE(announced.find('\n') == std::string::npos);
    // Round-trips, so a peer that fetched it can actually restore from it.
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(announced);
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    const core::Expected<sandbox::SandboxSnapshotManifest, std::string> decoded =
        sandbox::SandboxSnapshotManifest::FromJson(parsed.value());
    MT_EXPECT_TRUE(decoded.ok());
}

// ============================================================
// the prepared uploads
// ============================================================

MT_TEST(prepared_uploads_are_announced_content_addressed) {
    // The route that matters for compressed and dense-exported layers: the
    // committed record names the *rewritten* digest, and those bytes exist
    // only in the temporary the upload still owns. Announcing the capture
    // file instead would offer bytes whose digest nothing asks for.
    TempRoot root;
    sandbox::SandboxSnapshotManifest manifest;
    SnapshotPublication publication = MakePublication(root, &manifest);

    const std::string layer = root.path + "/prepared.commit";
    WriteFile(layer, "prepared-layer-bytes");
    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(layer);
    MT_EXPECT_TRUE(described.ok());
    if (!described.ok()) return;

    const repository::RepositoryResult<PreparedLayerUpload> upload =
        PrepareLayerUpload(layer, overlaybd::OverlaybdCompactOutput::Raw(),
                           core::Optional<std::string>(described.value().sha256),
                           described.value().size);
    MT_EXPECT_TRUE(upload.ok());
    if (!upload.ok()) return;
    publication.local_layers.push_back(upload.value());

    std::shared_ptr<RecordingTransport> transport(new RecordingTransport());
    PublishSnapshotP2pArtifacts(transport, publication, manifest);

    const std::string key =
        std::string("overlaybd-layer/v1/") + described.value().sha256;
    MT_EXPECT_TRUE(transport->Announced(key));
    MT_EXPECT_EQ(transport->PathOf(key), layer);
}

MT_TEST(a_duplicate_key_is_announced_once) {
    // An unchanged local layer reaches the list twice: once as a prepared
    // upload, once from the image config. Upstream dedups on the key.
    TempRoot root;
    sandbox::SandboxSnapshotManifest manifest;
    SnapshotPublication publication = MakePublication(root, &manifest);

    const std::string layer = root.path + "/dup.commit";
    WriteFile(layer, "dup-layer-bytes");
    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(layer);
    MT_EXPECT_TRUE(described.ok());
    if (!described.ok()) return;

    const repository::RepositoryResult<PreparedLayerUpload> upload =
        PrepareLayerUpload(layer, overlaybd::OverlaybdCompactOutput::Raw(),
                           core::Optional<std::string>(described.value().sha256),
                           described.value().size);
    MT_EXPECT_TRUE(upload.ok());
    if (!upload.ok()) return;
    // The same upload twice is the simplest way to reach the dedup; the key is
    // derived from the digest, so both entries produce one key.
    publication.local_layers.push_back(upload.value());
    publication.local_layers.push_back(upload.value());

    std::shared_ptr<RecordingTransport> transport(new RecordingTransport());
    PublishSnapshotP2pArtifacts(transport, publication, manifest);

    MT_EXPECT_EQ(transport->CountOf(std::string("overlaybd-layer/v1/") +
                                    described.value().sha256),
                 1);
}

// ============================================================
// best-effort semantics
// ============================================================

MT_TEST(a_failed_announcement_does_not_abort_the_rest) {
    // Rust warns and the stream continues. A snapshot nobody can fetch over
    // P2P still resolves through the repository, so one failure must not cost
    // the other announcements.
    TempRoot root;
    sandbox::SandboxSnapshotManifest manifest;
    const SnapshotPublication publication = MakePublication(root, &manifest);

    const std::string prefix = std::string("snapshot/v1/artifacts/") + kId + "/";
    std::shared_ptr<RecordingTransport> transport(new RecordingTransport());
    transport->FailKey(prefix + "vm_state.bin");

    PublishSnapshotP2pArtifacts(transport, publication, manifest);
    MT_EXPECT_TRUE(transport->Announced(prefix + "vm_state.bin"));
    MT_EXPECT_TRUE(transport->Announced(prefix + "firecracker-manifest.json"));
}

MT_TEST(a_disabled_transport_is_accepted_as_a_no_op) {
    // `DisabledP2pTransport::Publish` returns Ok, which is what makes P2P an
    // optional acceleration path rather than a publish dependency.
    TempRoot root;
    sandbox::SandboxSnapshotManifest manifest;
    const SnapshotPublication publication = MakePublication(root, &manifest);
    std::shared_ptr<p2p::P2pTransport> transport(new p2p::DisabledP2pTransport());
    PublishSnapshotP2pArtifacts(transport, publication, manifest);
}

MT_TEST(many_artifacts_are_announced_under_bounded_concurrency) {
    // 20 uploads against a concurrency bound of 8: every key must still be
    // announced exactly once, which is what a batching bug would break.
    TempRoot root;
    sandbox::SandboxSnapshotManifest manifest;
    SnapshotPublication publication = MakePublication(root, &manifest);

    std::vector<std::string> digests;
    for (int i = 0; i < 20; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "/layer-%02d.commit", i);
        const std::string layer = root.path + name;
        WriteFile(layer, std::string("layer-bytes-") + name);
        const core::Expected<core::FileDigest, std::string> described =
            core::DescribeFile(layer);
        MT_EXPECT_TRUE(described.ok());
        if (!described.ok()) return;
        const repository::RepositoryResult<PreparedLayerUpload> upload =
            PrepareLayerUpload(layer, overlaybd::OverlaybdCompactOutput::Raw(),
                               core::Optional<std::string>(described.value().sha256),
                               described.value().size);
        MT_EXPECT_TRUE(upload.ok());
        if (!upload.ok()) return;
        publication.local_layers.push_back(upload.value());
        digests.push_back(described.value().sha256);
    }

    std::shared_ptr<RecordingTransport> transport(new RecordingTransport());
    PublishSnapshotP2pArtifacts(transport, publication, manifest);

    for (std::size_t i = 0; i < digests.size(); ++i) {
        MT_EXPECT_EQ(transport->CountOf(std::string("overlaybd-layer/v1/") + digests[i]), 1);
    }
    // Two fixed artifacts plus the twenty layers.
    MT_EXPECT_EQ(transport->Keys().size(), static_cast<std::size_t>(22));
}

MT_MAIN
