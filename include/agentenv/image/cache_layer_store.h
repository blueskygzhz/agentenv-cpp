// SPDX-License-Identifier: MIT
// Rust: src/image/cache/store.rs — `OverlaybdLayerLocation` and the
//       `OverlaybdLayerStore` trait.
//
// Split into its own header purely so the snapshot side can reach it. These
// two declarations previously existed **twice** -- here in `image::cache` (the
// side that implements them, `cache_store.cc`) and again in
// `snapshot::` (`snapshot/runtime_support.h`, the side that consumes them) --
// with byte-identical signatures, where upstream has a single trait. Two
// identical C++ types are still two types, so the implementation could not be
// handed to `RuntimeImageMaterializer` or `PosixFsRuntimeResolver` at all: the
// argument simply would not convert.
//
// Unified here rather than on the snapshot side because this is upstream's
// home for it, and because the library edge only runs one way:
// `agentenv-snapshot` links `agentenv-image`, while `agentenv-image` may only
// reach `agentenv-snapshot-record` (see the comment in `src/image/
// CMakeLists.txt` -- the whole-library edge back would be a cycle).
//
// Deliberately light: `<cstdint>`, `<string>`, `<vector>` and nothing else.
// The obvious homes were both unusable -- `image/cache_store.h` pulls in
// `cache_service.h`, `metadata.h`, `oci_image.h` and `sandbox/backend.h`, and
// `cache_service.h` is heavier still. Including either from
// `snapshot/runtime_support.h` (reached by `snapshot/repository/posixfs/*.h`)
// would drag the whole image cache into every posixfs translation unit to
// obtain two declarations that need none of it.
#ifndef AGENTENV_IMAGE_CACHE_LAYER_STORE_H_
#define AGENTENV_IMAGE_CACHE_LAYER_STORE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace agentenv {
namespace image {
namespace cache {

/// Rust `enum OverlaybdLayerLocation`.
///
/// Rust's enum has no "unset" state; a default-constructed value here is a
/// C++ artifact and must be assigned before it is read. `CacheDir` is the
/// default because it is the conservative one: it tells overlaybd the layer
/// may have to be recovered, whereas a stray `LocalFile` would assert a local
/// copy exists. Nothing relies on it -- both producers
/// (`ImageCacheService::LayerLocation`, `cache_store.cc`) set `kind` on every
/// path -- and the factories below are the intended way in.
struct OverlaybdLayerLocation {
    enum class Kind { LocalFile, CacheDir };

    Kind        kind;
    std::string path;

    OverlaybdLayerLocation() : kind(Kind::CacheDir) {}

    static OverlaybdLayerLocation LocalFile(const std::string& p) {
        OverlaybdLayerLocation loc;
        loc.kind = Kind::LocalFile;
        loc.path = p;
        return loc;
    }
    static OverlaybdLayerLocation CacheDir(const std::string& p) {
        OverlaybdLayerLocation loc;
        loc.kind = Kind::CacheDir;
        loc.path = p;
        return loc;
    }
};

/// Rust `trait OverlaybdLayerStore`.
///
/// Decides where a layer lives on this node. The `has_remote` flag signals
/// that the layer also has a registry URL, so overlaybd can recover it
/// without a local file — allowing the local entry to use `dir=` (reclaimable
/// cache) rather than `file=` (required local copy).
class OverlaybdLayerStore {
 public:
    virtual ~OverlaybdLayerStore() {}
    virtual OverlaybdLayerLocation LayerLocation(const std::string& digest, uint64_t size,
                                                 bool has_remote) const = 0;
    virtual std::vector<std::string> PublishableRoots() const = 0;
};

}  // namespace cache
}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_CACHE_LAYER_STORE_H_
