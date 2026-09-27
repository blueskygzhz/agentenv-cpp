// SPDX-License-Identifier: MIT
// Rust upstream: services/api/*.go — public API HTTP server (thin wrapper on ApiImpl).
#include <cstdio>
int main() {
    std::fprintf(stderr,
        "agentenv-api-server: skeleton. "
        "Enable AGENTENV_WITH_HTTPLIB to build the HTTP surface.\n");
    return 0;
}
