// SPDX-License-Identifier: MIT
// Go: services/gateway/internal/{schedule_hint,node_list}.go
#include "services/gateway/internal.h"

#include <algorithm>

namespace agentenv {
namespace services {
namespace gateway {

core::Expected<ScheduleHintResult, std::string>
BuildScheduleHint(const std::string& method, const std::string& path,
                  const std::string& body) {
    ScheduleHintResult res;
    if (method != "POST") return res;  // has_hint=false

    // Trim a trailing '/'.
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();

    if (p == "/sandboxes-cold") {
        res.has_hint = true;
        res.hint.kind = "new_cold_sandbox";
    } else if (p == "/sandboxes") {
        res.has_hint = true;
        res.hint.kind = "new_sandbox";
    } else {
        return res;
    }
    // Best-effort extract "image" / "template" field from a tiny JSON body.
    if (body.size() <= kMaxHintBodyBytes) {
        size_t k = body.find("\"image\"");
        if (k == std::string::npos) k = body.find("\"template_id\"");
        if (k != std::string::npos) {
            size_t colon = body.find(':', k);
            size_t q1 = colon == std::string::npos ? std::string::npos
                                : body.find('"', colon + 1);
            size_t q2 = q1 == std::string::npos ? std::string::npos
                                                : body.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                res.hint.image_ref = body.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }
    return res;
}

void NodeList::Replace(const std::vector<UpstreamNode>& nodes) {
    nodes_ = nodes;
}
std::vector<UpstreamNode> NodeList::Snapshot() const {
    return nodes_;
}
bool NodeList::Empty() const {
    return nodes_.empty();
}

}  // namespace gateway
}  // namespace services
}  // namespace agentenv
