// SPDX-License-Identifier: MIT
// The Roaring bitmap portable serialization format.
//
// Rust reaches this through the `roaring` crate (`RoaringBitmap`), used by
// `backend/cache/meta.rs` to record which blocks of a cached file are
// present. The format is a published specification shared with CRoaring, so
// this is an implementation of that spec rather than a port of the crate:
// what has to match is the bytes on disk, which both sides agree on.
//
// Only the 32-bit flavour is here, which is what `RoaringBitmap` is.
#ifndef AGENTENV_STORAGE_OVERLAYBD_ROARING_H_
#define AGENTENV_STORAGE_OVERLAYBD_ROARING_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// A set of `uint32_t`, stored as one container per high 16 bits.
///
/// The split is what makes the format compact: a cache bitmap is dense within
/// a file's block range and empty everywhere else, so only the occupied
/// 64K-wide buckets cost anything.
///
/// Containers are kept as a plain sorted value list per bucket here rather
/// than switching representation internally. The *serialized* form still
/// chooses array or bitset per container exactly as the spec requires -- that
/// choice is about the bytes on disk, not about how the set is held in memory,
/// and the bitmaps this serves are small enough that the in-memory form does
/// not matter.
class RoaringBitmap {
 public:
    RoaringBitmap() {}

    void Insert(uint32_t value);
    void Remove(uint32_t value);
    bool Contains(uint32_t value) const;
    void Clear() { buckets_.clear(); }

    /// Number of values in the set. Named `Cardinality` rather than `Size` to
    /// keep it distinct from the serialized byte length.
    uint64_t Cardinality() const;
    bool IsEmpty() const;

    /// All values in ascending order. For iteration over a cache bitmap, which
    /// is at most a few thousand blocks.
    std::vector<uint32_t> ToVector() const;

    /// Rust `RoaringBitmap::range(lo..=hi)` — values within an inclusive
    /// range, ascending.
    ///
    /// Used by the cache to find which blocks of a requested read are already
    /// present. Narrowing here rather than filtering `ToVector` matters for a
    /// large bitmap: a read touches a handful of blocks out of possibly tens
    /// of thousands.
    std::vector<uint32_t> Range(uint32_t lo, uint32_t hi) const;

    /// Rust `RoaringBitmap::serialize_into` — the portable format.
    ///
    /// Run containers are never emitted. The Rust side does not run-optimize
    /// either, and emitting them would change the bytes for a bitmap the other
    /// implementation would have written without.
    std::vector<uint8_t> Serialize() const;

    /// Rust `RoaringBitmap::serialized_size`.
    size_t SerializedSize() const;

    /// Rust `RoaringBitmap::deserialize_from`.
    ///
    /// Accepts run containers even though `Serialize` never writes them: a
    /// bitmap written by CRoaring (or by a future run-optimizing writer) is
    /// still a valid bitmap, and refusing it would make the format one-way.
    ///
    /// `consumed` reports how many bytes the bitmap occupied, so a caller
    /// parsing a larger record can find what follows it. Rust gets this from
    /// its `Read` cursor position.
    static core::Expected<RoaringBitmap, std::string> Deserialize(const uint8_t* data,
                                                                     size_t len,
                                                                     size_t* consumed);

 private:
    /// high 16 bits -> sorted low 16 bits, no duplicates.
    std::map<uint16_t, std::vector<uint16_t> > buckets_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_ROARING_H_
