// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/target.rs
#include "agentenv/snapshot/image_export_target.h"

#include <cctype>
#include <set>

#include "agentenv/snapshot/source_registry.h"

namespace agentenv {
namespace snapshot {

const char* const kDefaultImageTag = "latest";

namespace {

SnapshotImageTargetError InvalidTarget(const std::string& reason) {
    SnapshotImageTargetError error;
    error.kind = SnapshotImageTargetError::Kind::InvalidTarget;
    error.reason = reason;
    return error;
}

SnapshotImageTargetError CannotInfer(const std::string& reason) {
    SnapshotImageTargetError error;
    error.kind = SnapshotImageTargetError::Kind::CannotInfer;
    error.reason = reason;
    return error;
}

/// Rust `valid_component` — a lowercase alnum start, then alnum plus
/// `.`, `_`, `-`. Uppercase is refused because a registry would reject it.
bool IsValidComponent(const std::string& component) {
    if (component.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(component[0]);
    const bool valid_first = (first >= 'a' && first <= 'z') || std::isdigit(first) != 0;
    if (!valid_first) return false;
    for (std::size_t i = 1; i < component.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(component[i]);
        const bool valid = (c >= 'a' && c <= 'z') || std::isdigit(c) != 0 || c == '.' ||
                           c == '_' || c == '-';
        if (!valid) return false;
    }
    return true;
}

bool HasWhitespace(const std::string& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(value[i])) != 0) return true;
    }
    return false;
}

std::string ToLowerAscii(const std::string& value) {
    std::string lowered = value;
    for (std::size_t i = 0; i < lowered.size(); ++i) {
        if (lowered[i] >= 'A' && lowered[i] <= 'Z') {
            lowered[i] = static_cast<char>(lowered[i] - 'A' + 'a');
        }
    }
    return lowered;
}

/// Rust `canonical_rootfs_publication` — the publication carrying this
/// snapshot's own rootfs tag.
core::Optional<PersistedDiskImagePublication> CanonicalRootfsPublication(
    const core::SnapshotId& snapshot_id, const CommittedSnapshot& committed) {
    const std::string rootfs_tag = RootfsSnapshotImageTag(snapshot_id);
    for (std::size_t i = 0; i < committed.disk_publications.size(); ++i) {
        if (committed.disk_publications[i].tag == rootfs_tag) {
            return core::Optional<PersistedDiskImagePublication>(
                committed.disk_publications[i]);
        }
    }
    return core::Optional<PersistedDiskImagePublication>();
}

/// Rust `infer_repository_from_blob_urls` — collects into a `BTreeSet`, so
/// exactly one distinct location must remain.
core::Expected<SourceRegistryRepository, SnapshotImageTargetError>
InferRepositoryFromBlobUrls(const std::vector<std::string>& repo_blob_urls) {
    std::set<SourceRegistryRepository> locations;
    for (std::size_t i = 0; i < repo_blob_urls.size(); ++i) {
        const repository::RepositoryResult<SourceRegistryRepository> source =
            SourceRegistryRepository::Parse(repo_blob_urls[i]);
        if (!source.ok()) {
            return core::make_unexpected(CannotInfer(source.error().ToString()));
        }
        locations.insert(source.value());
    }

    if (locations.size() != 1) {
        // Publishing to an arbitrary one of several sources would be a
        // guess, so the caller is told to pass --target-repository.
        const char* const reason = locations.empty()
                                       ? "snapshot rootfs has no external registry source"
                                       : "snapshot rootfs has multiple external registry "
                                         "sources";
        return core::make_unexpected(CannotInfer(reason));
    }
    return *locations.begin();
}

}  // namespace

std::string SnapshotImageTargetError::Message() const {
    if (kind == Kind::InvalidTarget) {
        return "invalid snapshot image target: " + reason;
    }
    return "cannot infer snapshot image target: " + reason + "; pass --target-repository";
}

bool IsValidRepository(const std::string& repository) {
    if (repository.empty()) return false;
    std::size_t begin = 0;
    while (true) {
        const std::size_t slash = repository.find('/', begin);
        const std::string component =
            slash == std::string::npos ? repository.substr(begin)
                                       : repository.substr(begin, slash - begin);
        if (!IsValidComponent(component)) return false;
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    return true;
}

core::Expected<std::string, SnapshotImageTargetError> ValidatedTag(
    const core::Optional<std::string>& tag) {
    const std::string value = tag.has_value() ? *tag : std::string(kDefaultImageTag);
    bool valid = !value.empty() && value.size() <= 128;
    if (valid) {
        const unsigned char first = static_cast<unsigned char>(value[0]);
        valid = (std::isalnum(first) != 0 && first < 0x80) || first == '_';
    }
    for (std::size_t i = 1; valid && i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        valid = (std::isalnum(c) != 0 && c < 0x80) || c == '_' || c == '.' || c == '-';
    }
    if (!valid) {
        return core::make_unexpected(InvalidTarget("invalid OCI image tag '" + value + "'"));
    }
    return value;
}

core::Expected<SnapshotImageTarget, SnapshotImageTargetError> SnapshotImageTarget::Parse(
    const std::string& target_repository, const core::Optional<std::string>& tag) {
    // A scheme or digest here would mean the caller passed a URL or a pinned
    // reference where a repository is expected.
    if (target_repository.find("://") != std::string::npos ||
        target_repository.find('@') != std::string::npos) {
        return core::make_unexpected(
            InvalidTarget("target repository must not include a URL scheme or digest"));
    }

    const std::size_t slash = target_repository.find('/');
    if (slash == std::string::npos) {
        return core::make_unexpected(
            InvalidTarget("target repository must include a registry and repository path"));
    }
    const std::string registry_part = target_repository.substr(0, slash);
    const std::string repository_part = target_repository.substr(slash + 1);

    if (registry_part.empty() || HasWhitespace(registry_part) ||
        !IsValidRepository(repository_part)) {
        return core::make_unexpected(InvalidTarget(
            "target repository contains an invalid registry or repository path"));
    }

    std::string registry = ToLowerAscii(registry_part);
    // Strip a default `:443`, matching `SourceRegistryRepository::parse`, so
    // an explicit target and an inferred one compare equal.
    const std::string default_port = ":443";
    if (registry.size() > default_port.size() &&
        registry.compare(registry.size() - default_port.size(), default_port.size(),
                         default_port) == 0) {
        const std::string host = registry.substr(0, registry.size() - default_port.size());
        if (!host.empty()) registry = host;
    }

    const core::Expected<std::string, SnapshotImageTargetError> validated_tag =
        ValidatedTag(tag);
    if (!validated_tag.ok()) return core::make_unexpected(validated_tag.error());

    SnapshotImageTarget target;
    target.registry = registry;
    target.repository = repository_part;
    target.tag = validated_tag.value();
    return target;
}

std::string SnapshotImageTarget::ImageRef() const {
    return registry + "/" + repository + ":" + tag;
}

core::Expected<SnapshotImageTarget, SnapshotImageTargetError> ResolveSnapshotImageTarget(
    const core::SnapshotId& snapshot_id, const CommittedSnapshot& committed,
    const core::Optional<std::string>& target_repository,
    const core::Optional<std::string>& tag) {
    if (target_repository.has_value()) {
        return SnapshotImageTarget::Parse(*target_repository, tag);
    }

    std::vector<std::string> candidates;
    const core::Optional<PersistedDiskImagePublication> publication =
        CanonicalRootfsPublication(snapshot_id, committed);
    if (publication.has_value()) {
        // A previous publication of this same snapshot is the most precise
        // answer available.
        candidates.push_back(publication->repo_blob_url);
    } else {
        for (std::size_t i = 0; i < committed.rootfs_layers.size(); ++i) {
            const OverlaybdLayerRef& layer = committed.rootfs_layers[i];
            // Managed layers live in this repository, not a registry.
            if (layer.kind != OverlaybdLayerRef::Kind::External) continue;
            // OSS-managed layers carry a synthetic `s3://` URL, which names
            // no registry.
            const std::string& url = layer.external.repo_blob_url;
            if (url.compare(0, 5, "s3://") == 0) continue;
            candidates.push_back(url);
        }
    }

    const core::Expected<SourceRegistryRepository, SnapshotImageTargetError> inferred =
        InferRepositoryFromBlobUrls(candidates);
    if (!inferred.ok()) return core::make_unexpected(inferred.error());

    SnapshotImageTarget target;
    target.registry = inferred.value().registry;
    target.repository = inferred.value().repository;
    if (tag.has_value()) {
        const core::Expected<std::string, SnapshotImageTargetError> validated =
            ValidatedTag(tag);
        if (!validated.ok()) return core::make_unexpected(validated.error());
        target.tag = validated.value();
    } else {
        // A standalone snapshot tag, so an inferred publish never overwrites
        // the source image's own `latest`.
        target.tag = std::string("snapshot-") + snapshot_id.ToString();
    }
    return target;
}

core::Optional<PersistedDiskImagePublication> SnapshotManagedPublicationForTarget(
    const CommittedSnapshot& committed, const SnapshotImageTarget& target) {
    const std::string image_ref = target.ImageRef();
    for (std::size_t i = 0; i < committed.disk_publications.size(); ++i) {
        const PersistedDiskImagePublication& publication = committed.disk_publications[i];
        if (publication.tag != target.tag) continue;

        const repository::RepositoryResult<SourceRegistryRepository> source =
            SourceRegistryRepository::Parse(publication.repo_blob_url);
        if (source.ok()) {
            // Compare the normalised registry/repository rather than the raw
            // strings, so `:443` and a case difference still match.
            if (source.value().registry == target.registry &&
                source.value().repository == target.repository) {
                return core::Optional<PersistedDiskImagePublication>(publication);
            }
            continue;
        }
        // An unparsable blob URL falls back to the recorded image ref, which
        // is all an older record has.
        if (publication.image_ref == image_ref) {
            return core::Optional<PersistedDiskImagePublication>(publication);
        }
    }
    return core::Optional<PersistedDiskImagePublication>();
}

}  // namespace snapshot
}  // namespace agentenv
