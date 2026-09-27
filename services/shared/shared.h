// SPDX-License-Identifier: MIT
// Rust upstream: services/shared/*.go — shared types across scheduler/gateway/api.
#ifndef AGENTENV_SERVICES_SHARED_H_
#define AGENTENV_SERVICES_SHARED_H_

#include <string>

namespace agentenv {
namespace services {
namespace shared {

/// Node registration payload sent from agentenv nodes to the scheduler.
struct NodeInfo {
    std::string node_id;
    std::string cluster_id;
    std::string host_addr;
    uint32_t    cpu_capacity = 0;
    uint64_t    mem_capacity_bytes = 0;
};

}}}
#endif
