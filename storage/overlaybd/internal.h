// SPDX-License-Identifier: MIT
// Implementation-only helpers shared between the overlaybd translation units.
// Not part of the installed surface: these are the adapters that bind the
// `lsmt::CompactTo` primitive to the io layer, which `lsmt.h` deliberately
// does not depend on.
#ifndef AGENTENV_STORAGE_OVERLAYBD_INTERNAL_H_
#define AGENTENV_STORAGE_OVERLAYBD_INTERNAL_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust: the `&[Arc<dyn VirtualFile>]` src_layers that `compact_to` indexes by
/// a mapping's tag.
///
/// The tag is the index into the same vector `OpenFilesRo` built, so this is
/// the one place that invariant is cashed in. An out-of-range tag is a corrupt
/// index and is refused rather than clamped: clamping would silently read
/// another layer's bytes into the compacted output.
///
/// Holds the vector by reference: every caller passes a live member of the
/// file being compacted, and copying the handles would only add refcount
/// traffic. The reference must outlive the `CompactTo` call.
class LayerStackCompactSource : public lsmt::CompactSource {
 public:
    explicit LayerStackCompactSource(const std::vector<std::shared_ptr<VirtualFile> >& layers)
        : layers_(layers) {}

    bool ReadAt(uint8_t tag, uint64_t src_byte_off, uint8_t* dst, uint64_t len) {
        if (static_cast<size_t>(tag) >= layers_.size()) return false;
        VirtualFile* file = layers_[tag].get();
        if (file == 0) return false;
        const core::Expected<size_t, std::string> got =
            file->ReadAtInto(src_byte_off, dst, static_cast<size_t>(len));
        // A short read is a failure, not a partial success: the compacted
        // output would otherwise carry whatever was already in the buffer.
        return got.ok() && got.value() == static_cast<size_t>(len);
    }

 private:
    const std::vector<std::shared_ptr<VirtualFile> >& layers_;
};

/// Rust `VirtualFileWriter` — binds a `CompactSink` to an open file.
///
/// The file is written but neither synced nor closed; its owner decides when
/// the bytes have to be durable, which is what lets a caller stage a
/// compacted layer and then upload it.
class VirtualFileCompactSink : public lsmt::CompactSink {
 public:
    explicit VirtualFileCompactSink(std::shared_ptr<VirtualFile> file) : file_(file) {}

    bool WriteAt(uint64_t off, const uint8_t* data, uint64_t len) {
        if (!file_) return false;
        uint64_t done = 0;
        while (done < len) {
            const core::Expected<size_t, std::string> put =
                file_->WriteAt(off + done, data + done, static_cast<size_t>(len - done));
            if (!put.ok()) return false;
            // Rust's `write_all_at` loops on a short write but cannot make
            // progress on a zero-length one; treat that as the failure it is
            // rather than spinning.
            if (put.value() == 0) return false;
            done += static_cast<uint64_t>(put.value());
        }
        return true;
    }

 private:
    std::shared_ptr<VirtualFile> file_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_INTERNAL_H_
