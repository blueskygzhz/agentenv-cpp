// SPDX-License-Identifier: MIT
// Rust: src/snapshot/p2p.rs
#include "agentenv/snapshot/p2p.h"

#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/overlaybd/p2p/artifact.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/layer.h"

namespace agentenv {
namespace snapshot {

const char* const kSnapshotP2pKeyPrefix = "snapshot/v1";

p2p::P2pArtifactKey FixedArtifactKey(const core::SnapshotId& snapshot_id,
                                     const std::string& name) {
    std::ostringstream os;
    os << kSnapshotP2pKeyPrefix << "/artifacts/" << snapshot_id.ToString() << "/" << name;
    return os.str();
}

SnapshotP2pArtifact SnapshotP2pArtifact::Fixed(const core::SnapshotId& snapshot_id,
                                               const std::string& name,
                                               const std::string& source_path) {
    SnapshotP2pArtifact artifact;
    artifact.key          = FixedArtifactKey(snapshot_id, name);
    artifact.source       = p2p::P2pPublishSource::FromPath(source_path);
    artifact.publish_mode = p2p::P2pPublishMode::Copy;
    return artifact;
}

SnapshotP2pArtifact SnapshotP2pArtifact::Bytes(const core::SnapshotId& snapshot_id,
                                               const std::string& name,
                                               const std::vector<uint8_t>& source) {
    SnapshotP2pArtifact artifact;
    artifact.key          = FixedArtifactKey(snapshot_id, name);
    artifact.source       = p2p::P2pPublishSource::FromBytes(source);
    artifact.publish_mode = p2p::P2pPublishMode::Copy;
    return artifact;
}

SnapshotP2pArtifact SnapshotP2pArtifact::ContentAddressedOverlaybdLayer(
    const std::string& source_path, const std::string& sha256, uint64_t size) {
    SnapshotP2pArtifact artifact;
    artifact.key          = overlaybd::p2p::LayerKeyFromDigest(sha256);
    artifact.source       = p2p::P2pPublishSource::FromPath(source_path);
    artifact.publish_mode = p2p::P2pPublishMode::Copy;
    artifact.metadata     = overlaybd::p2p::LayerMetadata::FromDigest(
        sha256, core::Optional<uint64_t>(size),
        core::Optional<std::string>()).ToJson();
    return artifact;
}

SnapshotP2pArtifact SnapshotP2pArtifact::UuidOverlaybdLayer(const std::string& source_path,
                                                            const std::string& uuid,
                                                            uint64_t size) {
    SnapshotP2pArtifact artifact;
    artifact.key          = overlaybd::p2p::LayerKeyFromUuid(uuid);
    artifact.source       = p2p::P2pPublishSource::FromPath(source_path);
    artifact.publish_mode = p2p::P2pPublishMode::Copy;
    artifact.metadata     = overlaybd::p2p::LayerMetadata::FromUuid(
        uuid, core::Optional<uint64_t>(size)).ToJson();
    return artifact;
}

std::vector<SnapshotP2pArtifact> SnapshotP2pArtifact::LocalOverlaybdLayers(
    const std::string& image_config_path,
    const std::set<std::string>& committed_digests,
    const std::set<std::string>& committed_uuids) {
    std::vector<SnapshotP2pArtifact> artifacts;

    core::Expected<std::string, std::string> text =
        core::fs::ReadToString(image_config_path);
    if (!text.ok()) {
        AGENTENV_WARN("skipping snapshot P2P layer publication because image config "
                          "could not be loaded: " + image_config_path + ": " + text.error());
        return artifacts;
    }
    core::Expected<storage::overlaybd::ImageConfig, std::string> image_config =
        storage::overlaybd::ParseImageConfig(text.value());
    if (!image_config.ok()) {
        AGENTENV_WARN("skipping snapshot P2P layer publication because image config "
                          "could not be loaded: " + image_config_path + ": " +
                          image_config.error());
        return artifacts;
    }

    const std::vector<storage::overlaybd::LayerConfig>& lowers = image_config.value().lowers;
    for (std::size_t i = 0; i < lowers.size(); ++i) {
        const storage::overlaybd::LayerConfig& layer = lowers[i];
        // `dir=` layers are remote-recoverable; there is no local file to serve.
        if (layer.file.empty()) continue;

        if (!layer.digest.empty() && layer.size > 0) {
            if (committed_digests.find(layer.digest) != committed_digests.end()) {
                artifacts.push_back(
                    ContentAddressedOverlaybdLayer(layer.file, layer.digest, layer.size));
            } else {
                // The committed record references different bytes for this
                // layer, so publish-time compression recontainerized the raw
                // local layer as zfile during upload. Publishing the raw file
                // under its raw digest would create a key no consumer ever
                // looks up.
                // TODO: propagate the uploaded (compressed) layer paths out of
                // repository publish so digest-keyed P2P publication can
                // advertise the same bytes the committed manifest records.
                AGENTENV_DEBUG("skipping digest-keyed snapshot P2P layer publication: "
                                   "layer digest is absent from the committed record, so "
                                   "the layer was recontainerized during upload and the "
                                   "raw-digest key would not match the manifest: " +
                                   layer.file);
            }
        }

        // Rust returns early before touching the filesystem when uuid-keyed
        // publication is not wanted at all.
        if (committed_uuids.empty()) continue;

        core::Expected<std::string, std::string> uuid =
            storage::overlaybd::ReadOverlaybdLayerUuid(layer.file);
        if (!uuid.ok()) {
            AGENTENV_WARN("skipping snapshot P2P layer publication because overlaybd "
                              "layer uuid could not be read: " + layer.file + ": " +
                              uuid.error());
            continue;
        }
        if (uuid.value().empty()) {
            AGENTENV_WARN("skipping snapshot P2P layer publication because overlaybd "
                              "layer uuid is nil: " + layer.file);
            continue;
        }
        if (committed_uuids.find(uuid.value()) == committed_uuids.end()) continue;

        // Rust reads the on-disk length rather than trusting the config, and
        // requires a regular file.
        if (!core::fs::IsFile(layer.file)) {
            AGENTENV_WARN("skipping snapshot P2P layer publication because path is not "
                              "a regular file: " + layer.file);
            continue;
        }
        core::Expected<uint64_t, std::string> size = core::fs::FileSize(layer.file);
        if (!size.ok()) {
            AGENTENV_WARN("skipping snapshot P2P layer publication because layer size "
                              "could not be read: " + layer.file + ": " + size.error());
            continue;
        }
        artifacts.push_back(UuidOverlaybdLayer(layer.file, uuid.value(), size.value()));
    }
    return artifacts;
}

core::Expected<core::Unit, std::string>
SnapshotP2pArtifact::Publish(const std::shared_ptr<p2p::P2pTransport>& transport) const {
    // Rust only carries the publish mode on the path variant; the bytes
    // variant is always a copy because there is no file to reference.
    p2p::P2pPublishRequest request =
        source.kind == p2p::P2pPublishSource::Path
            ? p2p::P2pPublishRequest::File(key, source.path).WithPublishMode(publish_mode)
            : p2p::P2pPublishRequest::FromBytes(key, source.bytes);
    request.WithMetadata(metadata);

    p2p::P2pResult<core::Unit> published = transport->Publish(request);
    if (!published.ok()) {
        return core::make_unexpected(std::string("publish snapshot artifact '") + key +
                                     "' to P2P: " + published.error().ToString());
    }
    return core::Unit{};
}

core::Expected<uint64_t, std::string>
FetchArtifact(const std::shared_ptr<p2p::P2pTransport>& transport,
              const p2p::P2pArtifactKey& key, const std::string& destination) {
    p2p::P2pArtifactDescriptor descriptor;
    p2p::P2pResult<bool> found = transport->Lookup(key, &descriptor);
    if (!found.ok()) return core::make_unexpected(found.error().ToString());
    if (!found.value()) {
        return core::make_unexpected(std::string("snapshot P2P artifact '") + key +
                                     "' was not found");
    }
    p2p::P2pResult<uint64_t> size = transport->Fetch(descriptor, destination);
    if (!size.ok()) {
        return core::make_unexpected(std::string("fetch snapshot P2P artifact '") + key +
                                     "': " + size.error().ToString());
    }
    return size.value();
}

core::Expected<std::vector<uint8_t>, std::string>
FetchArtifactBytes(const std::shared_ptr<p2p::P2pTransport>& transport,
                   const p2p::P2pArtifactKey& key) {
    p2p::P2pArtifactDescriptor descriptor;
    p2p::P2pResult<bool> found = transport->Lookup(key, &descriptor);
    if (!found.ok()) return core::make_unexpected(found.error().ToString());
    if (!found.value()) {
        return core::make_unexpected(std::string("snapshot P2P artifact '") + key +
                                     "' was not found");
    }
    p2p::P2pResult<std::vector<uint8_t> > bytes = transport->FetchBytes(descriptor);
    if (!bytes.ok()) {
        return core::make_unexpected(std::string("fetch snapshot P2P artifact '") + key +
                                     "': " + bytes.error().ToString());
    }
    return bytes.value();
}

}  // namespace snapshot
}  // namespace agentenv
