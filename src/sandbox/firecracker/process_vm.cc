// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/process_vm_reader.rs
#include "agentenv/sandbox/firecracker/process_vm.h"

#include <sys/uio.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

std::string Hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

}  // namespace

core::Expected<core::Unit, std::string> ProcessVmReader::ReadExactRemote(uint64_t remote_addr,
                                                                        void* dst,
                                                                        std::size_t len) const {
    if (len == 0) return core::Unit();
    if (dst == NULL) {
        return core::make_unexpected(std::string("process_vm_readv destination is null"));
    }

    unsigned char* cursor = static_cast<unsigned char*>(dst);
    std::size_t remaining = len;
    uint64_t    address   = remote_addr;

    while (remaining > 0) {
        // A 64-bit address must fit the platform's pointer width before it can
        // be handed to the kernel.
        if (address > static_cast<uint64_t>(SIZE_MAX)) {
            std::ostringstream message;
            message << "remote address " << Hex(address) << " for pid " << pid_
                    << " does not fit size_t";
            return core::make_unexpected(message.str());
        }

        struct iovec local;
        local.iov_base = cursor;
        local.iov_len  = remaining;

        struct iovec remote;
        remote.iov_base = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
        remote.iov_len  = remaining;

        const ssize_t read =
            ::process_vm_readv(static_cast<pid_t>(pid_), &local, 1, &remote, 1, 0);

        if (read < 0) {
            // A signal is not a failure; retrying is what a blocking read
            // would have done.
            if (errno == EINTR) continue;
            std::ostringstream message;
            message << "process_vm_readv pid " << pid_ << " remote address " << Hex(address)
                    << ": " << std::strerror(errno);
            return core::make_unexpected(message.str());
        }
        // Not progress. Treating it as success would leave the rest of the
        // buffer zeroed — indistinguishable from genuinely zeroed guest memory
        // — and looping would never terminate.
        if (read == 0) {
            std::ostringstream message;
            message << "process_vm_readv for pid " << pid_ << " returned 0 bytes at "
                    << Hex(address);
            return core::make_unexpected(message.str());
        }
        if (static_cast<std::size_t>(read) > remaining) {
            // The kernel cannot legitimately overrun the iovec; if it claims
            // to, the result is not trustworthy.
            std::ostringstream message;
            message << "process_vm_readv for pid " << pid_ << " returned " << read
                    << " bytes for " << remaining << " byte buffer at " << Hex(address);
            return core::make_unexpected(message.str());
        }

        const uint64_t advanced = static_cast<uint64_t>(read);
        if (advanced > UINT64_MAX - address) {
            std::ostringstream message;
            message << "process_vm_readv remote address overflow for pid " << pid_;
            return core::make_unexpected(message.str());
        }

        // A short read is normal: keep going until the destination is full.
        address += advanced;
        cursor += read;
        remaining -= static_cast<std::size_t>(read);
    }

    return core::Unit();
}

core::Expected<std::string, std::string> ProcessVmReader::ReadAt(uint64_t remote_addr,
                                                                 std::size_t len) const {
    std::string data;
    data.resize(len);
    if (len == 0) return data;

    const core::Expected<core::Unit, std::string> read =
        ReadExactRemote(remote_addr, &data[0], len);
    if (!read.ok()) return core::make_unexpected(read.error());
    return data;
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
