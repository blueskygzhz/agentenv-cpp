// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs — the validation/projection helpers.
#include "agentenv/api/sandbox_params.h"

#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT

namespace {

using agentenv::sandbox::network::BaseSandboxNetworkPolicy;
using agentenv::sandbox::network::SandboxNetworkEgressPolicy;
using agentenv::sandbox::network::SandboxNetworkPolicy;

std::vector<std::string> List(const std::string& a) {
    std::vector<std::string> out;
    out.push_back(a);
    return out;
}

std::vector<std::string> List(const std::string& a, const std::string& b) {
    std::vector<std::string> out;
    out.push_back(a);
    out.push_back(b);
    return out;
}

SandboxVolumeMountModel Mount(const std::string& name, const std::string& path) {
    SandboxVolumeMountModel mount;
    mount.name = name;
    mount.path = path;
    return mount;
}

}  // namespace

// ---- base policy tri-state -------------------------------------------------

MT_TEST(sandbox_base_policy_is_a_tri_state) {
    MT_EXPECT_TRUE(BasePolicyFromAllowInternetAccess(core::Optional<bool>(true)) ==
                   BaseSandboxNetworkPolicy::Allow);
    MT_EXPECT_TRUE(BasePolicyFromAllowInternetAccess(core::Optional<bool>(false)) ==
                   BaseSandboxNetworkPolicy::Deny);
    // Absent is not a synonym for deny: it means "inherit the node default".
    MT_EXPECT_TRUE(BasePolicyFromAllowInternetAccess(core::Optional<bool>()) ==
                   BaseSandboxNetworkPolicy::Default);
}

MT_TEST(sandbox_base_policy_round_trips_through_null) {
    MT_EXPECT_TRUE(*AllowInternetAccessFromBasePolicy(BaseSandboxNetworkPolicy::Allow));
    MT_EXPECT_TRUE(!*AllowInternetAccessFromBasePolicy(BaseSandboxNetworkPolicy::Deny));
    // Default maps back to null, not false — the field is nullable precisely
    // so a client can tell "not set" from "denied".
    MT_EXPECT_TRUE(
        !AllowInternetAccessFromBasePolicy(BaseSandboxNetworkPolicy::Default).has_value());
}

// ---- volume mounts ---------------------------------------------------------

MT_TEST(sandbox_volume_mounts_project_path_and_name) {
    std::map<std::string, std::string> mounts;
    mounts["/mnt/data"] = "vol-a";

    const std::vector<SandboxVolumeMountModel> models = VolumeMountsModel(mounts);
    MT_EXPECT_EQ(models.size(), static_cast<std::size_t>(1));
    // The internal map is keyed by path and valued by name; getting these
    // backwards would silently mount the wrong volume.
    MT_EXPECT_EQ(models[0].path, std::string("/mnt/data"));
    MT_EXPECT_EQ(models[0].name, std::string("vol-a"));
}

MT_TEST(sandbox_volume_mounts_from_model_treats_empty_as_absent) {
    const core::Expected<core::Optional<std::map<std::string, std::string> >, ApiError> parsed =
        VolumeMountsFromModel(std::vector<SandboxVolumeMountModel>());
    MT_EXPECT_TRUE(parsed.ok());
    // Absent, not an empty mount set.
    MT_EXPECT_TRUE(!parsed.value().has_value());
}

MT_TEST(sandbox_volume_mounts_from_model_rejects_a_duplicate_path) {
    std::vector<SandboxVolumeMountModel> mounts;
    mounts.push_back(Mount("vol-a", "/mnt/data"));
    mounts.push_back(Mount("vol-b", "/mnt/data"));

    // Silently letting the last one win would bring the sandbox up with a
    // volume the caller did not ask for at that path.
    const core::Expected<core::Optional<std::map<std::string, std::string> >, ApiError> parsed =
        VolumeMountsFromModel(mounts);
    MT_EXPECT_TRUE(!parsed.ok());
    MT_EXPECT_EQ(parsed.error().code, 400);
    MT_EXPECT_TRUE(parsed.error().message.find("duplicate volume mount path") !=
                   std::string::npos);
}

MT_TEST(sandbox_volume_mounts_from_model_allows_one_volume_at_two_paths) {
    std::vector<SandboxVolumeMountModel> mounts;
    // Distinct paths, same volume: this helper only guards the path key; the
    // "mounted more than once" rule belongs to mount resolution.
    mounts.push_back(Mount("vol-a", "/mnt/one"));
    mounts.push_back(Mount("vol-a", "/mnt/two"));

    const core::Expected<core::Optional<std::map<std::string, std::string> >, ApiError> parsed =
        VolumeMountsFromModel(mounts);
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value()->size(), static_cast<std::size_t>(2));
}

// ---- metadata filter -------------------------------------------------------

MT_TEST(sandbox_metadata_filter_parses_form_encoded_pairs) {
    const core::Optional<std::map<std::string, std::string> > filter =
        ParseMetadataFilter(core::Optional<std::string>(std::string("a=1&b=2")));
    MT_EXPECT_TRUE(filter.has_value());
    MT_EXPECT_EQ(filter->size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(filter->find("a")->second, std::string("1"));
    MT_EXPECT_EQ(filter->find("b")->second, std::string("2"));
}

MT_TEST(sandbox_metadata_filter_percent_decodes) {
    const core::Optional<std::map<std::string, std::string> > filter = ParseMetadataFilter(
        core::Optional<std::string>(std::string("my%20key=my%2Fvalue&plus=a+b")));
    MT_EXPECT_TRUE(filter.has_value());
    MT_EXPECT_EQ(filter->find("my key")->second, std::string("my/value"));
    // `+` is a space in form encoding, not a literal plus.
    MT_EXPECT_EQ(filter->find("plus")->second, std::string("a b"));
}

MT_TEST(sandbox_metadata_filter_drops_blank_halves) {
    // Neither a blank key nor a blank value can select anything.
    const core::Optional<std::map<std::string, std::string> > filter =
        ParseMetadataFilter(core::Optional<std::string>(std::string("=1&b=&c=3")));
    MT_EXPECT_TRUE(filter.has_value());
    MT_EXPECT_EQ(filter->size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(filter->find("c")->second, std::string("3"));
}

MT_TEST(sandbox_metadata_filter_is_absent_when_nothing_survives) {
    MT_EXPECT_TRUE(!ParseMetadataFilter(core::Optional<std::string>()).has_value());
    // An empty filter would match no sandbox at all, so "kept nothing" must
    // report as absent rather than as a filter.
    MT_EXPECT_TRUE(
        !ParseMetadataFilter(core::Optional<std::string>(std::string(""))).has_value());
    MT_EXPECT_TRUE(
        !ParseMetadataFilter(core::Optional<std::string>(std::string("=1&b="))).has_value());
}

// ---- cold start resources --------------------------------------------------

MT_TEST(sandbox_cold_start_resources_use_request_values) {
    NewColdSandboxResources body;
    body.cpu_count   = static_cast<uint32_t>(4);
    body.memory_mb   = static_cast<uint32_t>(2048);
    body.disk_size_mb = static_cast<uint32_t>(4096);

    const core::Expected<sandbox::SandboxResources, ApiError> resources =
        ColdStartResources(body);
    MT_EXPECT_TRUE(resources.ok());
    MT_EXPECT_EQ(resources.value().cpu_count, static_cast<uint32_t>(4));
    MT_EXPECT_EQ(resources.value().memory_mib, static_cast<uint32_t>(2048));
    MT_EXPECT_EQ(resources.value().disk_size_mib, static_cast<uint32_t>(4096));
}

MT_TEST(sandbox_cold_start_resources_reject_zero_cpu) {
    NewColdSandboxResources body;
    body.cpu_count = static_cast<uint32_t>(0);
    const core::Expected<sandbox::SandboxResources, ApiError> resources =
        ColdStartResources(body);
    MT_EXPECT_TRUE(!resources.ok());
    MT_EXPECT_EQ(resources.error().code, 400);
    MT_EXPECT_TRUE(resources.error().message.find("cpuCount") != std::string::npos);
}

MT_TEST(sandbox_cold_start_resources_enforce_the_memory_floor) {
    NewColdSandboxResources body;
    // 128 MiB is what the guest kernel plus envd need to reach a usable state.
    body.memory_mb = static_cast<uint32_t>(127);
    const core::Expected<sandbox::SandboxResources, ApiError> low = ColdStartResources(body);
    MT_EXPECT_TRUE(!low.ok());
    MT_EXPECT_TRUE(low.error().message.find("at least 128") != std::string::npos);

    body.memory_mb = static_cast<uint32_t>(128);
    MT_EXPECT_TRUE(ColdStartResources(body).ok());
}

MT_TEST(sandbox_cold_start_disk_must_be_whole_gibibytes) {
    NewColdSandboxResources body;
    // The block layer allocates in 1024 MiB units, so a non-multiple would be
    // silently rounded to something the caller did not ask for.
    body.disk_size_mb = static_cast<uint32_t>(1500);
    MT_EXPECT_TRUE(!ColdStartResources(body).ok());

    body.disk_size_mb = static_cast<uint32_t>(512);
    MT_EXPECT_TRUE(!ColdStartResources(body).ok());

    body.disk_size_mb = static_cast<uint32_t>(2048);
    MT_EXPECT_TRUE(ColdStartResources(body).ok());
}

MT_TEST(sandbox_cold_start_omitted_disk_stays_zero) {
    // Zero means omitted; the orchestrator fills it from the resolved rootfs.
    // The divisibility rule must therefore not fire for an absent field.
    const NewColdSandboxResources body;
    const core::Expected<sandbox::SandboxResources, ApiError> resources =
        ColdStartResources(body);
    MT_EXPECT_TRUE(resources.ok());
    MT_EXPECT_EQ(resources.value().disk_size_mib, static_cast<uint32_t>(0));
}

// ---- network policy validation ---------------------------------------------

MT_TEST(sandbox_policy_rejects_ipv6_cidrs) {
    const core::Expected<SandboxNetworkEgressPolicy, std::string> egress =
        SandboxNetworkEgressPolicy::New(
            core::Optional<std::vector<std::string> >(List("2001:db8::/32")),
            core::Optional<std::vector<std::string> >());
    if (!egress.ok()) return;  // rejected even earlier, which is also correct

    const SandboxNetworkPolicy policy = SandboxNetworkPolicy::New(
        true, BaseSandboxNetworkPolicy::Default, egress.value());
    // Rejected rather than ignored: the runtime installs only v4 rules, so an
    // accepted v6 rule would read as a restriction that is not in effect.
    const core::Expected<core::Unit, std::string> checked = ValidateIpv4Cidrs(policy);
    MT_EXPECT_TRUE(!checked.ok());
    MT_EXPECT_TRUE(checked.error().find("IPv6") != std::string::npos);
}

MT_TEST(sandbox_policy_accepts_ipv4_cidrs) {
    const core::Expected<SandboxNetworkEgressPolicy, std::string> egress =
        SandboxNetworkEgressPolicy::New(
            core::Optional<std::vector<std::string> >(List("10.0.0.0/8")),
            core::Optional<std::vector<std::string> >(List("0.0.0.0/0")));
    MT_EXPECT_TRUE(egress.ok());
    const SandboxNetworkPolicy policy = SandboxNetworkPolicy::New(
        true, BaseSandboxNetworkPolicy::Default, egress.value());
    MT_EXPECT_TRUE(ValidateIpv4Cidrs(policy).ok());
}

MT_TEST(sandbox_domain_allowlist_requires_an_all_traffic_deny) {
    const core::Expected<SandboxNetworkEgressPolicy, std::string> egress =
        SandboxNetworkEgressPolicy::New(
            core::Optional<std::vector<std::string> >(List("example.com")),
            core::Optional<std::vector<std::string> >());
    MT_EXPECT_TRUE(egress.ok());
    const SandboxNetworkPolicy policy = SandboxNetworkPolicy::New(
        true, BaseSandboxNetworkPolicy::Default, egress.value());

    // Domain inspection only covers HTTP/HTTPS; without an ALL_TRAFFIC deny
    // every other port stays wide open while the policy reads as a
    // restriction.
    const core::Expected<core::Unit, std::string> checked = ValidateDomainAllowlist(policy);
    MT_EXPECT_TRUE(!checked.ok());
    MT_EXPECT_TRUE(checked.error().find("ALL_TRAFFIC") != std::string::npos);
}

MT_TEST(sandbox_domain_allowlist_passes_with_the_all_traffic_deny) {
    const core::Expected<SandboxNetworkEgressPolicy, std::string> egress =
        SandboxNetworkEgressPolicy::New(
            core::Optional<std::vector<std::string> >(List("example.com")),
            core::Optional<std::vector<std::string> >(
                List(agentenv::sandbox::network::kAllInternetTrafficCidr)));
    MT_EXPECT_TRUE(egress.ok());
    const SandboxNetworkPolicy policy = SandboxNetworkPolicy::New(
        true, BaseSandboxNetworkPolicy::Default, egress.value());
    MT_EXPECT_TRUE(ValidateDomainAllowlist(policy).ok());
}

MT_TEST(sandbox_domain_allowlist_permits_mixed_domain_and_cidr) {
    const core::Expected<SandboxNetworkEgressPolicy, std::string> egress =
        SandboxNetworkEgressPolicy::New(
            core::Optional<std::vector<std::string> >(List("example.com", "10.0.0.0/8")),
            core::Optional<std::vector<std::string> >(
                List(agentenv::sandbox::network::kAllInternetTrafficCidr)));
    MT_EXPECT_TRUE(egress.ok());
    const SandboxNetworkPolicy policy = SandboxNetworkPolicy::New(
        true, BaseSandboxNetworkPolicy::Default, egress.value());
    // Not rejected: the runtime still honours explicit CIDR grants on every
    // port, so a mixed policy is meaningful.
    MT_EXPECT_TRUE(ValidateDomainAllowlist(policy).ok());
}

MT_TEST(sandbox_policy_without_domains_needs_no_all_traffic_deny) {
    const core::Expected<SandboxNetworkEgressPolicy, std::string> egress =
        SandboxNetworkEgressPolicy::New(
            core::Optional<std::vector<std::string> >(List("10.0.0.0/8")),
            core::Optional<std::vector<std::string> >());
    MT_EXPECT_TRUE(egress.ok());
    const SandboxNetworkPolicy policy = SandboxNetworkPolicy::New(
        true, BaseSandboxNetworkPolicy::Default, egress.value());
    MT_EXPECT_TRUE(ValidateDomainAllowlist(policy).ok());
}

// ---- policy construction ---------------------------------------------------

MT_TEST(sandbox_policy_from_create_defaults_to_public) {
    const core::Expected<SandboxNetworkPolicy, std::string> policy =
        NetworkPolicyFromCreate(core::Optional<bool>(), NULL);
    MT_EXPECT_TRUE(policy.ok());
    // An omitted network block describes an ordinary reachable sandbox.
    MT_EXPECT_TRUE(policy.value().allow_public_traffic);
    MT_EXPECT_TRUE(policy.value().base_policy == BaseSandboxNetworkPolicy::Default);
}

MT_TEST(sandbox_policy_from_create_honours_the_network_block) {
    SandboxNetworkConfigModel network;
    network.allow_out            = List("10.0.0.0/8");
    network.allow_public_traffic = core::Optional<bool>(false);

    const core::Expected<SandboxNetworkPolicy, std::string> policy =
        NetworkPolicyFromCreate(core::Optional<bool>(true), &network);
    MT_EXPECT_TRUE(policy.ok());
    MT_EXPECT_TRUE(!policy.value().allow_public_traffic);
    MT_EXPECT_TRUE(policy.value().base_policy == BaseSandboxNetworkPolicy::Allow);
    MT_EXPECT_EQ(policy.value().egress.allowed_cidrs.size(), static_cast<std::size_t>(1));
}

MT_TEST(sandbox_policy_from_create_propagates_validation_failures) {
    SandboxNetworkConfigModel network;
    // Domains with no ALL_TRAFFIC deny must fail construction, not just the
    // standalone validator.
    network.allow_out = List("example.com");
    MT_EXPECT_TRUE(!NetworkPolicyFromCreate(core::Optional<bool>(), &network).ok());
}

MT_TEST(sandbox_policy_from_update_is_always_public) {
    SandboxNetworkConfigModel body;
    body.allow_out = List("10.0.0.0/8");
    // An update only patches the egress rules and the base policy; public
    // reachability is not part of that surface.
    body.allow_public_traffic = core::Optional<bool>(false);

    const core::Expected<SandboxNetworkPolicy, std::string> policy =
        NetworkPolicyFromUpdate(body, core::Optional<bool>(false));
    MT_EXPECT_TRUE(policy.ok());
    MT_EXPECT_TRUE(policy.value().allow_public_traffic);
    MT_EXPECT_TRUE(policy.value().base_policy == BaseSandboxNetworkPolicy::Deny);
}

// ---- metrics ---------------------------------------------------------------

MT_TEST(sandbox_metrics_interval_validation) {
    MT_EXPECT_TRUE(ValidMetricsInterval(core::Optional<uint64_t>(), core::Optional<uint64_t>()));
    MT_EXPECT_TRUE(ValidMetricsInterval(core::Optional<uint64_t>(static_cast<uint64_t>(10)),
                                        core::Optional<uint64_t>(static_cast<uint64_t>(20))));
    // Equal bounds are a valid (empty) window.
    MT_EXPECT_TRUE(ValidMetricsInterval(core::Optional<uint64_t>(static_cast<uint64_t>(10)),
                                        core::Optional<uint64_t>(static_cast<uint64_t>(10))));
    // Inverted would silently return nothing.
    MT_EXPECT_TRUE(!ValidMetricsInterval(core::Optional<uint64_t>(static_cast<uint64_t>(20)),
                                         core::Optional<uint64_t>(static_cast<uint64_t>(10))));
    // Must survive conversion to the signed timestamp the store uses.
    const uint64_t too_big = static_cast<uint64_t>(INT64_MAX) + 1;
    MT_EXPECT_TRUE(
        !ValidMetricsInterval(core::Optional<uint64_t>(too_big), core::Optional<uint64_t>()));
    MT_EXPECT_TRUE(
        !ValidMetricsInterval(core::Optional<uint64_t>(), core::Optional<uint64_t>(too_big)));
}

MT_TEST(sandbox_metrics_ids_require_exactly_one_value) {
    // A repeated query parameter is a caller error, not a second list.
    MT_EXPECT_TRUE(!ParseMetricsIds(std::vector<std::string>()).ok());
    MT_EXPECT_TRUE(!ParseMetricsIds(List("a", "b")).ok());
}

MT_TEST(sandbox_metrics_ids_parse_a_comma_separated_list) {
    const core::SandboxId first  = core::SandboxId::Fresh();
    const core::SandboxId second = core::SandboxId::Fresh();
    const core::Expected<std::vector<core::SandboxId>, std::string> parsed =
        ParseMetricsIds(List(first.ToString() + "," + second.ToString()));
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().size(), static_cast<std::size_t>(2));
}

MT_TEST(sandbox_metrics_ids_reject_duplicates) {
    const core::SandboxId id = core::SandboxId::Fresh();
    // Two response entries for one sandbox would be ambiguous.
    const core::Expected<std::vector<core::SandboxId>, std::string> parsed =
        ParseMetricsIds(List(id.ToString() + "," + id.ToString()));
    MT_EXPECT_TRUE(!parsed.ok());
    MT_EXPECT_TRUE(parsed.error().find("distinct") != std::string::npos);
}

MT_TEST(sandbox_metrics_ids_reject_invalid_and_overlong_lists) {
    MT_EXPECT_TRUE(!ParseMetricsIds(List("not-a-uuid")).ok());

    std::string many;
    for (int i = 0; i < 101; ++i) {
        if (i != 0) many += ",";
        many += core::SandboxId::Fresh().ToString();
    }
    const core::Expected<std::vector<core::SandboxId>, std::string> parsed =
        ParseMetricsIds(List(many));
    MT_EXPECT_TRUE(!parsed.ok());
    MT_EXPECT_TRUE(parsed.error().find("at most 100") != std::string::npos);
}

int main() { return microtest::RunAll(); }
