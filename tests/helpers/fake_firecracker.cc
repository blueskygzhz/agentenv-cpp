// SPDX-License-Identifier: MIT
// Stand-in VMM for the boot tests.
//
// Invoked exactly as the real binary is:
//   fake_firecracker --api-sock <path> --mmds-size-limit <n>
//
// It creates the API socket path so `WaitForReady` succeeds, then stays alive
// so the parent observes a running child rather than an early exit. The API
// itself is served by an injected Connector, so no KVM and no real VMM are
// involved -- this exists only to make the process-lifecycle half of the boot
// sequence reachable on an ordinary host.
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
    if (sock != NULL) {
        // A plain file is enough: WaitForReady polls for the path's existence,
        // and the injected Connector never dials it.
        std::FILE* f = std::fopen(sock, "w");
        if (f != NULL) std::fclose(f);
    }
    // Long enough to outlive any test, short enough not to linger if a test
    // forgets to stop it.
    ::sleep(30);
    return 0;
}
