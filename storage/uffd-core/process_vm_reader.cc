// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/process_vm_reader.rs
#include "agentenv/storage/uffd-core/process_vm_reader.h"

#include <errno.h>
#include <string.h>
#include <sys/uio.h>

#include <sstream>

namespace agentenv {
namespace storage {
namespace uffd {

core::Expected<std::size_t, std::string> ProcessVmReader::ReadScatter(
    std::vector<ScatterRegion>* regions) {
    if (regions == 0 || regions->empty()) return static_cast<std::size_t>(0);

    std::vector<struct iovec> local;
    std::vector<struct iovec> remote;
    local.reserve(regions->size());
    remote.reserve(regions->size());
    for (std::size_t i = 0; i < regions->size(); ++i) {
        const ScatterRegion& r = (*regions)[i];
        if (r.buf == 0 && r.len != 0) {
            return core::make_unexpected(std::string("process_vm_readv scatter: null buffer"));
        }
        struct iovec l;
        l.iov_base = r.buf;
        l.iov_len = r.len;
        local.push_back(l);

        struct iovec m;
        m.iov_base = reinterpret_cast<void*>(static_cast<uintptr_t>(r.va));
        m.iov_len = r.len;
        remote.push_back(m);
    }

    const ssize_t n = ::process_vm_readv(static_cast<pid_t>(pid_), &local[0], local.size(),
                                         &remote[0], remote.size(), 0);
    if (n < 0) {
        std::ostringstream o;
        o << "process_vm_readv scatter: " << ::strerror(errno) << " (errno " << errno << ")";
        return core::make_unexpected(o.str());
    }
    return static_cast<std::size_t>(n);
}

core::Expected<overlaybd::ByteBuffer, std::string> ProcessVmReader::ReadAt(uint64_t /*offset*/,
                                                                          size_t /*len*/) {
    return core::make_unexpected(std::string("ProcessVmReader does not support read_at()"));
}

core::Expected<size_t, std::string> ProcessVmReader::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                                 size_t len) {
    if (len == 0) return static_cast<size_t>(0);
    if (dst == 0) {
        return core::make_unexpected(std::string("ProcessVmReader read_at_into: null dst"));
    }

    // Rust issues one `read_scatter` with a single region and returns its
    // count. The loop here is the short-read handling the file header calls
    // out: the caller of a `VirtualFile` is allowed to treat a short read as
    // end-of-file, and for this reader there is no end of file -- a short read
    // means the syscall stopped early and the tail of `dst` is still zeros.
    size_t done = 0;
    while (done < len) {
        std::vector<ScatterRegion> one;
        one.push_back(ScatterRegion(offset + done, dst + done, len - done));
        const core::Expected<std::size_t, std::string> n = ReadScatter(&one);
        if (!n.ok()) return core::make_unexpected(n.error());
        if (n.value() == 0) {
            // Not progress. Returning here instead of looping is what keeps
            // this from spinning forever on an unreadable address.
            std::ostringstream o;
            o << "process_vm_readv read 0 of " << (len - done) << " remaining byte(s) at 0x"
              << std::hex << (offset + done);
            return core::make_unexpected(o.str());
        }
        done += n.value();
    }
    return done;
}

core::Expected<size_t, std::string> ProcessVmReader::WriteAt(uint64_t /*offset*/,
                                                              const uint8_t* /*data*/,
                                                              size_t /*len*/) {
    return core::make_unexpected(std::string("ProcessVmReader does not support write_at()"));
}

core::Expected<uint64_t, std::string> ProcessVmReader::Size() {
    return core::make_unexpected(std::string("ProcessVmReader does not support size()"));
}

bool ProcessVmReader::ReadAt(uint8_t /*tag*/, uint64_t src_byte_off, uint8_t* dst,
                             uint64_t len) {
    const core::Expected<size_t, std::string> r =
        ReadAtInto(src_byte_off, dst, static_cast<size_t>(len));
    return r.ok() && r.value() == static_cast<size_t>(len);
}

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
