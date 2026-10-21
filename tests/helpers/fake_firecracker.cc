// SPDX-License-Identifier: MIT
// Stand-in VMM for the boot tests.
//
// Invoked exactly as the real binary is:
//   fake_firecracker --api-sock <path> --mmds-size-limit <n>
//
// It binds and listens on the API socket path, then stays alive so the parent
// observes a running child rather than an early exit. The API itself is served
// by an injected Connector, so no KVM and no real VMM are involved -- this
// exists only to make the process-lifecycle half of the boot sequence
// reachable on an ordinary host.
//
// Why a real `bind` + `listen` and not just creating the file: `WaitForReady`
// probes by connecting, exactly as Firecracker's own clients must, so a plain
// file is indistinguishable from a leftover socket and is correctly treated as
// not-ready. A fake that only touched the path would hang every boot test
// until the timeout.
//
// `listen` without ever calling `accept` is enough. A connect completes as
// soon as the kernel can queue it against the backlog, and the probe closes
// immediately without sending anything.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const char* sock = NULL;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--api-sock") == 0) {
            sock = argv[i + 1];
            break;
        }
    }

    int fd = -1;
    if (sock != NULL) {
        struct sockaddr_un addr;
        if (std::strlen(sock) >= sizeof(addr.sun_path)) {
            std::fprintf(stderr, "fake_firecracker: api socket path too long: %s\n", sock);
            return 1;
        }
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            std::fprintf(stderr, "fake_firecracker: socket() failed\n");
            return 1;
        }
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);

        // No unlink here on purpose: the parent removes a leftover socket
        // before spawning, so a bind collision means two VMMs were told to use
        // one path. Failing loudly is better than silently stealing it.
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::fprintf(stderr, "fake_firecracker: bind(%s) failed\n", sock);
            ::close(fd);
            return 1;
        }
        if (::listen(fd, 16) != 0) {
            std::fprintf(stderr, "fake_firecracker: listen(%s) failed\n", sock);
            ::close(fd);
            return 1;
        }
    }

    // Long enough to outlive any test, short enough not to linger if a test
    // forgets to stop it.
    ::sleep(30);
    if (fd >= 0) ::close(fd);
    return 0;
}
