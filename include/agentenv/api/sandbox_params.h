// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs — the request-validation and projection
// helpers, extracted from the handler bodies.
//
// The handlers themselves are bound to the generated route response enums and
// drive the orchestrator; what is portable (and worth porting) is the set of
// free functions that decide whether a request is acceptable and how it maps
// onto the internal types. Several of them encode rules that are not obvious
// from the field names:
//
//  * A domain allowlist is only enforceable for HTTP/HTTPS, so a policy that
//    names domains without also denying ALL_TRAFFIC would silently leak every
//    other port.
//  * IPv6 CIDRs are rejected outright rather than ignored, because the
//    runtime installs only v4 rules and an accepted-but-unenforced v6 rule
//    reads as a granted restriction that is not actually in effect.
//  * `diskSizeMB` must be a whole number of GiB: the block layer allocates in
//    1024 MiB units and a non-multiple would be silently rounded.
#ifndef AGENTENV_API_SANDBOX_PARAMS_H_
#define AGENTENV_API_SANDBOX_PARAMS_H_

#include <map>
#include <string>
#include <vector>

#include "agentenv/api/dto.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/network/policy.h"
#include "agentenv/sandbox/types.h"

namespace agentenv {
namespace api {

/// Rust generated `models::SandboxVolumeMount`.
struct SandboxVolumeMountModel {
    std::string name;
    std::string path;
};

/// Rust generated `models::NewColdSandbox`, restricted to the resource fields.
struct NewColdSandboxResources {
    core::Optional<uint32_t> cpu_count;
    core::Optional<uint32_t> memory_mb;
    core::Optional<uint32_t> disk_size_mb;
};

/// Rust generated `models::SandboxNetworkConfig`.
struct SandboxNetworkConfigModel {
    core::Optional<std::vector<std::string> > allow_out;
    core::Optional<std::vector<std::string> > deny_out;
    core::Optional<bool>                      allow_public_traffic;
};

/// Rust `base_policy_from_allow_internet_access` — a tri-state: absent means
/// "inherit the node default", which is distinct from an explicit allow/deny.
sandbox::network::BaseSandboxNetworkPolicy BasePolicyFromAllowInternetAccess(
    const core::Optional<bool>& value);

/// Rust `allow_internet_access_from_base_policy`.
///
/// `Default` maps back to a JSON null rather than to `false`: the field is
/// nullable precisely so a client can tell "not set" from "denied".
core::Optional<bool> AllowInternetAccessFromBasePolicy(
    sandbox::network::BaseSandboxNetworkPolicy policy);

/// Rust `volume_mounts_model` — internal map (path -> volume name) to the
/// wire list.
std::vector<SandboxVolumeMountModel> VolumeMountsModel(
    const std::map<std::string, std::string>& mounts);

/// Rust `volume_mounts_from_model`.
///
/// An absent or empty list is "no mounts", not an empty mount set. A repeated
/// path is rejected: the last one would otherwise silently win and the
/// sandbox would come up with a volume the caller did not ask for at that
/// path.
core::Expected<core::Optional<std::map<std::string, std::string> >, ApiError>
    VolumeMountsFromModel(const std::vector<SandboxVolumeMountModel>& mounts);

/// Rust `parse_metadata_filter` — an `application/x-www-form-urlencoded`
/// query value. Blank keys and blank values are dropped, and a filter that
/// keeps nothing is reported as absent rather than as an empty filter that
/// would match nothing.
core::Optional<std::map<std::string, std::string> > ParseMetadataFilter(
    const core::Optional<std::string>& raw);

/// Rust `cold_start_resources`.
core::Expected<sandbox::SandboxResources, ApiError> ColdStartResources(
    const NewColdSandboxResources& body);

/// Rust `validate_ipv4_cidrs`.
core::Expected<core::Unit, std::string> ValidateIpv4Cidrs(
    const sandbox::network::SandboxNetworkPolicy& policy);

/// Rust `validate_domain_allowlist`.
core::Expected<core::Unit, std::string> ValidateDomainAllowlist(
    const sandbox::network::SandboxNetworkPolicy& policy);

/// Rust `network_policy_from_create`.
core::Expected<sandbox::network::SandboxNetworkPolicy, std::string> NetworkPolicyFromCreate(
    const core::Optional<bool>& allow_internet_access,
    const SandboxNetworkConfigModel* network);

/// Rust `network_policy_from_update` — an update always describes a public
/// sandbox (`allow_public_traffic = true`); only the egress rules and the base
/// policy are patchable.
core::Expected<sandbox::network::SandboxNetworkPolicy, std::string> NetworkPolicyFromUpdate(
    const SandboxNetworkConfigModel& body,
    const core::Optional<bool>& allow_internet_access);

/// Rust `valid_metrics_interval` — each bound must fit a signed 64-bit
/// timestamp, and the range must not be inverted.
bool ValidMetricsInterval(const core::Optional<uint64_t>& start,
                          const core::Optional<uint64_t>& end);

/// Rust `parse_metrics_ids`.
///
/// Expects exactly one comma-separated value (a repeated query parameter is a
/// caller error, not a second list), caps the batch at 100, and rejects
/// duplicates so a response cannot carry two entries for one sandbox.
core::Expected<std::vector<core::SandboxId>, std::string> ParseMetricsIds(
    const std::vector<std::string>& values);

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_SANDBOX_PARAMS_H_
