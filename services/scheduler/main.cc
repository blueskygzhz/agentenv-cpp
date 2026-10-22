// SPDX-License-Identifier: MIT
// Go upstream: services/scheduler/cmd/main.go — the placement server.
//
// What this serves. Upstream exposes placement and P2P discovery over gRPC;
// `AGENTENV_WITH_GRPC` needs a code generator this build does not carry, so
// the P2P half is served over HTTP instead — the same four calls, the same
// field names, matching `agentenv/p2p/scheduler_http.h` on the node side.
// With this running, a cluster gets P2P peer discovery without any external
// component; before it, the node-side wire had nothing to dial.
//
// What this is not. Placement itself is still only a library here: the
// strategy and filter code is ported and unit-tested, but no RPC reaches it,
// because nothing in this tree calls a scheduler for placement yet. Serving a
// placement endpoint no client uses would be inventing a contract.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "agentenv/api/server.h"
#include "agentenv/core/logging.h"
#include "services/scheduler/artifact_index.h"
#include "services/scheduler/internal.h"
#include "services/scheduler/p2p_routes.h"

namespace ae = agentenv;
using namespace agentenv::services::scheduler;  // NOLINT

namespace {

volatile sig_atomic_t g_shutdown_signal = 0;
ae::api::Server* g_server_for_signal = NULL;

void HandleShutdownSignal(int signum) {
    g_shutdown_signal = signum;
    // The only call safe on this path: it flips a flag and shuts the listening
    // fd down, which is what unblocks `ListenAndServe`.
    if (g_server_for_signal != NULL) g_server_for_signal->Stop();
}

void InstallShutdownHandlers(ae::api::Server* server) {
    g_server_for_signal = server;
    struct sigaction action;
    ::sigemptyset(&action.sa_mask);
    action.sa_handler = HandleShutdownSignal;
    action.sa_flags = 0;
    ::sigaction(SIGINT, &action, NULL);
    ::sigaction(SIGTERM, &action, NULL);

    // A peer that disconnects mid-response must not take the process with it.
    struct sigaction ignore;
    ::sigemptyset(&ignore.sa_mask);
    ignore.sa_handler = SIG_IGN;
    ignore.sa_flags = 0;
    ::sigaction(SIGPIPE, &ignore, NULL);
}

std::string EnvOr(const char* name, const std::string& fallback) {
    const char* raw = ::getenv(name);
    if (raw == NULL || *raw == '\0') return fallback;
    return std::string(raw);
}

std::size_t EnvSize(const char* name, std::size_t fallback) {
    const char* raw = ::getenv(name);
    if (raw == NULL || *raw == '\0') return fallback;
    const unsigned long parsed = std::strtoul(raw, NULL, 10);
    return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
}

}  // namespace

int main() {
    const std::string bind = EnvOr("AENV_SCHEDULER_BIND", "0.0.0.0:9090");
    // The same cluster secret the blob surface uses: one thing to rotate.
    const std::string token = EnvOr("AENV_P2P_SHARED_TOKEN", std::string());
    const std::size_t capacity =
        EnvSize("AENV_SCHEDULER_ARTIFACT_CAPACITY", kDefaultArtifactStoreCapacity);
    // Go's `ArtifactLookupNodeLimit`. Zero means no cap; a small value bounds
    // how many providers one lookup hands back, which is enough because the
    // node tries them in order and stops at the first that answers.
    const std::size_t lookup_limit = EnvSize("AENV_SCHEDULER_LOOKUP_NODE_LIMIT", 0);

    if (token.empty()) {
        // Warned, not refused: a scheduler reachable only from inside the
        // cluster may legitimately run open. Silence is the problem, since
        // `core::http` has no TLS and an operator who did not mean this has
        // no other hint.
        AGENTENV_WARN("scheduler P2P surface is unauthenticated;"
                      << " set AENV_P2P_SHARED_TOKEN unless the port is"
                      << " unreachable from outside the cluster");
    }

    NodeRegistry nodes;
    InMemoryArtifactStore artifacts(capacity, lookup_limit);

    std::unique_ptr<ae::api::Server> server = ae::api::MakeServer();
    if (!server) {
        AGENTENV_ERROR("agentenv-scheduler: no HTTP server backend is compiled in");
        return 1;
    }

    RegisterSchedulerP2pRoutes(server.get(), &nodes, &artifacts, token);
    // Registered because nothing else populates the registry in this tree:
    // upstream gets nodes from Kubernetes discovery or a heartbeat RPC,
    // neither of which is ported, and without them every P2P query would
    // answer with an empty list.
    RegisterSchedulerNodeRoutes(server.get(), &nodes, &artifacts, token);

    InstallShutdownHandlers(server.get());

    AGENTENV_INFO("agentenv-scheduler starting"
                  << " bind=" << bind << " artifact_capacity=" << capacity
                  << " lookup_node_limit=" << lookup_limit);

    // No "listening" line here: `ListenAndServe` logs once the socket is
    // actually bound, with the port it really got.
    const ae::core::Expected<ae::core::Unit, ae::core::AnyError> served =
        server->ListenAndServe(bind);

    if (g_shutdown_signal != 0) {
        AGENTENV_INFO("agentenv-scheduler received signal "
                      << static_cast<int>(g_shutdown_signal) << ", shutting down");
    }
    if (!served.ok()) {
        AGENTENV_ERROR("agentenv-scheduler failed: " << served.error().chain());
        return 1;
    }
    return 0;
}
