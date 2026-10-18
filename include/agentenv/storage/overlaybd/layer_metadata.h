// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/layer/layer_metadata.rs — reading what a sealed
// layer declares about itself.
#ifndef AGENTENV_STORAGE_OVERLAYBD_LAYER_METADATA_H_
#define AGENTENV_STORAGE_OVERLAYBD_LAYER_METADATA_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `COMMIT_FILE_NAME` / `SEALED_FILE_NAME` — the names probed inside a
/// layer's `dir`, in this order.
///
/// Public because they are a naming convention rather than an implementation
/// detail: anything that *writes* a layer has to use one of these for the
/// result to be findable from a `LayerConfig` carrying only a directory.
/// `kCommitFileName` is what to write; `kSealedFileName` is only ever read,
/// for layers produced before that name settled.
extern const char* const kCommitFileName;
extern const char* const kSealedFileName;

/// Rust `resolve_local_layer_path`.
///
/// Returns nothing when the layer is remote or cannot be located. An explicit
/// `file` wins over `dir`, and a `file` containing `://` is reported as absent
/// rather than treated as a path — it is a URL, and opening it as a local file
/// would produce a confusing "no such file" instead of routing it to the
/// remote path.
core::Optional<std::string> ResolveLocalLayerPath(const LayerConfig& layer);

/// Rust `read_overlaybd_layer_virtual_size` — the logical size from the
/// trailer.
core::Expected<uint64_t, std::string> ReadOverlaybdLayerVirtualSize(
    const std::string& path);

/// Rust `read_overlaybd_layer_uuid`.
///
/// An unparseable or absent uuid reads back as the nil uuid rather than an
/// error: the field is informational, and a layer that predates uuid stamping
/// is still perfectly readable.
core::Expected<std::string, std::string> ReadOverlaybdLayerUuid(const std::string& path);

/// Rust `read_overlaybd_layer_is_sparse_rw` — from the *header*.
///
/// The header, not the trailer: this says how the layer's data is laid out,
/// which is fixed when the file is created, whereas the trailer describes the
/// finished result.
core::Expected<bool, std::string> ReadOverlaybdLayerIsSparseRw(const std::string& path);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_LAYER_METADATA_H_
