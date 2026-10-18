// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/io/virtual_file.rs
#include "agentenv/storage/overlaybd/virtual_file.h"

#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

/// Rust's `bail!("<op> is not supported")`, kept word for word so a failure
/// here is greppable against the Rust tree.
std::string Unsupported(const char* op) {
    return std::string(op) + " is not supported";
}

}  // namespace

// ---------------------------------------------------------------------------
// VirtualFile defaults
// ---------------------------------------------------------------------------

core::Expected<size_t, std::string> VirtualFile::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                             size_t len) {
    // Rust returns `Ok(0)` for an empty destination rather than reading: a
    // zero-length read is not an error, and issuing one would cost a syscall
    // to copy nothing.
    if (len == 0) return static_cast<size_t>(0);
    if (dst == NULL) {
        return core::make_unexpected(std::string("read_at_into: null destination"));
    }

    const core::Expected<ByteBuffer, std::string> data = ReadAt(offset, len);
    if (!data.ok()) return core::make_unexpected(data.error());

    // `min`, because a short read is a success: the file may simply end here.
    // Copying `len` regardless would hand the caller whatever was already in
    // its buffer and call it data.
    const size_t n = data.value().size() < len ? data.value().size() : len;
    if (n > 0) std::memcpy(dst, &data.value()[0], n);
    return n;
}

core::Expected<ByteBuffer, std::string> VirtualFile::ReadAtWithCtx(const IoCtx& /*ctx*/,
                                                                    uint64_t offset,
                                                                    size_t len) {
    // The context is dropped here by design. An implementation that can use
    // the ring overrides this; a wrapper that holds another file overrides it
    // to pass the context down, because stopping the propagation at any layer
    // drops everything below it to synchronous I/O.
    return ReadAt(offset, len);
}

core::Expected<size_t, std::string> VirtualFile::ReadAtIntoWithCtx(const IoCtx& ctx,
                                                                    uint64_t offset,
                                                                    uint8_t* dst,
                                                                    size_t len) {
    if (len == 0) return static_cast<size_t>(0);
    if (dst == NULL) {
        return core::make_unexpected(std::string("read_at_into_with_ctx: null destination"));
    }
    // Through the ctx-aware read, not the plain one: this is what lets an
    // implementation override a single method and have both variants honour
    // the ring.
    const core::Expected<ByteBuffer, std::string> data = ReadAtWithCtx(ctx, offset, len);
    if (!data.ok()) return core::make_unexpected(data.error());

    const size_t n = data.value().size() < len ? data.value().size() : len;
    if (n > 0) std::memcpy(dst, &data.value()[0], n);
    return n;
}

core::Expected<size_t, std::string> VirtualFile::WriteAtWithCtx(const IoCtx& /*ctx*/,
                                                                 uint64_t offset,
                                                                 const uint8_t* data,
                                                                 size_t len) {
    return WriteAt(offset, data, len);
}

core::Expected<core::Unit, std::string> VirtualFile::Truncate(uint64_t /*size*/) {
    return core::make_unexpected(Unsupported("truncate"));
}

core::Expected<core::Unit, std::string> VirtualFile::Sync() {
    return core::make_unexpected(Unsupported("sync"));
}

core::Expected<core::Optional<uint64_t>, std::string> VirtualFile::SeekData(
    uint64_t /*offset*/) {
    // Absent, not an error: "I cannot tell you where the data is" is a valid
    // answer, and the caller falls back to treating the range as data.
    return core::Optional<uint64_t>();
}

core::Expected<core::Optional<uint64_t>, std::string> VirtualFile::SeekHole(
    uint64_t /*offset*/) {
    return core::Optional<uint64_t>();
}

core::Expected<core::Unit, std::string> VirtualFile::Discard(uint64_t /*offset*/,
                                                              uint64_t /*len*/) {
    return core::make_unexpected(Unsupported("discard"));
}

core::Expected<core::Unit, std::string> VirtualFile::EvictRange(uint64_t /*offset*/,
                                                                 uint64_t /*len*/) {
    // Success, unlike the refusals above. This is a hint about cache
    // residency, and a file that keeps nothing cached has already satisfied
    // it -- reporting an error would make every caller handle a failure that
    // means nothing happened and nothing needed to.
    return core::Unit();
}

core::Expected<core::Unit, std::string> VirtualFile::EvictAll() {
    return core::Unit();
}

core::Expected<ByteBuffer, std::string> VirtualFile::FGetXattr(
    const std::string& /*name*/) {
    // Refused rather than answered with an empty buffer, which the caller
    // would read as "the attribute exists and is blank".
    return core::make_unexpected(Unsupported("fgetxattr"));
}

core::Expected<std::vector<std::string>, std::string> VirtualFile::FListXattr() {
    // Likewise: an empty list means "no attributes set", which is a different
    // claim from "this file cannot have attributes".
    return core::make_unexpected(Unsupported("flistxattr"));
}

core::Expected<core::Unit, std::string> VirtualFile::FSetXattr(const std::string& /*name*/,
                                                                const uint8_t* /*value*/,
                                                                size_t /*len*/,
                                                                int /*flags*/) {
    return core::make_unexpected(Unsupported("fsetxattr"));
}

core::Expected<core::Unit, std::string> VirtualFile::FRemoveXattr(
    const std::string& /*name*/) {
    return core::make_unexpected(Unsupported("fremovexattr"));
}

// ---------------------------------------------------------------------------
// VirtualFileWriter
// ---------------------------------------------------------------------------

VirtualFileWriter::VirtualFileWriter(std::shared_ptr<VirtualFile> file)
    : file_(file), buf_size_(kVirtualFileWriterBufSize) {}

VirtualFileWriter::VirtualFileWriter(std::shared_ptr<VirtualFile> file, size_t buf_size)
    : file_(file), buf_size_(buf_size) {}

core::Expected<util::CompactBuffer, std::string> VirtualFileWriter::AllocBuffer() {
    // Zeroed, matching Rust's `vec![0u8; buf_size]`. A caller that fills only
    // part of the buffer and submits the whole thing writes zeroes rather
    // than whatever the allocator last held there.
    return util::CompactBuffer(buf_size_, 0);
}

core::Expected<core::Unit, std::string> VirtualFileWriter::Write(
    const util::CompactBuffer& buf, uint64_t offset, size_t len) {
    if (len > buf.size()) {
        // Rust's wording. Refused rather than clamped: a caller asking to
        // write more than it filled has miscounted, and writing the shorter
        // amount would commit a silently truncated record.
        return core::make_unexpected(
            std::string("VirtualFileWriter write len exceed buffer range"));
    }
    if (len == 0) return core::Unit();
    if (!file_) {
        return core::make_unexpected(std::string("VirtualFileWriter: no file"));
    }

    const core::Expected<size_t, std::string> written = file_->WriteAt(offset, &buf[0], len);
    if (!written.ok()) return core::make_unexpected(written.error());
    if (written.value() != len) {
        // A short write is a legitimate result from `WriteAt` but a failure
        // here: this writer is laying out a format, and a record that only
        // partly landed leaves the file unparseable with nothing recording
        // where it went wrong.
        std::ostringstream why;
        why << "VirtualFileWriter: short write at offset " << offset << ": expected " << len
            << ", got " << written.value();
        return core::make_unexpected(why.str());
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> VirtualFileWriter::WriteAllAt(const uint8_t* data,
                                                                       size_t len,
                                                                       uint64_t offset) {
    // Overrides the chunking default: a plain file can take the caller's
    // slice as it is, so routing small metadata writes through an allocated
    // buffer would copy for nothing.
    if (len == 0) return core::Unit();
    if (data == NULL) {
        return core::make_unexpected(std::string("VirtualFileWriter: null data"));
    }
    if (!file_) {
        return core::make_unexpected(std::string("VirtualFileWriter: no file"));
    }

    const core::Expected<size_t, std::string> written = file_->WriteAt(offset, data, len);
    if (!written.ok()) return core::make_unexpected(written.error());
    if (written.value() != len) {
        std::ostringstream why;
        why << "VirtualFileWriter: short write at offset " << offset << ": expected " << len
            << ", got " << written.value();
        return core::make_unexpected(why.str());
    }
    return core::Unit();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
