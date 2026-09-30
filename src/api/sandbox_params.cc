// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs
#include "agentenv/api/sandbox_params.h"

#include <set>
#include <sstream>

#include "agentenv/cfg.h"

namespace agentenv {
namespace api {

sandbox::network::BaseSandboxNetworkPolicy BasePolicyFromAllowInternetAccess(
    const core::Optional<bool>& value) {
    // Absent means "inherit the node default", which is a third state rather
    // than a synonym for deny.
    if (!value.has_value()) return sandbox::network::BaseSandboxNetworkPolicy::Default;
    return *value ? sandbox::network::BaseSandboxNetworkPolicy::Allow
                  : sandbox::network::BaseSandboxNetworkPolicy::Deny;
}

core::Optional<bool> AllowInternetAccessFromBasePolicy(
    sandbox::network::BaseSandboxNetworkPolicy policy) {
    switch (policy) {
        case sandbox::network::BaseSandboxNetworkPolicy::Allow:
            return core::Optional<bool>(true);
        case sandbox::network::BaseSandboxNetworkPolicy::Deny:
            return core::Optional<bool>(false);
        case sandbox::network::BaseSandboxNetworkPolicy::Default:
            break;
    }
    // Null, not false: the field is nullable so a client can tell "not set"
    // from "denied".
    return core::Optional<bool>();
}

std::vector<SandboxVolumeMountModel> VolumeMountsModel(
    const std::map<std::string, std::string>& mounts) {
    std::vector<SandboxVolumeMountModel> models;
    models.reserve(mounts.size());
    for (std::map<std::string, std::string>::const_iterator it = mounts.begin();
         it != mounts.end(); ++it) {
        SandboxVolumeMountModel model;
        // The internal map is keyed by mount path and valued by volume name;
        // the wire shape names both explicitly.
        model.path = it->first;
        model.name = it->second;
        models.push_back(model);
    }
    return models;
}

core::Expected<core::Optional<std::map<std::string, std::string> >, ApiError>
VolumeMountsFromModel(const std::vector<SandboxVolumeMountModel>& mounts) {
    // Empty is "no mounts", not an empty mount set.
    if (mounts.empty()) {
        return core::Optional<std::map<std::string, std::string> >();
    }

    std::map<std::string, std::string> result;
    for (std::size_t i = 0; i < mounts.size(); ++i) {
        // A repeated path would silently let the last one win, and the sandbox
        // would come up with a volume the caller did not ask for there.
        if (result.find(mounts[i].path) != result.end()) {
            return core::make_unexpected(
                ApiError::Make(400, "duplicate volume mount path: " + mounts[i].path));
        }
        result[mounts[i].path] = mounts[i].name;
    }
    return core::Optional<std::map<std::string, std::string> >(result);
}

namespace {

/// Percent-decoding for one `application/x-www-form-urlencoded` component.
std::string FormUrlDecode(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '+') {
            out.push_back(' ');
            continue;
        }
        if (raw[i] == '%' && i + 2 < raw.size()) {
            const char hi = raw[i + 1];
            const char lo = raw[i + 2];
            int hi_v = -1;
            int lo_v = -1;
            if (hi >= '0' && hi <= '9') hi_v = hi - '0';
            else if (hi >= 'a' && hi <= 'f') hi_v = hi - 'a' + 10;
            else if (hi >= 'A' && hi <= 'F') hi_v = hi - 'A' + 10;
            if (lo >= '0' && lo <= '9') lo_v = lo - '0';
            else if (lo >= 'a' && lo <= 'f') lo_v = lo - 'a' + 10;
            else if (lo >= 'A' && lo <= 'F') lo_v = lo - 'A' + 10;
            if (hi_v >= 0 && lo_v >= 0) {
                out.push_back(static_cast<char>(hi_v * 16 + lo_v));
                i += 2;
                continue;
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

}  // namespace

core::Optional<std::map<std::string, std::string> > ParseMetadataFilter(
    const core::Optional<std::string>& raw) {
    if (!raw.has_value()) return core::Optional<std::map<std::string, std::string> >();

    std::map<std::string, std::string> filter;
    const std::string& text = *raw;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t amp = text.find('&', begin);
        const std::string pair =
            amp == std::string::npos ? text.substr(begin) : text.substr(begin, amp - begin);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            const std::string key =
                FormUrlDecode(eq == std::string::npos ? pair : pair.substr(0, eq));
            const std::string value =
                eq == std::string::npos ? std::string() : FormUrlDecode(pair.substr(eq + 1));
            // Both halves must carry something: a blank key or value cannot
            // select anything.
            if (!key.empty() && !value.empty()) filter[key] = value;
        }
        if (amp == std::string::npos) break;
        begin = amp + 1;
    }

    // A filter that kept nothing is absent, not an empty filter that would
    // match no sandbox at all.
    if (filter.empty()) return core::Optional<std::map<std::string, std::string> >();
    return core::Optional<std::map<std::string, std::string> >(filter);
}

core::Expected<sandbox::SandboxResources, ApiError> ColdStartResources(
    const NewColdSandboxResources& body) {
    const cfg::AppConfig* config = cfg::ConfigManager::GlobalConfig();
    // Rust reads the global config unconditionally; a missing global falls
    // back to the same literals `MachineConfig` declares.
    uint32_t cpu_count  = config != NULL ? config->machine.vcpu_count : 2;
    uint32_t memory_mib = config != NULL ? config->machine.mem_size_mib : 1024;
    if (body.cpu_count.has_value()) cpu_count = *body.cpu_count;
    if (body.memory_mb.has_value()) memory_mib = *body.memory_mb;

    if (cpu_count == 0) {
        return core::make_unexpected(ApiError::Make(400, "cpuCount must be greater than 0"));
    }
    // 128 MiB is the floor the guest kernel plus envd need to reach a usable
    // state at all.
    if (memory_mib < 128) {
        return core::make_unexpected(ApiError::Make(400, "memoryMB must be at least 128"));
    }

    const uint32_t disk_size_mib = body.disk_size_mb.has_value() ? *body.disk_size_mb : 0;
    if (body.disk_size_mb.has_value() && (disk_size_mib < 1024 || disk_size_mib % 1024 != 0)) {
        // The block layer allocates in 1024 MiB units, so a non-multiple would
        // be silently rounded to something the caller did not ask for.
        return core::make_unexpected(
            ApiError::Make(400, "diskSizeMB must be at least 1024 and divisible by 1024"));
    }

    sandbox::SandboxResources resources;
    resources.cpu_count  = cpu_count;
    resources.memory_mib = memory_mib;
    // Zero means omitted; the orchestrator fills it from the resolved rootfs.
    resources.disk_size_mib = disk_size_mib;
    return resources;
}

core::Expected<core::Unit, std::string> ValidateIpv4Cidrs(
    const sandbox::network::SandboxNetworkPolicy& policy) {
    // Rejected rather than ignored: the runtime installs only v4 rules, so an
    // accepted v6 rule would read as a granted restriction that is not in
    // effect.
    for (std::size_t i = 0; i < policy.egress.allowed_cidrs.size(); ++i) {
        if (!policy.egress.allowed_cidrs[i].is_v4) {
            return core::make_unexpected(
                std::string("IPv6 CIDRs are not supported by the sandbox network API"));
        }
    }
    for (std::size_t i = 0; i < policy.egress.denied_cidrs.size(); ++i) {
        if (!policy.egress.denied_cidrs[i].is_v4) {
            return core::make_unexpected(
                std::string("IPv6 CIDRs are not supported by the sandbox network API"));
        }
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> ValidateDomainAllowlist(
    const sandbox::network::SandboxNetworkPolicy& policy) {
    if (!policy.HasDomainAllowRules()) return core::Unit();

    // Domain inspection only applies to HTTP/HTTPS (TCP 80/443); every other
    // port stays CIDR-only. Without an explicit ALL_TRAFFIC deny, a policy
    // that names domains would leave all those other ports wide open while
    // reading as a restriction.
    //
    // A mixed domain/CIDR policy is *not* rejected: the runtime still honours
    // explicit CIDR grants on every port.
    bool denies_all = false;
    for (std::size_t i = 0; i < policy.egress.denied_cidrs.size(); ++i) {
        if (policy.egress.denied_cidrs[i].ToString() ==
            sandbox::network::IpNetwork::Parse(sandbox::network::kAllInternetTrafficCidr).value().ToString()) {
            denies_all = true;
        }
    }
    if (!denies_all) {
        return core::make_unexpected(std::string("allowOut contains domains but denyOut is "
                                                 "missing ") +
                                     sandbox::network::kAllInternetTrafficCidr + " (ALL_TRAFFIC)");
    }
    return core::Unit();
}

namespace {

core::Expected<sandbox::network::SandboxNetworkPolicy, std::string> FinishPolicy(
    bool allow_public_traffic, sandbox::network::BaseSandboxNetworkPolicy base_policy,
    const core::Optional<std::vector<std::string> >& allow_out,
    const core::Optional<std::vector<std::string> >& deny_out) {
    const core::Expected<sandbox::network::SandboxNetworkEgressPolicy, std::string> egress =
        sandbox::network::SandboxNetworkEgressPolicy::New(allow_out, deny_out);
    if (!egress.ok()) return core::make_unexpected(egress.error());

    const sandbox::network::SandboxNetworkPolicy policy =
        sandbox::network::SandboxNetworkPolicy::New(allow_public_traffic, base_policy, egress.value());

    const core::Expected<core::Unit, std::string> v4 = ValidateIpv4Cidrs(policy);
    if (!v4.ok()) return core::make_unexpected(v4.error());
    const core::Expected<core::Unit, std::string> domains = ValidateDomainAllowlist(policy);
    if (!domains.ok()) return core::make_unexpected(domains.error());
    return policy;
}

}  // namespace

core::Expected<sandbox::network::SandboxNetworkPolicy, std::string> NetworkPolicyFromCreate(
    const core::Optional<bool>& allow_internet_access,
    const SandboxNetworkConfigModel* network) {
    core::Optional<std::vector<std::string> > allow_out;
    core::Optional<std::vector<std::string> > deny_out;
    // Defaults to public: an omitted network block describes an ordinary
    // reachable sandbox.
    bool allow_public_traffic = true;
    if (network != NULL) {
        allow_out = network->allow_out;
        deny_out  = network->deny_out;
        if (network->allow_public_traffic.has_value()) {
            allow_public_traffic = *network->allow_public_traffic;
        }
    }
    return FinishPolicy(allow_public_traffic,
                        BasePolicyFromAllowInternetAccess(allow_internet_access), allow_out,
                        deny_out);
}

core::Expected<sandbox::network::SandboxNetworkPolicy, std::string> NetworkPolicyFromUpdate(
    const SandboxNetworkConfigModel& body, const core::Optional<bool>& allow_internet_access) {
    // An update always describes a public sandbox; only the egress rules and
    // the base policy are patchable.
    return FinishPolicy(true, BasePolicyFromAllowInternetAccess(allow_internet_access),
                        body.allow_out, body.deny_out);
}

bool ValidMetricsInterval(const core::Optional<uint64_t>& start,
                          const core::Optional<uint64_t>& end) {
    const uint64_t kMaxSigned = static_cast<uint64_t>(INT64_MAX);
    // Each bound has to survive conversion to the signed timestamp the store
    // uses.
    if (start.has_value() && *start > kMaxSigned) return false;
    if (end.has_value() && *end > kMaxSigned) return false;
    // An inverted range would silently return nothing.
    if (start.has_value() && end.has_value() && *start > *end) return false;
    return true;
}

core::Expected<std::vector<core::SandboxId>, std::string> ParseMetricsIds(
    const std::vector<std::string>& values) {
    // A repeated query parameter is a caller error, not a second list.
    if (values.size() != 1) {
        return core::make_unexpected(
            std::string("sandbox_ids must be a single comma-separated list"));
    }

    std::vector<std::string> raw_ids;
    const std::string& joined = values[0];
    std::size_t begin = 0;
    while (begin <= joined.size()) {
        const std::size_t comma = joined.find(',', begin);
        raw_ids.push_back(comma == std::string::npos ? joined.substr(begin)
                                                     : joined.substr(begin, comma - begin));
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }

    if (raw_ids.size() > 100) {
        return core::make_unexpected(std::string("sandbox_ids must contain at most 100 IDs"));
    }

    std::set<std::string> seen;
    std::vector<core::SandboxId> parsed;
    for (std::size_t i = 0; i < raw_ids.size(); ++i) {
        const core::Expected<core::SandboxId, std::string> id =
            core::SandboxId::Parse(raw_ids[i]);
        if (!id.ok()) {
            return core::make_unexpected(
                std::string("sandbox_ids must contain valid sandbox IDs"));
        }
        // Duplicates would produce two response entries for one sandbox.
        if (!seen.insert(id.value().ToString()).second) {
            return core::make_unexpected(std::string("sandbox_ids must contain distinct IDs"));
        }
        parsed.push_back(id.value());
    }
    return parsed;
}

}  // namespace api
}  // namespace agentenv
