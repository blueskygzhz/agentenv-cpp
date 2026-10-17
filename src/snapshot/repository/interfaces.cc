// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/interfaces.rs
#include "agentenv/snapshot/repository/interfaces.h"

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
// Volume catalog defaults.
//
// Rust gives each of these a body of `unsupported("<feature>")`, so a backend
// that only deals in snapshots still satisfies the trait and fails loudly at
// runtime. The feature strings below are copied verbatim from upstream because
// they surface in API error messages.
// ---------------------------------------------------------------------------

RepositoryResult<core::Optional<volume::VolumeRecord> >
SnapshotRepository::GetVolume(const std::string&) {
    return core::make_unexpected(RepositoryError::Unsupported("volume catalog"));
}

RepositoryResult<VolumeRecordPage>
SnapshotRepository::ListVolumesPage(const core::Optional<std::string>&, std::size_t) {
    return core::make_unexpected(RepositoryError::Unsupported("volume catalog"));
}

RepositoryResult<core::Unit> SnapshotRepository::CreateVolume(const volume::VolumeRecord&) {
    return core::make_unexpected(RepositoryError::Unsupported("volume catalog"));
}

RepositoryResult<core::Unit> SnapshotRepository::PutVolume(const volume::VolumeRecord&) {
    return core::make_unexpected(RepositoryError::Unsupported("volume catalog"));
}

RepositoryResult<core::Unit> SnapshotRepository::DeleteVolume(const std::string&) {
    return core::make_unexpected(RepositoryError::Unsupported("volume catalog"));
}

RepositoryResult<core::Optional<std::string> >
SnapshotRepository::ReserveVolume(const std::string&, const std::string&) {
    return core::make_unexpected(RepositoryError::Unsupported("volume reservations"));
}

RepositoryResult<core::Unit>
SnapshotRepository::ReserveReadOnlyVolume(const std::string&, const std::string&) {
    return core::make_unexpected(RepositoryError::Unsupported("read-only volume reservations"));
}

RepositoryResult<core::Unit>
SnapshotRepository::ReplaceVolumeOwnerFor(const std::string&, const std::string&,
                                          const core::Optional<std::string>&) {
    return core::make_unexpected(RepositoryError::Unsupported("volume reservations"));
}

RepositoryResult<std::vector<OverlaybdLayerRef> >
SnapshotRepository::PublishVolumeBacking(const std::string&, const std::string&) {
    return core::make_unexpected(RepositoryError::Unsupported("volume backing publication"));
}

RepositoryResult<std::string>
SnapshotRepository::MaterializeVolumeBacking(const std::string&,
                                             const std::vector<OverlaybdLayerRef>&,
                                             const std::string&) {
    return core::make_unexpected(RepositoryError::Unsupported("volume backing materialization"));
}

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
