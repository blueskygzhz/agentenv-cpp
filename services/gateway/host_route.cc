// SPDX-License-Identifier: MIT
// Go: services/gateway/internal/host_route.go
#include "services/gateway/internal.h"

#include <algorithm>
#include <cctype>

namespace agentenv {
namespace services {
namespace gateway {

static bool is_valid_dns_label(const std::string& s) {
    if (s.empty() || s.size() > kMaxDnsLabelLength) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    if (s.front() == '-' || s.back() == '-') return false;
    return true;
}

std::string NormalizeRequestHost(const std::string& raw_host) {
    std::string host = raw_host;
    // trim spaces
    size_t b = host.find_first_not_of(" \t");
    size_t e = host.find_last_not_of(" \t");
    if (b == std::string::npos) return "";
    host = host.substr(b, e - b + 1);
    // strip :port (only when a single trailing colon+digits, keep IPv6-in-brackets simple)
    size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) {
        host = host.substr(0, colon);
    }
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

static std::string trim_lower_dot(const std::string& in) {
    std::string d = in;
    size_t b = d.find_first_not_of(" \t");
    size_t e = d.find_last_not_of(" \t");
    if (b == std::string::npos) return "";
    d = d.substr(b, e - b + 1);
    std::transform(d.begin(), d.end(), d.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!d.empty() && d.back() == '.') d.pop_back();
    return d;
}

std::vector<std::string> NormalizeProxyDomains(const std::vector<std::string>& domains) {
    std::vector<std::string> out;
    for (size_t i = 0; i < domains.size(); ++i) {
        std::string d = trim_lower_dot(domains[i]);
        if (d.empty()) continue;
        if (std::find(out.begin(), out.end(), d) != out.end()) continue;
        out.push_back(d);
    }
    // sort by descending length (longest suffix wins).
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& a, const std::string& b) {
                         return a.size() > b.size();
                     });
    return out;
}

core::Expected<HostRouteResult, std::string>
ParseHostRoute(const std::string& raw_host, const std::vector<std::string>& domains) {
    HostRouteResult res;
    if (domains.empty()) return res;  // matched=false

    std::string host = NormalizeRequestHost(raw_host);
    if (host.empty()) return res;

    for (size_t i = 0; i < domains.size(); ++i) {
        const std::string& domain = domains[i];
        if (host == domain) return res;  // apex domain — not a data-plane host

        std::string suffix = "." + domain;
        if (host.size() <= suffix.size() ||
            host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        std::string label = host.substr(0, host.size() - suffix.size());
        if (label.empty() || label.find('.') != std::string::npos ||
            label.find('-') == std::string::npos) {
            continue;
        }
        if (label.size() > kMaxDnsLabelLength) {
            return core::make_unexpected(
                std::string("invalid sandbox data-plane host: label too long"));
        }
        // Split on first '-' => "<port>-<sandbox>".
        size_t dash = label.find('-');
        std::string port_text = label.substr(0, dash);
        std::string sandbox_id = label.substr(dash + 1);
        if (sandbox_id.empty()) {
            return core::make_unexpected(
                std::string("invalid sandbox data-plane host: sandbox id is empty"));
        }
        if (!is_valid_dns_label(sandbox_id)) {
            return core::make_unexpected(
                std::string("invalid sandbox data-plane host: sandbox id is invalid"));
        }
        // Parse port.
        if (port_text.empty()) {
            return core::make_unexpected(
                std::string("invalid sandbox data-plane host: port is not numeric"));
        }
        int port = 0;
        for (size_t k = 0; k < port_text.size(); ++k) {
            if (port_text[k] < '0' || port_text[k] > '9') {
                return core::make_unexpected(
                    std::string("invalid sandbox data-plane host: port is not numeric"));
            }
            port = port * 10 + (port_text[k] - '0');
        }
        if (port <= 0 || port > 65535) {
            return core::make_unexpected(
                std::string("invalid sandbox data-plane host: port out of range"));
        }
        res.matched = true;
        res.route.sandbox_id = sandbox_id;
        res.route.target_port = port;
        return res;
    }
    return res;  // matched=false
}

}  // namespace gateway
}  // namespace services
}  // namespace agentenv
