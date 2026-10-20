// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/dense_export.rs
#include "agentenv/storage/overlaybd/dense_export.h"

#include <sstream>

#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/layer_metadata.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "internal.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

// ---------------------------------------------------------------------------
// DenseLayerDigest
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> DenseLayerDigest::Absorb(uint64_t offset,
                                                                    const uint8_t* data,
                                                                    size_t len) {
    if (offset != offset_) {
        // Refused rather than seeked to. A hash absorbs bytes in the order it
        // is handed them, so filling a gap later would still hash them late
        // and produce a digest for a byte string that was never stored.
        std::ostringstream o;
        o << "dense overlaybd writer received out-of-order write: got offset " << offset
          << ", expected " << offset_;
        return core::make_unexpected(o.str());
    }
    if (len > 0 && data == 0) {
        return core::make_unexpected(std::string("dense overlaybd writer received null data"));
    }
    if (offset_ > (~static_cast<uint64_t>(0)) - static_cast<uint64_t>(len)) {
        return core::make_unexpected(std::string("dense overlaybd output size overflow"));
    }

    if (len > 0) hasher_.Update(data, len);
    offset_ += static_cast<uint64_t>(len);
    return core::Unit();
}

DenseLayerDescriptor DenseLayerDigest::Descriptor() const {
    DenseLayerDescriptor out;
    // `PeekHex` already carries the `sha256:` prefix, matching Rust's
    // `format!("sha256:{:x}", ..)`.
    out.digest = hasher_.PeekHex();
    out.size = offset_;
    return out;
}

// ---------------------------------------------------------------------------
// DenseDigestSink
// ---------------------------------------------------------------------------

bool DenseDigestSink::WriteAt(uint64_t off, const uint8_t* data, uint64_t len) {
    if (len > static_cast<uint64_t>(~static_cast<size_t>(0))) {
        error_ = "dense overlaybd digest write exceeds addressable range";
        return false;
    }
    const core::Expected<core::Unit, std::string> absorbed =
        digest_.Absorb(off, data, static_cast<size_t>(len));
    if (!absorbed.ok()) {
        // Kept, because `CompactSink::WriteAt` can only report a bool and the
        // compactor would otherwise turn this into a generic "write data
        // failed" with the real reason gone.
        error_ = absorbed.error();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

bool ShouldDenseExportLayer(const std::string& path) {
    const core::Expected<bool, std::string> is_sparse = ReadOverlaybdLayerIsSparseRw(path);
    if (is_sparse.ok()) return is_sparse.value();

    // False on failure, with a warning. A layer whose header cannot be read
    // is uploaded raw: larger, but correct. Assuming sparse instead would
    // hand a non-sparse file to the exporter, which would then fail or
    // produce a layer the registry cannot serve.
    AGENTENV_WARN("failed to inspect overlaybd sparse header; falling back to raw layer "
                  "upload: path="
                  << path << " error=" << is_sparse.error());
    return false;
}

core::Expected<core::Unit, std::string> WriteDenseLayerTo(const std::string& path,
                                                             lsmt::CompactSink* sink) {
    if (sink == 0) {
        return core::make_unexpected(std::string("dense export requires a destination writer"));
    }

    const core::Expected<std::shared_ptr<LocalFile>, std::string> source =
        LocalFile::OpenRo(path);
    if (!source.ok()) {
        return core::make_unexpected("open sparse overlaybd layer '" + path + "': " +
                                     source.error());
    }

    const core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> layer =
        LsmtReadOnlyFile::Open(source.value());
    if (!layer.ok()) {
        return core::make_unexpected("open sealed sparse overlaybd layer '" + path + "': " +
                                     layer.error());
    }

    // `Open` above builds a single-file sealed layer from this path, which is
    // what `CommitPreservingMetadataTo` requires: it refuses a stacked
    // read-only view, because such a view has no single trailer to take the
    // exported object's identity from.
    //
    // Rust additionally pins `args.concurrency = 1` here. There is no
    // counterpart: `lsmt::CompactTo` copies data blocks sequentially by
    // construction (see `lsmt.h`), so the ordering this sink depends on is
    // already guaranteed rather than requested.
    const core::Expected<core::Unit, std::string> committed =
        layer.value()->CommitPreservingMetadataTo(sink);
    if (!committed.ok()) {
        return core::make_unexpected("dense-export sparse overlaybd layer '" + path + "': " +
                                     committed.error());
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> WriteDenseLayerToFile(
    const std::string& path, std::shared_ptr<VirtualFile> dest) {
    if (!dest) {
        return core::make_unexpected(std::string("dense export requires a destination file"));
    }
    VirtualFileCompactSink sink(dest);
    return WriteDenseLayerTo(path, &sink);
}

core::Expected<DenseLayerDescriptor, std::string> DescribeDenseLayer(const std::string& path) {
    DenseDigestSink sink;
    const core::Expected<core::Unit, std::string> written = WriteDenseLayerTo(path, &sink);
    if (!written.ok()) {
        // The sink's own reason wins when it has one: the compactor wraps a
        // sink failure in its generic message, and the out-of-order detail is
        // the part worth reporting.
        if (!sink.Error().empty()) {
            return core::make_unexpected(written.error() + " (" + sink.Error() + ")");
        }
        return core::make_unexpected(written.error());
    }
    return sink.Descriptor();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
