// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/dense_export.rs
#ifndef AGENTENV_STORAGE_OVERLAYBD_DENSE_EXPORT_H_
#define AGENTENV_STORAGE_OVERLAYBD_DENSE_EXPORT_H_

#include <cstdint>
#include <string>

#include "agentenv/core/digest.h"
#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `DENSE_EXPORT_BUFFER_SIZE`.
///
/// Only a buffer hint in Rust, where `CompactWriter::alloc_buffer` hands the
/// compactor a buffer of this size. `lsmt::CompactTo` here allocates per
/// mapping instead, so this port has nothing to size -- kept because it is
/// part of the module's observable shape and a future buffered sink would
/// want the same number.
const uint64_t kDenseExportBufferSize = 8ull * 1024 * 1024;

/// Rust `DenseLayerDescriptor`.
struct DenseLayerDescriptor {
    std::string digest;
    uint64_t size;

    DenseLayerDescriptor() : size(0) {}
};

/// Rust `DenseLayerDigest` — the digest and size of a dense overlaybd byte
/// stream.
///
/// Bytes must be fed in strictly increasing offset order, and a gap or a
/// repeat is **refused**, not tolerated. That is what makes a streaming digest
/// equal to the digest of the finished object: a hash absorbs bytes in the
/// order it is given them, so accepting an out-of-order write would produce a
/// digest for a byte string nobody ever stored. Refusing lets an upload path
/// verify exactly the bytes it emitted without buffering the whole layer.
class DenseLayerDigest {
 public:
    DenseLayerDigest() : offset_(0) {}

    /// Rust `absorb`.
    core::Expected<core::Unit, std::string> Absorb(uint64_t offset, const uint8_t* data,
                                                      size_t len);

    /// Rust `descriptor` — the running digest and byte count.
    ///
    /// Callable mid-stream: `core::Sha256Stream::PeekHex` finalises a copy of
    /// the state, matching Rust's `hasher.clone().finalize()`.
    DenseLayerDescriptor Descriptor() const;

 private:
    DenseLayerDigest(const DenseLayerDigest&);
    DenseLayerDigest& operator=(const DenseLayerDigest&);

    core::Sha256Stream hasher_;
    uint64_t offset_;
};

/// Rust `DigestCompactWriter` — a `CompactSink` that only hashes.
///
/// Rust guards its state with a mutex because `CompactWriter` is shared across
/// the compactor's in-flight writes. Not needed here: `lsmt::CompactTo` copies
/// data blocks sequentially (see `lsmt.h`), which is Rust's `concurrency = 1`,
/// and `dense_export.rs` pins that value explicitly for exactly this sink. A
/// lock would suggest a concurrency this port does not have.
class DenseDigestSink : public lsmt::CompactSink {
 public:
    DenseDigestSink() {}

    bool WriteAt(uint64_t off, const uint8_t* data, uint64_t len);

    DenseLayerDescriptor Descriptor() const { return digest_.Descriptor(); }

    /// Why the write failed, when `WriteAt` reported false.
    ///
    /// `CompactSink::WriteAt` is a bool, so the reason would otherwise be lost
    /// and surface as the compactor's generic "write data failed".
    const std::string& Error() const { return error_; }

 private:
    DenseDigestSink(const DenseDigestSink&);
    DenseDigestSink& operator=(const DenseDigestSink&);

    DenseLayerDigest digest_;
    std::string error_;
};

/// Rust `should_dense_export_layer`.
///
/// False when the header cannot be inspected, with a warning: a layer that
/// cannot be classified is uploaded raw, which is correct but larger. Treating
/// an unreadable header as sparse would instead feed a non-sparse file to the
/// exporter.
bool ShouldDenseExportLayer(const std::string& path);

/// Rust `write_dense_layer_to` — rewrite a sealed sparse layer into `sink`.
core::Expected<core::Unit, std::string> WriteDenseLayerTo(const std::string& path,
                                                             lsmt::CompactSink* sink);

/// The same, writing to a file.
///
/// Rust reaches this by handing `CommitArgs::new(file).writer` to
/// `write_dense_layer_to`, which is the file-backed `CompactWriter`. The
/// equivalent adapter here lives in a private header, so this overload is how
/// a caller outside the library writes a dense layer to a path at all.
core::Expected<core::Unit, std::string> WriteDenseLayerToFile(
    const std::string& path, std::shared_ptr<VirtualFile> dest);

/// Rust `describe_dense_layer` — the digest and size the dense form would
/// have, without storing it.
core::Expected<DenseLayerDescriptor, std::string> DescribeDenseLayer(const std::string& path);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_DENSE_EXPORT_H_
