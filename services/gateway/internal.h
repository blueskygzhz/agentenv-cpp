// SPDX-License-Identifier: MIT
// Rust upstream (Go): services/gateway/internal/{host_route,schedule_hint,
// node_list,cluster_list,metrics,server}.go — HTTP reverse proxy front door.
#ifndef AGENTENV_SERVICES_GATEWAY_INTERNAL_H_
#define AGENTENV_SERVICES_GATEWAY_INTERNAL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace services {
namespace gateway {

// ---- Go: internal/host_route.go ----
static const size_t kMaxDnsLabelLength = 63;
static const size_t kMaxDnsNameLength  = 253;

/// Go: struct hostRoute — a parsed sandbox data-plane route "PORT-SANDBOXID.domain".
struct HostRoute {
    std::string sandbox_id;
    int         target_port = 0;
};

/// Go: parseHostRoute(rawHost, domains).
/// Returns:
///   - ok(true) + routewhen the host matches a "<port>-<sandbox>.<domain>" form
///   - ok(false)         when the host is not a data-plane host (pass through)
///   - errorwhen it matches the shape but is malformed
struct HostRouteResult {
    bool matched = false;
    HostRoute route;
};
core::Expected<HostRouteResult, std::string>
    ParseHostRoute(const std::string& raw_host, const std::vector<std::string>& domains);

/// Go: normalizeProxyDomains — lowercases, dedups, sorts by descending length.
std::vector<std::string> NormalizeProxyDomains(const std::vector<std::string>& domains);

/// Go: normalizeRequestHost — trims, lowercases, strips :port.
std::string NormalizeRequestHost(const std::string& raw_host);

// ---- Go: internal/schedule_hint.go ----
static const size_t kMaxHintBodyBytes = 64 * 1024;

/// Go: ScheduleRequestHint produced from an inbound request.
struct ScheduleRequestHint {
    std::string kind;// "new_sandbox" | "new_cold_sandbox" | ""
    std::string image_ref;   // parsed from body when available
};

/// Go: buildScheduleHint(method, path, body).
struct ScheduleHintResult {
    bool has_hint = false;
    ScheduleRequestHint hint;
};
core::Expected<ScheduleHintResult, std::string>
    BuildScheduleHint(const std::string& method,
                      const std::string& path,
                      const std::string& body);

// ---- Go: internal/node_list.go / cluster_list.go ----
/// Go: one upstream node the gateway can proxy to.
struct UpstreamNode {
    std::string node_id;
    std::string base_url;   // "http://10.0.0.1:6767"
};

/// Go: NodeList — the live set of upstream nodes for one cluster.
class NodeList {
 public:
    void Replace(const std::vector<UpstreamNode>& nodes);
    std::vector<UpstreamNode> Snapshot() const;
    bool Empty() const;
 private:
    mutable std::vector<UpstreamNode> nodes_;
};

}  // namespace gateway
}  // namespace services
}  // namespace agentenv
#endif  // AGENTENV_SERVICES_GATEWAY_INTERNAL_H_
