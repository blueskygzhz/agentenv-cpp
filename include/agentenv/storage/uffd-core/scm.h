// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/scm.rs — `UnixRecvFdExt::recv_with_fd`.
#ifndef AGENTENV_STORAGE_UFFD_CORE_SCM_H_
#define AGENTENV_STORAGE_UFFD_CORE_SCM_H_

#include <cstddef>
#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace uffd {

/// What a single `recvmsg` produced.
struct RecvWithFd {
    /// Bytes written into the caller's buffer.
    std::size_t bytes;
    /// The descriptor passed via SCM_RIGHTS, or -1 if the message carried
    /// none. Owned by the caller once returned.
    int fd;

    RecvWithFd() : bytes(0), fd(-1) {}
};

/// Rust `recv_with_fd` — receive a message plus at most one descriptor.
///
/// Only the receive half is ported: the handler never sends a descriptor, it
/// only accepts the uffd that Firecracker hands over.
///
/// More than one descriptor is an error, and every one of them is closed
/// before returning. Rust does the same, and the reason is that the error path
/// is the only place they can be reached: nothing else holds them, so an early
/// return without the closes leaks a descriptor per malformed handshake.
core::Expected<RecvWithFd, std::string> RecvWithFdFrom(int sock_fd, void* buf, std::size_t len);

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UFFD_CORE_SCM_H_
