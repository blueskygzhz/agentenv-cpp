// SPDX-License-Identifier: MIT
// Rust: src/api/proxy.rs — HTTP reverse proxy from gateway to sandbox.
#ifndef AGENTENV_API_PROXY_H_
#define AGENTENV_API_PROXY_H_

#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/orchestrator/proxy.h"

namespace agentenv {
namespace api {

/// Rust fn: given a sandbox id + inbound HTTP request, resolve a `ProxyTarget`
/// via `orchestrator::ProxyRouteTable` and forward the request to it.
///
/// Skeleton: real implementation would depend on an HTTP client library.
struct ProxyRequest {
    std::string sandbox_id;
    std::string method;
    std::string path;
    std::string body;
};

struct ProxyResponse {
    int         status = 200;
    std::string body;
};

class Proxy {
 public:
    virtual ~Proxy() {}
    virtual core::Expected<ProxyResponse, std::string>
        Forward(const ProxyRequest& req) = 0;
};

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_PROXY_H_
