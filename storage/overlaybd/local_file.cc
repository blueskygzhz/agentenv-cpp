// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/local.rs
#include "agentenv/storage/overlaybd/local_file.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

#include "agentenv/storage/overlaybd/sys.h"
#include "agentenv/storage/util/aligned_buffer.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

std::string Errno(const char* what, int err) {
    std::ostringstream o;
    o << what << ": " << std::strerror(err);
    return o.str();
}

std::string ErrnoAt(const char* what, const std::string& path, int err) {
    std::ostringstream o;
    o << what << " " << path << ": " << std::strerror(err);
    return o.str();
}

}  // namespace

uint64_t AlignDown(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    // Saturating, matching Rust. Wrapping here would round a value within
    // `alignment` of the maximum down to zero, and the caller would then read
    // or punch from the start of the file instead of the end.
    const uint64_t max = ~static_cast<uint64_t>(0);
    const uint64_t bumped = (value > max - (alignment - 1)) ? max : value + (alignment - 1);
    return bumped & ~(alignment - 1);
}

// ---------------------------------------------------------------------------
// LocalFileBuilder
// ---------------------------------------------------------------------------

LocalFileBuilder::LocalFileBuilder()
    : read_(true), write_(true), create_(true), truncate_(false), mode_(0644),
      direct_io_(false) {}

LocalFileBuilder& LocalFileBuilder::Read(bool v) { read_ = v; return *this; }
LocalFileBuilder& LocalFileBuilder::Write(bool v) { write_ = v; return *this; }
LocalFileBuilder& LocalFileBuilder::Create(bool v) { create_ = v; return *this; }
LocalFileBuilder& LocalFileBuilder::Truncate(bool v) { truncate_ = v; return *this; }
LocalFileBuilder& LocalFileBuilder::Mode(uint32_t v) { mode_ = v; return *this; }
LocalFileBuilder& LocalFileBuilder::DirectIo(bool v) { direct_io_ = v; return *this; }

core::Expected<std::shared_ptr<LocalFile>, std::string> LocalFileBuilder::Open(
    const std::string& path) const {
    int flags = 0;
    if (read_ && write_) {
        flags = O_RDWR;
    } else if (write_) {
        flags = O_WRONLY;
    } else {
        flags = O_RDONLY;
    }
    if (create_) flags |= O_CREAT;
    if (truncate_) flags |= O_TRUNC;
    // Not inherited across exec. A VMM spawned from this process would
    // otherwise hold a descriptor to every image file open at the time.
    flags |= O_CLOEXEC;

    if (direct_io_) {
        const core::Optional<int> extra = sys::DirectIoOpenFlag();
        if (extra.has_value()) flags |= *extra;
    }

    const int fd = ::open(path.c_str(), flags, static_cast<mode_t>(mode_));
    if (fd < 0) {
        return core::make_unexpected(ErrnoAt("open", path, errno));
    }

    if (direct_io_) {
        const core::Expected<core::Unit, sys::SysError> on = sys::EnableDirectIo(fd);
        if (!on.ok()) {
            ::close(fd);
            return core::make_unexpected(std::string("enable direct io on ") + path + ": " +
                                         on.error().Message());
        }
    }

    std::shared_ptr<LocalFile> f(new LocalFile());
    f->path_ = path;
    f->fd_ = fd;
    f->direct_io_ = direct_io_;
    return f;
}

// ---------------------------------------------------------------------------
// LocalFile construction
// ---------------------------------------------------------------------------

LocalFile::LocalFile() : fd_(-1), direct_io_(false), write_calls_(0) {}

LocalFile::~LocalFile() {
    if (fd_ >= 0) ::close(fd_);
}

LocalFileBuilder LocalFile::Builder() { return LocalFileBuilder(); }

core::Expected<std::shared_ptr<LocalFile>, std::string> LocalFile::New(
    const std::string& path) {
    return Builder().Truncate(true).Open(path);
}

core::Expected<std::shared_ptr<LocalFile>, std::string> LocalFile::OpenRo(
    const std::string& path) {
    return Builder().Write(false).Create(false).Open(path);
}

core::Expected<std::shared_ptr<LocalFile>, std::string> LocalFile::OpenRw(
    const std::string& path, bool create) {
    return Builder().Create(create).Open(path);
}

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------

core::Expected<size_t, std::string> LocalFile::ReadIntoAt(uint64_t offset, uint8_t* buf,
                                                            size_t len) {
    size_t read = 0;
    while (read < len) {
        const ssize_t n = ::pread(fd_, buf + read, len - read, static_cast<off_t>(offset + read));
        // Zero means end of file. Breaking rather than erroring is what makes
        // a short read a success -- the caller compares the count it gets
        // back against what it asked for. (`break` and `return read` are
        // interchangeable here; the loop has nothing left to do either way.)
        if (n == 0) break;
        if (n > 0) {
            read += static_cast<size_t>(n);
            continue;
        }
        // Retried rather than reported: a signal arriving mid-read is not a
        // failure of the read, and surfacing it would make every I/O racy
        // with any signal the process happens to take.
        if (errno == EINTR) continue;
        return core::make_unexpected(Errno("pread", errno));
    }
    return read;
}

core::Expected<ByteBuffer, std::string> LocalFile::ReadBufferedAt(uint64_t offset,
                                                                    size_t len) {
    ByteBuffer buf(len, 0);
    const core::Expected<size_t, std::string> n = ReadIntoAt(offset, &buf[0], len);
    if (!n.ok()) return core::make_unexpected(n.error());
    buf.resize(n.value());
    return buf;
}

core::Expected<ByteBuffer, std::string> LocalFile::ReadDirectAt(uint64_t offset,
                                                                  size_t len) {
    // `O_DIRECT` requires the offset, the length and the buffer address to be
    // aligned, so a read of an arbitrary range becomes: widen to the
    // enclosing aligned window, read that, hand back the slice the caller
    // asked for.
    struct stat st;
    if (::fstat(fd_, &st) != 0) return core::make_unexpected(Errno("fstat", errno));
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    if (offset >= size) return ByteBuffer();

    // Clamped to what the file holds, so the aligned window does not extend
    // past the end and the read does not come back short for a reason the
    // caller would misread as EOF mid-range.
    //
    // This clamp and the `min` on `tail` below mask each other: with either
    // one present the slice comes out right, because the clamp keeps the
    // window inside the file and the `min` trims whatever the window
    // overshot. Removing *both* is caught -- a read crossing the end then
    // returns the full requested length, padded with whatever the aligned
    // window picked up past EOF. Keeping both means the length is bounded
    // before the allocation as well as after the read, so an over-long
    // request does not allocate and read a window far larger than the file.
    const uint64_t avail = size - offset;
    const size_t expected = avail < static_cast<uint64_t>(len) ? static_cast<size_t>(avail) : len;

    const uint64_t alignment = static_cast<uint64_t>(kDirectIoAlignment);
    const uint64_t aligned_offset = AlignDown(offset, alignment);
    const uint64_t head_u = offset - aligned_offset;
    const uint64_t aligned_end = AlignUp(offset + static_cast<uint64_t>(expected), alignment);
    const uint64_t aligned_len_u = aligned_end - aligned_offset;
    if (aligned_len_u > static_cast<uint64_t>(static_cast<size_t>(-1))) {
        return core::make_unexpected(std::string("aligned read length overflow"));
    }
    const size_t head = static_cast<size_t>(head_u);
    const size_t aligned_len = static_cast<size_t>(aligned_len_u);

    core::Expected<util::AlignedBuffer, std::string> ab =
        util::AlignedBuffer::New(aligned_len, kDirectIoAlignment);
    if (!ab.ok()) return core::make_unexpected(ab.error());
    util::AlignedBuffer buffer = ab.take_value();

    const core::Expected<size_t, std::string> got =
        ReadIntoAt(aligned_offset, buffer.Data(), aligned_len);
    if (!got.ok()) return core::make_unexpected(got.error());

    // The read stopped before reaching the caller's range at all. Empty, not
    // an error: this is EOF landing inside the alignment padding.
    //
    // Unreachable on its own while the `offset >= size` guard above stands:
    // that guard gives `offset < size`, and the aligned window always spans
    // at least `head + expected`, so a full read returns more than `head`.
    // What is left is the race -- the file shrinking between the `fstat` and
    // the `pread`, or a short read -- which no single-threaded test can
    // stage. Removing it together with the size guard *is* caught.
    if (got.value() <= head) return ByteBuffer();

    const size_t want_end = head + expected;
    const size_t tail = want_end < got.value() ? want_end : got.value();
    // Rust hands the aligned buffer onward as the backing of the returned
    // `Bytes`, with no copy. A `std::vector` result cannot adopt foreign
    // storage, so this copies. The alternative -- returning the aligned
    // buffer itself -- would push its alignment contract onto every caller of
    // `ReadAt`, including the ones reading buffered files.
    return ByteBuffer(buffer.Data() + head, buffer.Data() + tail);
}

core::Expected<ByteBuffer, std::string> LocalFile::ReadAt(uint64_t offset, size_t len) {
    if (len == 0) return ByteBuffer();
    if (direct_io_) return ReadDirectAt(offset, len);
    return ReadBufferedAt(offset, len);
}

core::Expected<size_t, std::string> LocalFile::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                            size_t len) {
    if (len == 0) return static_cast<size_t>(0);
    if (dst == 0) {
        return core::make_unexpected(std::string("read_at_into: null destination"));
    }
    if (direct_io_) {
        // Via the aligned path and then a copy: the caller's buffer has no
        // alignment guarantee, and handing an unaligned address to an
        // `O_DIRECT` read is rejected by the kernel outright.
        const core::Expected<ByteBuffer, std::string> data = ReadDirectAt(offset, len);
        if (!data.ok()) return core::make_unexpected(data.error());
        const size_t n = data.value().size() < len ? data.value().size() : len;
        if (n > 0) std::memcpy(dst, &data.value()[0], n);
        return n;
    }
    return ReadIntoAt(offset, dst, len);
}

// ---------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> LocalFile::WriteAllAt(uint64_t offset,
                                                                const uint8_t* buf,
                                                                size_t len) {
    size_t done = 0;
    while (done < len) {
        const ssize_t n =
            ::pwrite(fd_, buf + done, len - done, static_cast<off_t>(offset + done));
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) {
            // Not retried. A zero-length write that was asked to move bytes
            // makes no progress, so looping would spin forever; reporting it
            // as a failure is the only outcome that terminates.
            return core::make_unexpected(std::string("pwrite made no progress"));
        }
        return core::make_unexpected(Errno("pwrite", errno));
    }
    return core::Unit();
}

core::Expected<size_t, std::string> LocalFile::WriteAtSyncPwrite(uint64_t offset,
                                                                   const uint8_t* buf,
                                                                   size_t len) {
    ++write_calls_;
    const core::Expected<core::Unit, std::string> w = WriteAllAt(offset, buf, len);
    if (!w.ok()) return core::make_unexpected(w.error());
    return len;
}

core::Expected<size_t, std::string> LocalFile::WriteAt(uint64_t offset,
                                                         const uint8_t* data, size_t len) {
    if (len == 0) return static_cast<size_t>(0);
    if (data == 0) {
        return core::make_unexpected(std::string("write_at: null data"));
    }

    if (direct_io_) {
        // Checked here rather than left to the kernel so the error names what
        // is wrong. `O_DIRECT` rejects a misaligned write with a bare EINVAL,
        // which is indistinguishable from a dozen other causes.
        if (offset % static_cast<uint64_t>(kDirectIoAlignment) != 0) {
            std::ostringstream o;
            o << "write_at_sync: offset " << offset << " is not aligned to "
              << kDirectIoAlignment;
            return core::make_unexpected(o.str());
        }
        if (len % kDirectIoAlignment != 0) {
            std::ostringstream o;
            o << "write_at_sync: buf len " << len << " is not aligned to "
              << kDirectIoAlignment;
            return core::make_unexpected(o.str());
        }
    }

    ++write_calls_;

    if (direct_io_ &&
        reinterpret_cast<uintptr_t>(data) % kDirectIoAlignment != 0) {
        // The offset and length were the caller's to get right; the buffer
        // address often is not -- it may be the interior of a larger
        // allocation. Bouncing through an aligned copy keeps that from being
        // the caller's problem, at the cost of one copy on a path that only
        // triggers when the address happens to be unaligned.
        core::Expected<util::AlignedBuffer, std::string> ab =
            util::AlignedBuffer::New(len, kDirectIoAlignment);
        if (!ab.ok()) return core::make_unexpected(ab.error());
        util::AlignedBuffer aligned = ab.take_value();
        std::memcpy(aligned.Data(), data, len);
        const core::Expected<core::Unit, std::string> w =
            WriteAllAt(offset, aligned.Data(), len);
        if (!w.ok()) return core::make_unexpected(w.error());
        return len;
    }

    const core::Expected<core::Unit, std::string> w = WriteAllAt(offset, data, len);
    if (!w.ok()) return core::make_unexpected(w.error());
    return len;
}

// ---------------------------------------------------------------------------
// metadata and hints
// ---------------------------------------------------------------------------

core::Expected<uint64_t, std::string> LocalFile::Size() {
    struct stat st;
    if (::fstat(fd_, &st) != 0) return core::make_unexpected(Errno("fstat", errno));
    return static_cast<uint64_t>(st.st_size);
}

core::Expected<core::Unit, std::string> LocalFile::Truncate(uint64_t size) {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        return core::make_unexpected(Errno("ftruncate", errno));
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> LocalFile::Sync() {
    if (::fsync(fd_) != 0) return core::make_unexpected(Errno("fsync", errno));
    return core::Unit();
}

core::Expected<core::Optional<uint64_t>, std::string> LocalFile::SeekData(uint64_t offset) {
    const off_t r = ::lseek(fd_, static_cast<off_t>(offset), SEEK_DATA);
    if (r < 0) {
        // ENXIO means there is no data at or after this offset -- the answer
        // to the question, not a failure to answer it. Any other errno is a
        // real failure and surfaces.
        if (errno == ENXIO) return core::Optional<uint64_t>();
        return core::make_unexpected(Errno("lseek SEEK_DATA", errno));
    }
    return core::Optional<uint64_t>(static_cast<uint64_t>(r));
}

core::Expected<core::Optional<uint64_t>, std::string> LocalFile::SeekHole(uint64_t offset) {
    const off_t r = ::lseek(fd_, static_cast<off_t>(offset), SEEK_HOLE);
    if (r < 0) {
        if (errno == ENXIO) return core::Optional<uint64_t>();
        return core::make_unexpected(Errno("lseek SEEK_HOLE", errno));
    }
    return core::Optional<uint64_t>(static_cast<uint64_t>(r));
}

core::Expected<core::Unit, std::string> LocalFile::Discard(uint64_t offset, uint64_t len) {
    if (len == 0) return core::Unit();
    const core::Expected<core::Unit, sys::SysError> r =
        sys::ReleaseSpaceHint(fd_, offset, len);
    if (r.ok()) return core::Unit();
    // Surfaced, unlike the eviction hints below. This is real filesystem work
    // -- freeing extents and a journal commit -- and a caller told it
    // succeeded would believe space was reclaimed when none was.
    std::ostringstream o;
    o << "discard " << len << " bytes at offset " << offset << ": " << r.error().Message();
    return core::make_unexpected(o.str());
}

core::Expected<core::Unit, std::string> LocalFile::EvictRange(uint64_t offset,
                                                                uint64_t len) {
    const core::Expected<core::Unit, sys::SysError> r = sys::EvictPageCache(fd_, offset, len);
    if (r.ok()) return core::Unit();
    // Swallowed when the platform has no equivalent, and only then. Dropping
    // cache residency never changes what a read returns, so an absent
    // `posix_fadvise` is not a failure of this call -- and ZFile invokes this
    // with `?` while writing, so reporting it would break those writes
    // outright. A real errno still propagates.
    //
    // Unreachable as ported: `sys::EvictPageCache` only ever reports an
    // errno here, because Linux always has `posix_fadvise`. The branch is
    // what Rust's macOS arm needs, and it is kept because the alternative is
    // for whoever adds that arm to discover the requirement from a broken
    // ZFile write.
    if (r.error().IsUnsupported()) return core::Unit();
    return core::make_unexpected(std::string("evict_range: ") + r.error().Message());
}

core::Expected<core::Unit, std::string> LocalFile::EvictAll() {
    // Zero length means "through end of file", which for a layer file walks
    // its entire page cache.
    const core::Expected<core::Unit, sys::SysError> r = sys::EvictPageCache(fd_, 0, 0);
    if (r.ok()) return core::Unit();
    if (r.error().IsUnsupported()) return core::Unit();
    return core::make_unexpected(std::string("evict_all: ") + r.error().Message());
}

// ---------------------------------------------------------------------------
// xattr
// ---------------------------------------------------------------------------
//
// Forwarded to `sys`, which owns the two-phase size query and the name
// validation. The only work here is restating the failure in this layer's
// error type; the base class refuses all four, so reaching these at all is
// what marks the file as supporting them.

core::Expected<ByteBuffer, std::string> LocalFile::FGetXattr(const std::string& name) {
    const core::Expected<std::vector<uint8_t>, sys::SysError> r = sys::FGetXattr(fd_, name);
    if (r.ok()) return r.value();
    return core::make_unexpected(std::string("fgetxattr ") + name + ": " +
                                 r.error().Message());
}

core::Expected<std::vector<std::string>, std::string> LocalFile::FListXattr() {
    const core::Expected<std::vector<std::string>, sys::SysError> r = sys::FListXattr(fd_);
    if (r.ok()) return r.value();
    return core::make_unexpected(std::string("flistxattr: ") + r.error().Message());
}

core::Expected<core::Unit, std::string> LocalFile::FSetXattr(const std::string& name,
                                                               const uint8_t* value,
                                                               size_t len, int flags) {
    const core::Expected<core::Unit, sys::SysError> r =
        sys::FSetXattr(fd_, name, value, len, flags);
    if (r.ok()) return core::Unit();
    return core::make_unexpected(std::string("fsetxattr ") + name + ": " +
                                 r.error().Message());
}

core::Expected<core::Unit, std::string> LocalFile::FRemoveXattr(const std::string& name) {
    const core::Expected<core::Unit, sys::SysError> r = sys::FRemoveXattr(fd_, name);
    if (r.ok()) return core::Unit();
    return core::make_unexpected(std::string("fremovexattr ") + name + ": " +
                                 r.error().Message());
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
