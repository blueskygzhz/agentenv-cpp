// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/scm.rs
#include "agentenv/storage/uffd-core/scm.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sstream>
#include <vector>

namespace agentenv {
namespace storage {
namespace uffd {

core::Expected<RecvWithFd, std::string> RecvWithFdFrom(int sock_fd, void* buf, std::size_t len) {
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = len;

    // Room for exactly two descriptors. Sized for one more than is legal on
    // purpose: with space for only one, a sender passing two would have the
    // second silently dropped by the kernel (MSG_CTRUNC) and leaked in the
    // sender's own table, instead of being detected and refused here.
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int) * 2)];
    } control;
    ::memset(&control, 0, sizeof(control));

    struct msghdr msg;
    ::memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);

    ssize_t n;
    for (;;) {
        n = ::recvmsg(sock_fd, &msg, 0);
        if (n >= 0) break;
        // Rust retries on EWOULDBLOCK after awaiting readability; the loop
        // here is the same decision made against a blocking descriptor, where
        // EINTR is the form a spurious wakeup takes.
        if (errno == EINTR) continue;
        std::ostringstream o;
        o << "recvmsg: " << ::strerror(errno) << " (errno " << errno << ")";
        return core::make_unexpected(o.str());
    }

    std::vector<int> fds;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != 0; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const std::size_t payload = c->cmsg_len - CMSG_LEN(0);
        const std::size_t count = payload / sizeof(int);
        for (std::size_t i = 0; i < count; ++i) {
            int fd = -1;
            ::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            fds.push_back(fd);
        }
    }

    if (fds.size() >= 2) {
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if (fds[i] >= 0) ::close(fds[i]);
        }
        std::ostringstream o;
        o << "recv_with_fd got too many fds: " << fds.size();
        return core::make_unexpected(o.str());
    }

    RecvWithFd out;
    out.bytes = static_cast<std::size_t>(n);
    out.fd = fds.empty() ? -1 : fds[0];
    return out;
}

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
