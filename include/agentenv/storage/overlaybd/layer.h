// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/layer/ — LayerMetadata + the stacked read path.
#ifndef AGENTENV_STORAGE_OVERLAYBD_LAYER_H_
#define AGENTENV_STORAGE_OVERLAYBD_LAYER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust struct `LayerMetadata` (layer/layer_metadata.rs).
struct LayerMetadata {
    std::string digest;
    uint64_t    uncompressed_size = 0;
    uint64_t    block_size = 4096;
    bool    compressed = false;
};

/// Rust: a stack of read-only layers, resolved bottom-up. Implements VirtualFile.
class LayerStack : public VirtualFile {
 public:
    virtual ~LayerStack() {}
    /// Push a lower layer (called from bottom to top when building the stack).
    virtual void PushLayer(std::shared_ptr<VirtualFile> layer,
      const LayerMetadata& meta) = 0;
};

std::unique_ptr<LayerStack> MakeLayerStack();

// ---- layer_metadata.rs free functions --------------------------------------

/// Rust `COMMIT_FILE_NAME` / `SEALED_FILE_NAME`. These are a naming
/// convention rather than an implementation detail: anything that *writes* a
/// layer must use one of them for the result to be findable from a
/// `LayerConfig` that carries only a directory.
extern const char* const kCommitFileName;
extern const char* const kSealedFileName;

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

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_LAYER_H_
