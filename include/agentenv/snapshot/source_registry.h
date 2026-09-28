// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/common/acr/source_image.rs —
// `SourceRegistryRepository` only.
//
// Parses the `repoBlobUrl` recorded with an external layer back into the
// registry and repository it came from. The normalisation matters: two URLs
// that differ only by an explicit `:443` name the same registry, and treating
// them as different would publish a snapshot twice.
#ifndef AGENTENV_SNAPSHOT_SOURCE_REGISTRY_H_
#define AGENTENV_SNAPSHOT_SOURCE_REGISTRY_H_

#include <string>

#include "agentenv/snapshot/repository/errors.h"

namespace agentenv {
namespace snapshot {

/// Rust `struct SourceRegistryRepository`.
struct SourceRegistryRepository {
    std::string registry;
    std::string repository;
    /// The canonicalised URL: original scheme, normalised registry, and the
    /// path with any trailing slash removed.
    std::string repo_blob_url;

    /// Rust `SourceRegistryRepository::parse`.
    ///
    /// Requires `https://<registry>/v2/<repo>/blobs`. Plain HTTP is refused
    /// except on loopback, which exists for tests against a local registry.
    static repository::RepositoryResult<SourceRegistryRepository> Parse(
        const std::string& repo_blob_url);

    /// Rust `image_ref`.
    std::string ImageRef(const std::string& tag) const;

    /// Rust `upload_url`.
    std::string UploadUrl() const;

    /// Rust `manifest_url`.
    std::string ManifestUrl(const std::string& tag) const;

    bool operator==(const SourceRegistryRepository& o) const {
        return registry == o.registry && repository == o.repository &&
               repo_blob_url == o.repo_blob_url;
    }
    bool operator!=(const SourceRegistryRepository& o) const { return !(*this == o); }
    bool operator<(const SourceRegistryRepository& o) const {
        if (registry != o.registry) return registry < o.registry;
        return repository < o.repository;
    }
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_SOURCE_REGISTRY_H_
