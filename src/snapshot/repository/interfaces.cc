// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/interfaces.rs
#include "agentenv/snapshot/repository/interfaces.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace snapshot {
namespace repository {

SnapshotListFilter SnapshotListFilter::Templates() {
    SnapshotListFilter f;
    std::vector<SnapshotSourceKind> s;
    s.push_back(SnapshotSourceKind::Template);
    f.sources = s;
    return f;
}

SnapshotListFilter SnapshotListFilter::ByIds(const std::vector<core::SnapshotId>& ids) {
    SnapshotListFilter f;
    f.snapshot_ids = ids;
    return f;
}

bool SnapshotListFilter::Matches(const SnapshotRecord& r) const {
    // Rust leaves matching to each backend; the posix-fs catalog had the only
    // correct implementation and every other backend would have needed its
    // own copy. It lives here now so there is one set of rules, and the
    // catalog's `MatchesRecordFilter` forwards to it.
    if (alias_prefix.has_value()) {
        // An unaliased record cannot match a prefix. The length test matters:
        // `compare` on a short string would read past its end.
        if (!r.alias.has_value()) return false;
        const std::string alias = r.alias->ToString();
        if (alias.size() < alias_prefix->size() ||
            alias.compare(0, alias_prefix->size(), *alias_prefix) != 0) {
            return false;
        }
    }

    if (snapshot_ids.has_value()) {
        bool found = false;
        for (std::size_t i = 0; i < snapshot_ids->size(); ++i) {
            if ((*snapshot_ids)[i] == r.id) { found = true; break; }
        }
        if (!found) return false;
    }

    if (snapshot_id_or_alias.has_value()) {
        const bool id_matches = r.id.ToString() == *snapshot_id_or_alias;
        const bool alias_matches =
            r.alias.has_value() && r.alias->ToString() == *snapshot_id_or_alias;
        if (!id_matches && !alias_matches) return false;
    }

    if (source_sandbox_id.has_value()) {
        // Only a sandbox-sourced record can match; a template never does.
        if (r.source.kind != SnapshotSourceKind::Sandbox) return false;
        if (r.source.source_sandbox_id != *source_sandbox_id) return false;
    }

    if (sources.has_value()) {
        bool found = false;
        for (std::size_t i = 0; i < sources->size(); ++i) {
            if ((*sources)[i] == r.source.kind) { found = true; break; }
        }
        if (!found) return false;
    }

    if (template_statuses.has_value()) {
        // A status filter implies a template: a sandbox record has no build.
        if (r.source.kind != SnapshotSourceKind::Template) return false;
        bool found = false;
        for (std::size_t i = 0; i < template_statuses->size(); ++i) {
            if ((*template_statuses)[i] == r.source.build.status) { found = true; break; }
        }
        if (!found) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Document and blob defaults.
//
// Rust gives each of these a body of `unsupported("<feature>")`, so a backend
// that only deals in snapshots still satisfies the trait and fails loudly at
// runtime. The feature strings below are copied verbatim from upstream because
// they surface in API error messages.
// ---------------------------------------------------------------------------

RepositoryResult<core::Optional<std::string> > SnapshotRepository::GetDocument(
    const DocumentKey&) {
    return core::make_unexpected(RepositoryError::Unsupported("repository documents"));
}

RepositoryResult<core::Unit> SnapshotRepository::PutDocument(const DocumentKey&,
                                                             const std::string&) {
    return core::make_unexpected(RepositoryError::Unsupported("repository documents"));
}

RepositoryResult<core::Optional<std::string> > SnapshotRepository::UpdateDocument(
    const DocumentKey&, const DocumentUpdate&) {
    return core::make_unexpected(RepositoryError::Unsupported("repository documents"));
}

RepositoryResult<core::Unit> SnapshotRepository::DeleteDocument(const DocumentKey&) {
    return core::make_unexpected(RepositoryError::Unsupported("repository documents"));
}

RepositoryResult<std::vector<std::string> > SnapshotRepository::ListDocuments(
    DocumentCollection, const core::Optional<std::string>&, std::size_t) {
    return core::make_unexpected(RepositoryError::Unsupported("repository documents"));
}

core::Optional<std::string> SnapshotRepository::ManagedLayersRepoBlobUrl() const {
    return core::Optional<std::string>();
}

RepositoryResult<core::Unit> SnapshotRepository::PutBlob(const BlobKey&, const BlobBody&) {
    return core::make_unexpected(RepositoryError::Unsupported("blob storage"));
}

RepositoryResult<BlobLocation> SnapshotRepository::LocateBlob(const BlobKey&) const {
    return core::make_unexpected(RepositoryError::Unsupported("blob storage"));
}

RepositoryResult<bool> SnapshotRepository::HasBlob(const BlobKey&) {
    return core::make_unexpected(RepositoryError::Unsupported("blob storage"));
}

RepositoryResult<std::string> SnapshotRepository::ReadBlob(const BlobKey&) {
    return core::make_unexpected(RepositoryError::Unsupported("blob storage"));
}

namespace {

/// Rust `managed_layer_config` (layer_config.rs) — a managed layer read in
/// place when the repository allows it; otherwise overlaybd reads it by
/// digest from the repository's `repoBlobUrl`.
RepositoryResult<storage::overlaybd::LayerConfig> ManagedLayerConfig(
    const SnapshotRepository& repository, const ManagedLayer& layer, LayerRole role) {
    const RepositoryResult<BlobLocation> location =
        repository.LocateBlob(BlobKey::Layer(layer.digest, role));
    if (!location.ok()) return core::make_unexpected(location.error());
    storage::overlaybd::LayerConfig config;
    if (location.value().kind == BlobLocation::Kind::Local) config.file = location.value().value;
    config.digest = layer.digest;
    config.size = layer.size;
    if (layer.uuid.has_value()) config.uuid = *layer.uuid;
    return config;
}

}  // namespace

RepositoryResult<std::string> MaterializeVolumeBacking(
    const SnapshotRepository& repository, const std::vector<OverlaybdLayerRef>& layers,
    const std::string& destination) {
    const core::Optional<std::string> managed_repo_blob_url =
        repository.ManagedLayersRepoBlobUrl();
    storage::overlaybd::ImageConfig image_config;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const OverlaybdLayerRef& layer = layers[i];
        if (layer.kind == OverlaybdLayerRef::Kind::Managed) {
            RepositoryResult<storage::overlaybd::LayerConfig> config =
                ManagedLayerConfig(repository, layer.managed, LayerRole::Rootfs);
            if (!config.ok()) return core::make_unexpected(config.error());
            if (managed_repo_blob_url.has_value()) {
                config.value().repo_blob_url = *managed_repo_blob_url;
            }
            image_config.lowers.push_back(config.value());
        } else {
            storage::overlaybd::LayerConfig config;
            config.repo_blob_url = layer.external.repo_blob_url;
            config.digest = layer.external.digest;
            config.size = layer.external.size;
            image_config.lowers.push_back(config);
        }
    }

    const core::Optional<std::string> parent = core::fs::Parent(destination);
    if (!parent.has_value() || parent->empty()) {
        return core::make_unexpected(
            RepositoryError::Backend("volume image config '" + destination + "' has no parent"));
    }
    const core::Expected<core::Unit, std::string> created = core::fs::CreateDirAll(*parent);
    if (!created.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "create volume image config dir '" + *parent + "'", created.error()));
    }
    // Rust `serde_json::to_vec_pretty(&image_config)`.
    const std::string bytes = storage::overlaybd::ImageConfigToJsonPretty(image_config);
    // Rust `destination.with_extension(format!("tmp-{}", Uuid::now_v7().simple()))`.
    std::string stem = destination;
    const std::size_t slash = destination.rfind('/');
    const std::size_t dot = destination.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) {
        stem = destination.substr(0, dot);
    }
    const std::string temporary = stem + ".tmp-" + core::Uuid::GenV7().ToSimpleString();
    const core::Expected<core::Unit, std::string> written = core::fs::Write(temporary, bytes);
    if (!written.ok()) {
        core::fs::RemoveFile(temporary);
        return core::make_unexpected(RepositoryError::Backend(
            "write temporary volume image config '" + temporary + "'", written.error()));
    }
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        const std::string error = std::strerror(errno);
        core::fs::RemoveFile(temporary);
        return core::make_unexpected(RepositoryError::Backend(
            "install volume image config '" + destination + "'", error));
    }
    return destination;
}

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
