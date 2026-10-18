// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/image/helper.rs
//
// Path rewriting for overlaybd image configs, plus the one call that lays down
// a fresh runtime upper. These sit between the config structs and `ImageFile`:
// a config is written relative to wherever it was produced, and has to be
// rewritten before a different directory can load it.
#ifndef AGENTENV_STORAGE_OVERLAYBD_IMAGE_HELPER_H_
#define AGENTENV_STORAGE_OVERLAYBD_IMAGE_HELPER_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `resolve_config_path` — turn a config-relative path into an absolute
/// one against `base`.
///
/// Lexical only: an overlaybd config may name files that do not exist yet, so
/// resolving through the filesystem would reject a valid config -- and it
/// would also follow symlinks, changing the layout that later gets persisted.
core::Expected<std::string, std::string> ResolveConfigPath(const std::string& base,
                                                            const std::string& raw);

/// Rust `relative_path` — express `to_path` relative to `from_dir`.
///
/// Returns `.` when the two are the same path, because an empty string is not
/// a usable path and would be read as "the current directory" only by
/// accident.
core::Expected<std::string, std::string> RelativePath(const std::string& from_dir,
                                                        const std::string& to_path);

/// Rust `LOWER_PATH_KEYS` — the lower fields that name a local file.
///
/// Only these four are rewritten. A lower also carries `digest`, `size` and
/// `repoBlobUrl`, none of which is a path, and rewriting them would corrupt
/// the layer's identity.
extern const char* const kLowerPathKeys[4];

/// Rust `rewrite_overlaybd_lower_paths` — retarget every lower path in
/// `image_config` so a loader running in `runtime_dir` resolves the same files
/// that `source_base` did.
///
/// Three things are left alone, each for its own reason:
///
///  * An **empty** value, which means the field is unset rather than pointing
///    at the config's own directory.
///  * Anything containing `://`, which is a URL -- rewriting it as a path
///    would destroy the only way to fetch that layer.
///  * A lower with **no** path keys at all (a pure digest reference), which
///    has nothing local to retarget.
///
/// Mutates in place and reports the first failure. A partially rewritten
/// config is not handed back as success: half its lowers would resolve against
/// one directory and half against another.
core::Expected<core::Unit, std::string> RewriteOverlaybdLowerPaths(
    core::Json* image_config, const std::string& source_base,
    const std::string& runtime_dir);

/// Rust `prepare_runtime_upper` — create the upper's files for `mode`.
///
/// `index_path` is unused by a sparse upper and required by the other two;
/// see `InitializeFileRwPaths` for why the absent case is refused rather than
/// defaulted.
core::Expected<core::Unit, std::string> PrepareRuntimeUpper(
    const std::string& data_path, const core::Optional<std::string>& index_path,
    uint64_t virtual_size, UpperMode mode);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_IMAGE_HELPER_H_
