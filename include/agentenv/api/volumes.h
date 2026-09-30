// SPDX-License-Identifier: MIT
// Rust: src/api/impls/volumes.rs — volume mount resolution, the record
// projection, and the error-to-status mapping.
//
// `ResolveVolumeMounts` is the part with real consequences. It reserves each
// volume as it goes, and a failure anywhere in the loop releases *everything*
// reserved so far: a half-reserved request would otherwise leave volumes
// leased to a sandbox that never launched, and nothing else would free them.
//
// Mount paths are checked for overlap component-wise, not by string prefix.
// `/mnt/a` and `/mnt/ab` are siblings and may coexist; `/mnt/a` and `/mnt/a/b`
// would shadow each other inside the guest and are rejected.
//
// The handler methods themselves (`volumes_get`, `volumes_post`, ...) are bound
// to the generated route response enums and are not ported here; what they
// need — the status map, the JSON shape, and the mode parsing — is.
#ifndef AGENTENV_API_VOLUMES_H_
#define AGENTENV_API_VOLUMES_H_

#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/api/dto.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/volume.h"

namespace agentenv {
namespace api {

/// Rust `models::Error`. Volume handlers return the same `{code, message}`
/// body as every other handler, so this is just the shared type.
typedef ApiError VolumeApiError;

/// Rust `error_response` — the status each `VolumeError` maps to.
///
/// The 409 group is the interesting one: a name conflict, an active
/// reservation, an in-flight upload and a failed backing are all "the volume
/// exists but cannot be used for this right now", which is a conflict rather
/// than a bad request.
int VolumeErrorStatus(const volume::VolumeError& error);

/// Rust `error_response` — status and rendered message together.
VolumeApiError VolumeErrorResponse(const volume::VolumeError& error);

/// Rust `From<VolumeRecord> for models::Volume`.
///
/// Only the five public fields are projected: the backing layers, the local
/// image-config cache path and the reservation owner are node-internal.
core::Json VolumeRecordToJson(const volume::VolumeRecord& record);

/// Rust's mode parsing in `volumes_post`: absent defaults to `exclusive`, and
/// an unrecognised value is rejected rather than silently defaulted.
core::Expected<volume::VolumeMode, VolumeApiError>
    ParseVolumeModeParam(const core::Optional<std::string>& mode);

/// The result of resolving a request's `mounts` map.
struct ResolvedVolumeMounts {
    /// One drive per mount, ordered by mount path.
    std::vector<sandbox::ExtraDrive> drives;
    /// Normalized mount path -> volume id, which is what gets recorded on the
    /// sandbox so a resume can rebind the same volumes.
    std::map<std::string, std::string> normalized_mounts;
};

/// Rust `mount paths overlap` check — component-wise, so `/mnt/a` does not
/// conflict with `/mnt/ab`.
bool MountPathsOverlap(const std::string& a, const std::string& b);

/// Rust `resolve_volume_mounts`.
///
/// `mounts` maps a requested mount path to a volume reference (id or name).
/// On failure every reservation this call made is released before returning.
core::Expected<ResolvedVolumeMounts, VolumeApiError>
    ResolveVolumeMounts(volume::VolumeManager* manager,
                        const std::map<std::string, std::string>& mounts,
                        const std::string& owner);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_VOLUMES_H_
