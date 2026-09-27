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

bool SnapshotListFilter::Matches(const SnapshotRecord& r) const {
    if (alias_prefix.has_value()) {
        const std::string& pfx = *alias_prefix;
        if (r.alias.compare(0, pfx.size(), pfx) != 0) return false;
    }
    if (snapshot_ids.has_value()) {
        const std::vector<std::string>& ids = *snapshot_ids;
        bool found = false;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == r.id) { found = true; break; }
        }
        if (!found) return false;
    }
    if (snapshot_id_or_alias.has_value()) {
        const std::string& v = *snapshot_id_or_alias;
        if (r.id != v && r.alias != v) return false;
    }
    if (source_sandbox_id.has_value()) {
        if (r.source != SnapshotSourceKind::Sandbox) return false;
        if (r.source_sandbox_id != *source_sandbox_id) return false;
    }
    if (sources.has_value()) {
        const std::vector<SnapshotSourceKind>&ss = *sources;
        bool found = false;
        for (size_t i = 0; i < ss.size(); ++i) {
            if (ss[i] == r.source) { found = true; break; }
        }
        if (!found) return false;
    }
    if (template_statuses.has_value()) {
        if (r.source != SnapshotSourceKind::Template) return false;
        const std::vector<TemplateBuildStatus>& ts = *template_statuses;
        bool found = false;
        for (size_t i = 0; i < ts.size(); ++i) {
            if (ts[i] == r.build_status) { found = true; break; }
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
