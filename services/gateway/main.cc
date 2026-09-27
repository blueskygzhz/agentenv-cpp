// SPDX-License-Identifier: MIT
// Go upstream: services/gateway/cmd/main.go — HTTP reverse proxy front door.
#include <cstdio>

#include "services/gateway/internal.h"

using namespace agentenv::services::gateway;

int main() {
    std::vector<std::string> domains = NormalizeProxyDomains({"e2b.example", "sbx.example"});
    auto r = ParseHostRoute("8080-mysandbox.sbx.example", domains);
    if (r.ok() && r.value().matched) {
        std::fprintf(stderr,
                     "agentenv-gateway: route sandbox=%s port=%d\n",
                     r.value().route.sandbox_id.c_str(),
                     r.value().route.target_port);
    }
    std::fprintf(stderr, "agentenv-gateway: skeleton (enable AGENTENV_WITH_HTTPLIB for the proxy).\n");
    return 0;
}
