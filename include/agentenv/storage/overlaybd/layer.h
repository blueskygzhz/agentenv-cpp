// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/layer/layer_metadata.rs
//
// That module is the whole of Rust's `layer/`: two file-name constants and
// three readers over a sealed layer's header/trailer. There is no layer-stack
// type here, and there was never meant to be one — the stacked read path lives
// in `lsmt/` (`ReadOnlyIndex::Merge` composes layers, `ComboIndex` resolves
// upper-over-lower), which is what `lsmt.h` ports.
//
// This header used to also declare `LayerMetadata`, `LayerStack` and
// `MakeLayerStack()`. None of the three existed in the Rust tree. They were
// scaffolding invented during the skeleton phase, and the signature gave it
// away: `PushLayer(VirtualFile, LayerMetadata)` takes no index, so it could
// not have performed an LSMT merge even in principle. Worse, that
// `LayerMetadata` collided by name with the real `overlaybd::p2p::LayerMetadata`
// (`overlaybd/p2p/artifact.rs`), which *is* ported and widely used.
#ifndef AGENTENV_STORAGE_OVERLAYBD_LAYER_H_
#define AGENTENV_STORAGE_OVERLAYBD_LAYER_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `COMMIT_FILE_NAME` / `SEALED_FILE_NAME`. These are a naming
/// convention rather than an implementation detail: anything that *writes* a
/// layer must use one of them for the result to be findable from a
/// `LayerConfig` that carries only a directory.
extern const char* const kCommitFileName;
extern const char* const kSealedFileName;

/// Rust `resolve_local_layer_path`.
///
/// Where a layer's bytes are on this host, or absent when they are not local.
///
/// An explicit `file` wins, but only when it is a path: a `file` containing
/// `://` names a remote blob, and returning it would hand a URL to something
/// that is about to `open(2)` it. With only a `dir`, the two conventional file
/// names are probed in order — commit before sealed, as Rust does — and the
/// probe is by existence, so a directory holding neither yields nothing rather
/// than a path that cannot be opened.
core::Optional<std::string> ResolveLocalLayerPath(const LayerConfig& layer);

/// Rust `read_overlaybd_layer_uuid`.
///
/// The uuid lives in the sealed trailer as a NUL-terminated ASCII string. Rust
/// maps both "no uuid" and "unparseable uuid" onto `Uuid::nil()`, so callers
/// only have to test for nil; this port keeps that, returning an empty string
/// for the nil case. An unreadable or non-sealed file is an error, not nil.
core::Expected<std::string, std::string>
    ReadOverlaybdLayerUuid(const std::string& path);

/// Rust `read_overlaybd_layer_virtual_size`.
core::Expected<uint64_t, std::string>
    ReadOverlaybdLayerVirtualSize(const std::string& path);

/// Rust `read_overlaybd_layer_is_sparse_rw`.
///
/// Reads the **header** at offset 0, not the trailer. That is the difference
/// that matters: the sparse-RW flag is written when the layer is created, and
/// an RW layer has no sealed trailer to read at all — asking for the trailer
/// would fail on exactly the layers this predicate exists to identify.
core::Expected<bool, std::string>
    ReadOverlaybdLayerIsSparseRw(const std::string& path);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_LAYER_H_
