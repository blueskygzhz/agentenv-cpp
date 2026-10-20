// SPDX-License-Identifier: MIT
// Implementation-only helpers shared between the overlaybd translation units.
// Not part of the installed surface: these are the adapters that bind the
// `lsmt::CompactTo` primitive to the io layer, which `lsmt.h` deliberately
// does not depend on.
#ifndef AGENTENV_STORAGE_OVERLAYBD_INTERNAL_H_
#define AGENTENV_STORAGE_OVERLAYBD_INTERNAL_H_

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
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

/// Rust `helper.rs :: parse_uuid_field` — read a NUL-padded uuid slot.
///
/// Reports absence for an empty or unparseable field, which is what lets a
/// commit distinguish "this layer names no parent" from "this layer names a
/// parent whose stamp is corrupt". Returns the canonical spelling rather than
/// the raw bytes, matching Rust's parse-then-`to_string` round trip.
inline core::Optional<std::string> ParseUuidField(const uint8_t* field, size_t len) {
    size_t end = 0;
    while (end < len && field[end] != 0) ++end;
    if (end == 0) return core::Optional<std::string>();
    const std::string text(reinterpret_cast<const char*>(field), end);
    core::Uuid parsed;
    if (!core::Uuid::Parse(text, &parsed)) return core::Optional<std::string>();
    return core::Optional<std::string>(parsed.ToString());
}

/// Rust `helper.rs :: parse_user_tag_field` — read a NUL-padded opaque tag.
///
/// Unlike the uuid slot the bytes are not interpreted, so only emptiness is
/// an absence here.
inline core::Optional<std::vector<uint8_t> > ParseUserTagField(const uint8_t* field, size_t len) {
    size_t end = 0;
    while (end < len && field[end] != 0) ++end;
    if (end == 0) return core::Optional<std::vector<uint8_t> >();
    return core::Optional<std::vector<uint8_t> >(std::vector<uint8_t>(field, field + end));
}

/// Rust `vfile_io.rs :: DirectRead` — the plain per-layer read primitive.
///
/// Both LSMT resolvers (`LsmtReadOnlyFile` and `LsmtFile`) are templates over
/// this shape rather than plain functions, so that the ctx variant runs the
/// *same* hole-and-mapping arithmetic. A second copy written to add a context
/// parameter is exactly where the two paths would drift, and the drift would
/// surface as a hole read out of the wrong layer.
struct DirectLayerRead {
    core::Expected<size_t, std::string> ReadInto(VirtualFile* file, uint64_t offset,
                                                   uint8_t* dst, size_t len) const {
        return file->ReadAtInto(offset, dst, len);
    }
};

/// Rust `vfile_io.rs :: CtxRead` — the same primitive, submitted on `ctx`.
///
/// Holds the context by pointer: `IoCtx` is a lightweight handle to a ring
/// owned by the caller's thread, and copying it per read would be a copy of
/// something this code must not own anyway.
struct CtxLayerRead {
    explicit CtxLayerRead(const IoCtx& ctx) : ctx(&ctx) {}

    core::Expected<size_t, std::string> ReadInto(VirtualFile* file, uint64_t offset,
                                                   uint8_t* dst, size_t len) const {
        return file->ReadAtIntoWithCtx(*ctx, offset, dst, len);
    }

    const IoCtx* ctx;
};

/// Rust `read_exact`, generic in the reader strategy.
///
/// One implementation for both strategies: they differ only in `ReadInto`, so
/// a per-strategy body here would be a second place to drift.
template <typename Reader>
inline core::Expected<core::Unit, std::string> ReadExactWith(const Reader& reader,
                                                              VirtualFile* file, uint64_t offset,
                                                              uint8_t* dst, size_t len) {
    size_t done = 0;
    while (done < len) {
        const core::Expected<size_t, std::string> n =
            reader.ReadInto(file, offset + done, dst + done, len - done);
        if (!n.ok()) return core::make_unexpected(n.error());
        if (n.value() == 0) {
            // A short read is a success at the `VirtualFile` boundary but a
            // failure here: the index said these bytes are data, so handing
            // back the zeros already in the buffer would silently substitute
            // a hole for whatever the layer was supposed to hold.
            std::ostringstream o;
            o << "unexpected EOF: still need " << (len - done) << " byte(s)";
            return core::make_unexpected(o.str());
        }
        done += n.value();
    }
    return core::Unit();
}

/// Rust `vfile_io.rs :: DirectWrite` / `CtxWrite` — the write counterpart.
///
/// The same reasoning as the read pair: `LsmtFile::WriteInternal` is a long
/// piece of hybrid/sparse placement logic, and a second copy written to add
/// `ctx` is where the two would diverge.
struct DirectLayerWrite {
    core::Expected<size_t, std::string> WriteInto(VirtualFile* file, uint64_t offset,
                                                    const uint8_t* data, size_t len) const {
        return file->WriteAt(offset, data, len);
    }
};

struct CtxLayerWrite {
    explicit CtxLayerWrite(const IoCtx& ctx) : ctx(&ctx) {}

    core::Expected<size_t, std::string> WriteInto(VirtualFile* file, uint64_t offset,
                                                    const uint8_t* data, size_t len) const {
        return file->WriteAtWithCtx(*ctx, offset, data, len);
    }

    const IoCtx* ctx;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_INTERNAL_H_
