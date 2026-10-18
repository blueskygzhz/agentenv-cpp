// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/local.rs — LocalFileBuilder / LocalFile.
//
// The first concrete `VirtualFile`: a plain file on local storage, buffered or
// `O_DIRECT`. Everything above it in the stack (the LSMT layers, the image
// file, the startup pack) ultimately reads through one of these.
#ifndef AGENTENV_STORAGE_OVERLAYBD_LOCAL_FILE_H_
#define AGENTENV_STORAGE_OVERLAYBD_LOCAL_FILE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

class LocalFile;

/// Rust `DIRECT_IO_ALIGNMENT`.
///
/// The alignment `O_DIRECT` imposes on the offset, the length and the buffer
/// address. 512 rather than the page size because that is what the block
/// layer requires; a larger value would reject I/O the kernel accepts.
const size_t kDirectIoAlignment = 512;

/// Rust `LocalFileBuilder`.
///
/// Defaults match Rust's: read, write and create enabled, mode 0644, no
/// truncate, no direct I/O.
class LocalFileBuilder {
 public:
    LocalFileBuilder();

    LocalFileBuilder& Read(bool v);
    LocalFileBuilder& Write(bool v);
    LocalFileBuilder& Create(bool v);
    LocalFileBuilder& Truncate(bool v);
    LocalFileBuilder& Mode(uint32_t v);
    LocalFileBuilder& DirectIo(bool v);

    /// Rust `open`.
    ///
    /// When direct I/O was requested, a failure to enable it is propagated
    /// rather than swallowed: handing back a cached file would make
    /// `DirectIo(true)` mean two different things depending on the platform,
    /// and the alignment rules enforced on every write would then be
    /// guarding nothing.
    core::Expected<std::shared_ptr<LocalFile>, std::string> Open(
        const std::string& path) const;

 private:
    bool read_;
    bool write_;
    bool create_;
    bool truncate_;
    uint32_t mode_;
    bool direct_io_;
};

/// Rust `LocalFile`.
///
/// Rust splits every operation into a synchronous body and an async wrapper
/// that either runs it inline or hands it to `spawn_blocking`. That split does
/// not survive into a synchronous port, and collapsing it loses no behaviour
/// -- the bodies are identical, and the offload decision was about *which
/// thread* runs the syscall, never about what it does.
///
/// Where that decision has to reappear is the caller. Rust offloads
/// `truncate`, `sync`, `discard` and the eviction hints because each has
/// unbounded latency, and `discard` in particular sits on a guest-triggerable
/// path: a guest `fstrim` reaches it in a loop. Called inline on a ublk queue
/// thread, any of them stalls every other in-flight request on that queue.
/// Nothing here can prevent that; the queue worker has to.
class LocalFile : public VirtualFile {
 public:
    ~LocalFile();

    /// Rust `builder`.
    static LocalFileBuilder Builder();
    /// Rust `new` — create and truncate.
    static core::Expected<std::shared_ptr<LocalFile>, std::string> New(
        const std::string& path);
    /// Rust `open_ro` — read-only, no create.
    static core::Expected<std::shared_ptr<LocalFile>, std::string> OpenRo(
        const std::string& path);
    /// Rust `open_rw`.
    static core::Expected<std::shared_ptr<LocalFile>, std::string> OpenRw(
        const std::string& path, bool create);

    const std::string& Path() const { return path_; }
    int Fd() const { return fd_; }
    bool IsDirectIo() const { return direct_io_; }

    /// Rust `write_calls` — a probe counting positional-write submissions.
    uint64_t WriteCalls() const { return write_calls_; }

    /// Rust `write_at_sync_pwrite` — write the whole buffer with one
    /// positional write, with none of the direct-I/O alignment checks.
    ///
    /// A separate entry point rather than a flag, because the caller is
    /// asserting something this class cannot check: that the file is buffered
    /// local storage, so the syscall returns promptly. Reaching it with an
    /// `O_DIRECT` file hands unaligned I/O straight to the kernel.
    core::Expected<size_t, std::string> WriteAtSyncPwrite(uint64_t offset,
                                                           const uint8_t* buf, size_t len);

    // -- VirtualFile -------------------------------------------------------

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                    size_t len) override;
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                 size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;
    core::Expected<core::Unit, std::string> Truncate(uint64_t size) override;
    core::Expected<core::Unit, std::string> Sync() override;
    core::Expected<core::Optional<uint64_t>, std::string> SeekData(uint64_t offset) override;
    core::Expected<core::Optional<uint64_t>, std::string> SeekHole(uint64_t offset) override;
    core::Expected<core::Unit, std::string> Discard(uint64_t offset, uint64_t len) override;
    core::Expected<core::Unit, std::string> EvictRange(uint64_t offset,
                                                        uint64_t len) override;
    core::Expected<core::Unit, std::string> EvictAll() override;
    core::Expected<ByteBuffer, std::string> FGetXattr(const std::string& name) override;
    core::Expected<std::vector<std::string>, std::string> FListXattr() override;
    core::Expected<core::Unit, std::string> FSetXattr(const std::string& name,
                                                       const uint8_t* value, size_t len,
                                                       int flags) override;
    core::Expected<core::Unit, std::string> FRemoveXattr(const std::string& name) override;

    // The `*WithCtx` overrides are deliberately absent, so the base defaults
    // apply and every read falls back to a synchronous `pread`.
    //
    // Rust submits these on the caller's ring, which is the whole reason the
    // context is threaded down at all. Doing the same here needs a completion
    // demultiplexer that does not exist yet: `util::Ring::Reap` drains *every*
    // visible completion, so a read that reaped its own would also swallow
    // the ublk queue worker's, and those in-flight commands would never be
    // answered -- a wedged device rather than a slow one. The fallback is
    // correct; on a queue thread it is also a blocking syscall.

 private:
    LocalFile();
    LocalFile(const LocalFile&);
    LocalFile& operator=(const LocalFile&);
    friend class LocalFileBuilder;

    /// Rust `read_into_at` — fill `buf` as far as the file allows.
    core::Expected<size_t, std::string> ReadIntoAt(uint64_t offset, uint8_t* buf,
                                                    size_t len);
    /// Rust `read_buffered_at` / `read_direct_at`.
    core::Expected<ByteBuffer, std::string> ReadBufferedAt(uint64_t offset, size_t len);
    core::Expected<ByteBuffer, std::string> ReadDirectAt(uint64_t offset, size_t len);
    /// Rust `write_all_at` — loop until the whole buffer is written.
    core::Expected<core::Unit, std::string> WriteAllAt(uint64_t offset, const uint8_t* buf,
                                                        size_t len);

    std::string path_;
    int fd_;
    bool direct_io_;
    uint64_t write_calls_;
};

/// Rust `align_down` / `align_up`. Exposed for testing.
///
/// `AlignUp` saturates rather than wrapping: a value within `alignment` of the
/// maximum would otherwise round to zero and address the start of the file.
uint64_t AlignDown(uint64_t value, uint64_t alignment);
uint64_t AlignUp(uint64_t value, uint64_t alignment);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_LOCAL_FILE_H_
