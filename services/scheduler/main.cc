// SPDX-License-Identifier: MIT
// Go upstream: services/scheduler/cmd/main.go — multi-node placement server.
//
// This skeleton wires theported placement library (strategy/filter/registry)
// into a runnable entrypoint. The real gRPC transport is enabled with
// AGENTENV_WITH_GRPC.
#include <cstdio>

#include "services/scheduler/internal.h"

using namespace agentenv::services::scheduler;

int main() {
    NodeRegistry registry;
    registry.Upsert(Node{"node-a", "10.0.0.1:7000"});
    registry.Upsert(Node{"node-b", "10.0.0.2:7000"});

    std::unique_ptr<Strategy> strategy = NewStrategy("round_robin");
    std::vector<RichNode> nodes = registry.RichNodes();

    auto pick = strategy->Select(nodes, nullptr);
    if (pick.ok()) {
        std::fprintf(stderr, "agentenv-scheduler: strategy=%s picked node=%s\n",
                     strategy->Name().c_str(), pick.value().node.id.c_str());
    } else {
        std::fprintf(stderr, "agentenv-scheduler: %s\n", pick.error().c_str());
    }
    std::fprintf(stderr, "agentenv-scheduler: skeleton (enable AGENTENV_WITH_GRPC for the RPC server).\n");
    return 0;
}
