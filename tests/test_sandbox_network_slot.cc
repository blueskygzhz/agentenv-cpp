// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/slot.rs :: tests
//
// Covers the parts of `Slot` that do not need root: index validation, address
// derivation, the `ip=` boot argument, namespace paths, the namespace iptables
// rule set, and resolv.conf parsing. The kernel-touching paths
// (`CreateNetwork` / `Cleanup`) need CAP_NET_ADMIN and a real namespace, so
// they are exercised by the integration suite instead.
#include "microtest.h"

#include <string>
#include <vector>

#include "agentenv/cfg/network.h"
#include "agentenv/sandbox/network/slot.h"

using namespace agentenv;
using namespace agentenv::sandbox::network;

namespace {

uint32_t Ip(const std::string& dotted) {
    uint32_t v = 0;
    Ipv4Parse(dotted, &v);
    return v;
}

}  // namespace

// ============================================================
// slot.rs :: test_slot_ip_calculation
// ============================================================
MT_TEST(slot_new_derives_addresses_from_plan) {
    NetworkAddressPlan plan = NetworkAddressPlan::Default();
    core::Expected<Slot*, NetworkError> slot = Slot::New(1, plan, "/tmp/aenv-ns-test");
    MT_EXPECT_TRUE(slot.ok());

    // The three addresses must match what the plan derives for this index.
    uint32_t host_interaction = 0, veth_host = 0, veth_vm = 0;
    MT_EXPECT_TRUE(plan.SlotIps(1, &host_interaction, &veth_host, &veth_vm).ok());
    MT_EXPECT_EQ(slot.value()->HostInteractionIp(), host_interaction);
    MT_EXPECT_EQ(slot.value()->VethHostIp(), veth_host);
    MT_EXPECT_EQ(slot.value()->VethVmIp(), veth_vm);
    MT_EXPECT_EQ(slot.value()->Idx(), static_cast<uint32_t>(1));

    // `New` must not have created kernel state, so the destructor is a no-op.
    delete slot.value();
}

// ============================================================
// slot.rs :: test_slot_overflow
// ============================================================
MT_TEST(slot_new_rejects_zero_and_out_of_range_indices) {
    NetworkAddressPlan plan = NetworkAddressPlan::Default();
    const uint32_t max = static_cast<uint32_t>(cfg::kNetworkMaxSlots);

    // Slot 0 is reserved.
    core::Expected<Slot*, NetworkError> zero = Slot::New(0, plan, "/tmp/x");
    MT_EXPECT_TRUE(!zero.ok());
    MT_EXPECT_TRUE(zero.error().kind == NetworkError::Kind::SlotOutOfRange);
    MT_EXPECT_EQ(zero.error().idx, static_cast<uint32_t>(0));
    MT_EXPECT_EQ(zero.error().max, max - 1);

    // The ceiling itself is out of range.
    core::Expected<Slot*, NetworkError> over = Slot::New(max, plan, "/tmp/x");
    MT_EXPECT_TRUE(!over.ok());
    MT_EXPECT_TRUE(over.error().kind == NetworkError::Kind::SlotOutOfRange);

    // The message reproduces the Rust `#[error(...)]` text.
    MT_EXPECT_TRUE(over.error().Message().find("Slot index out of range") == 0);
}

// ============================================================
// Slot::host_veth_name / namespace_path
// ============================================================
MT_TEST(slot_names_use_the_rust_prefixes) {
    MT_EXPECT_EQ(Slot::HostVethName(7), std::string("veth-7"));
    MT_EXPECT_EQ(Slot::HostVethName(0), std::string("veth-0"));

    NetworkAddressPlan plan = NetworkAddressPlan::Default();
    core::Expected<Slot*, NetworkError> slot = Slot::New(3, plan, "/run/aenv/netns");
    MT_EXPECT_TRUE(slot.ok());

    // The namespace id must carry the prefix `PrepareRuntime` scans for,
    // otherwise stale namespaces would never be reclaimed.
    MT_EXPECT_TRUE(slot.value()->NamespaceId().find(kNetnsPrefix) == 0);
    // The path is the id joined onto the configured directory.
    MT_EXPECT_EQ(slot.value()->NamespacePath(),
                 std::string("/run/aenv/netns/") + slot.value()->NamespaceId());
    delete slot.value();
}

MT_TEST(slot_namespace_ids_are_unique_per_slot) {
    NetworkAddressPlan plan = NetworkAddressPlan::Default();
    core::Expected<Slot*, NetworkError> a = Slot::New(1, plan, "/tmp/x");
    core::Expected<Slot*, NetworkError> b = Slot::New(1, plan, "/tmp/x");
    MT_EXPECT_TRUE(a.ok() && b.ok());
    // Same index, but a fresh namespace each time: reusing an id would make
    // two slots bind-mount over each other.
    MT_EXPECT_TRUE(a.value()->NamespaceId() != b.value()->NamespaceId());
    delete a.value();
    delete b.value();
}

// ============================================================
// slot.rs :: custom_address_plan_calculates_slot_ips_and_boot_arg
// ============================================================
MT_TEST(build_ip_boot_arg_matches_rust_layout) {
    NetworkAddressPlan plan = NetworkAddressPlan::Default();
    core::Expected<Slot*, NetworkError> slot = Slot::New(1, plan, "/tmp/x");
    MT_EXPECT_TRUE(slot.ok());

    const std::string arg = slot.value()->BuildIpBootArg();

    // Rust format: ip=<vm_ip>::<tap_ip>:<mask>:instance:eth0:off:<dns>
    MT_EXPECT_TRUE(arg.find("ip=") == 0);
    MT_EXPECT_TRUE(arg.find(Ipv4ToString(plan.VmIp())) != std::string::npos);
    MT_EXPECT_TRUE(arg.find(Ipv4ToString(plan.TapIp())) != std::string::npos);
    // The mask must be dotted, not a prefix length — the guest's ip= parser
    // rejects a prefix here.
    MT_EXPECT_TRUE(arg.find(Ipv4ToString(plan.VmLinkMask())) != std::string::npos);
    MT_EXPECT_TRUE(arg.find(":instance:eth0:off:") != std::string::npos);

    delete slot.value();
}

MT_TEST(vm_link_mask_is_dotted_form_of_the_prefix) {
    // /24 -> 255.255.255.0
    Ipv4Cidr host = Ipv4Cidr::Parse("10.0.0.0/16").value();
    Ipv4Cidr veth = Ipv4Cidr::Parse("10.1.0.0/16").value();
    Ipv4Cidr vm24 = Ipv4Cidr::Parse("192.168.1.0/24").value();
    NetworkAddressPlan p24(host, veth, vm24);
    MT_EXPECT_EQ(Ipv4ToString(p24.VmLinkMask()), std::string("255.255.255.0"));

    // /30 -> 255.255.255.252
    Ipv4Cidr vm30 = Ipv4Cidr::Parse("192.168.1.0/30").value();
    NetworkAddressPlan p30(host, veth, vm30);
    MT_EXPECT_EQ(Ipv4ToString(p30.VmLinkMask()), std::string("255.255.255.252"));
}

// ============================================================
// slot.rs :: configure_namespace_iptables_rules
// ============================================================
MT_TEST(namespace_iptables_rules_match_rust_rule_set) {
    const uint32_t host_ip = Ip("10.0.0.5");
    const uint32_t veth_vm = Ip("10.1.0.3");
    const uint32_t vm_ip   = Ip("192.168.1.1");

    std::vector<IptablesRestoreCommand> rules =
        BuildNamespaceIptablesRules(host_ip, veth_vm, vm_ip);

    // Rust installs exactly five rules.
    MT_EXPECT_EQ(rules.size(), static_cast<size_t>(5));

    // 1. FORWARD out from the guest.
    MT_EXPECT_EQ(rules[0].table, std::string("filter"));
    MT_EXPECT_EQ(rules[0].chain, std::string("FORWARD"));
    MT_EXPECT_EQ(rules[0].rule, std::string("-i tap0 -o vpeer -j ACCEPT"));

    // 2. FORWARD back, restricted to established flows.
    MT_EXPECT_EQ(rules[1].table, std::string("filter"));
    MT_EXPECT_TRUE(rules[1].rule.find("RELATED,ESTABLISHED") != std::string::npos);

    // 3. SNAT for the guest address.
    MT_EXPECT_EQ(rules[2].table, std::string("nat"));
    MT_EXPECT_EQ(rules[2].chain, std::string("POSTROUTING"));
    MT_EXPECT_EQ(rules[2].rule,
                 std::string("-o vpeer -s 192.168.1.1 -j SNAT --to 10.0.0.5"));

    // 4. SNAT for the namespace-local egress proxy, which dials from vpeer.
    MT_EXPECT_EQ(rules[3].table, std::string("nat"));
    MT_EXPECT_EQ(rules[3].rule,
                 std::string("-o vpeer -s 10.1.0.3 -j SNAT --to 10.0.0.5"));

    // 5. DNAT so the host can reach the guest by the slot IP.
    MT_EXPECT_EQ(rules[4].table, std::string("nat"));
    MT_EXPECT_EQ(rules[4].chain, std::string("PREROUTING"));
    MT_EXPECT_EQ(rules[4].rule,
                 std::string("-i vpeer -d 10.0.0.5 -j DNAT --to 192.168.1.1"));

    // All `nat` rules must be contiguous: BuildRestoreScript emits one
    // *table/COMMIT block per run, so interleaving would drop rules.
    MT_EXPECT_EQ(rules[2].table, rules[3].table);
    MT_EXPECT_EQ(rules[3].table, rules[4].table);
}

// ============================================================
// slot.rs :: parse_nameserver_ipv4_*
// ============================================================
MT_TEST(parse_nameserver_prefers_non_loopback_ipv4) {
    // Rust: a stub resolver on loopback is unreachable from the guest
    // namespace, so it must be skipped in favour of the next entry.
    const std::string contents =
        "nameserver 127.0.0.53\n"
        "nameserver 10.0.0.1\n";
    uint32_t ip = 0;
    MT_EXPECT_TRUE(ParseNameserverIpv4(contents, &ip));
    MT_EXPECT_EQ(Ipv4ToString(ip), std::string("10.0.0.1"));
}

MT_TEST(parse_nameserver_ignores_non_ipv4_entries) {
    const std::string contents =
        "nameserver fe80::1\n"
        "nameserver 2001:db8::1\n"
        "nameserver 192.168.0.1\n";
    uint32_t ip = 0;
    MT_EXPECT_TRUE(ParseNameserverIpv4(contents, &ip));
    MT_EXPECT_EQ(Ipv4ToString(ip), std::string("192.168.0.1"));
}

MT_TEST(parse_nameserver_accepts_link_local_dns) {
    // Link-local is not loopback, so Rust accepts it.
    const std::string contents = "nameserver 169.254.1.1\n";
    uint32_t ip = 0;
    MT_EXPECT_TRUE(ParseNameserverIpv4(contents, &ip));
    MT_EXPECT_EQ(Ipv4ToString(ip), std::string("169.254.1.1"));
}

MT_TEST(parse_nameserver_skips_malformed_and_commented_lines) {
    const std::string contents =
        "# nameserver 1.1.1.1\n"
        "\n"
        "   \n"
        "nameserver\n"
        "nameserver not-an-ip\n"
        "options edns0\n"
        "search example.com\n"
        "  nameserver 8.8.4.4  \n";
    uint32_t ip = 0;
    MT_EXPECT_TRUE(ParseNameserverIpv4(contents, &ip));
    MT_EXPECT_EQ(Ipv4ToString(ip), std::string("8.8.4.4"));
}

MT_TEST(parse_nameserver_reports_absence) {
    uint32_t ip = 0;
    // No nameserver at all.
    MT_EXPECT_TRUE(!ParseNameserverIpv4("search example.com\n", &ip));
    // Only unusable ones.
    MT_EXPECT_TRUE(!ParseNameserverIpv4("nameserver 127.0.0.1\n", &ip));
    MT_EXPECT_TRUE(!ParseNameserverIpv4("nameserver 0.0.0.0\n", &ip));
    MT_EXPECT_TRUE(!ParseNameserverIpv4("", &ip));
}

MT_TEST(resolve_guest_dns_server_always_returns_a_usable_address) {
    // Whatever the host's resolv.conf says, the result must never be
    // loopback or unspecified — Rust falls back to 8.8.8.8 for that reason.
    const uint32_t ip = ResolveGuestDnsServer();
    MT_EXPECT_TRUE(ip != 0);
    MT_EXPECT_TRUE((ip >> 24) != 127u);
}

// ============================================================
// NetworkError messages
// ============================================================
MT_TEST(network_error_messages_match_rust_format_strings) {
    MT_EXPECT_EQ(NetworkError::Namespace("boom").Message(),
                 std::string("Namespace operation failed: boom"));
    MT_EXPECT_EQ(NetworkError::HostIptables("nope").Message(),
                 std::string("Host iptables operation failed: nope"));
    MT_EXPECT_EQ(NetworkError::Io("gone").Message(),
                 std::string("IO error: gone"));
    MT_EXPECT_EQ(NetworkError::SlotOutOfRangeErr(9, 4).Message(),
                 std::string("Slot index out of range (max 4): 9"));
}

MT_MAIN
