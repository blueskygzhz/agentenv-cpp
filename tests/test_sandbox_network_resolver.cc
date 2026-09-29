// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/resolver.rs `mod tests` +
//       src/sandbox/network/address_plan.rs.
#include "agentenv/sandbox/network/resolver.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/cfg/network.h"
#include "agentenv/sandbox/network.h"
#include "microtest.h"

using namespace agentenv;                  // NOLINT
using namespace agentenv::sandbox::network;  // NOLINT

// ---- resolver.rs constants must not drift: they bound the proxy's blast radius ----
MT_TEST(resolver_constants_match_rust) {
    MT_EXPECT_TRUE(kResolverQueueCapacity == 128);
    MT_EXPECT_TRUE(kResolverMaxInFlight == 16);
    MT_EXPECT_TRUE(kResolverStopPollMs == 100);
    MT_EXPECT_TRUE(kResolverDnsTimeoutMs == 5000);
    MT_EXPECT_TRUE(kResolverResponseTimeoutMs == 30000);
}

// ---- Rust test: shutdown_is_idempotent_and_releases_waiters ----
MT_TEST(resolver_shutdown_is_idempotent_and_releases_waiters) {
    HostNetResolver resolver;

    std::atomic<bool> lookup_returned(false);
    std::thread lookup([&resolver, &lookup_returned]() {
        std::vector<ResolvedAddr> out;
        // An invalid TLD: this either fails fast or is still in libc when the
        // shutdown lands. Either way the call must return, not hang.
        resolver.Resolve("resolver-shutdown.invalid", 443, NULL, &out);
        lookup_returned.store(true);
    });

    resolver.Shutdown();
    resolver.Shutdown();  // idempotent

    // The waiter must be released well inside the 30s response deadline.
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!lookup_returned.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MT_EXPECT_TRUE(lookup_returned.load());
    lookup.join();

    // Rust: after shutdown every resolve is None.
    std::vector<ResolvedAddr> out;
    MT_EXPECT_TRUE(!resolver.Resolve("example.com", 443, NULL, &out));
}

MT_TEST(resolver_resolve_after_shutdown_is_none) {
    HostNetResolver resolver;
    resolver.Shutdown();
    std::vector<ResolvedAddr> out;
    MT_EXPECT_TRUE(!resolver.Resolve("localhost", 80, NULL, &out));
    MT_EXPECT_TRUE(out.empty());
}

// ---- cancellation is observed on the stop-poll cadence, not the deadline ----
MT_TEST(resolver_honors_cancel_flag) {
    HostNetResolver resolver;
    std::atomic<bool> cancel(true);  // already cancelled
    std::vector<ResolvedAddr> out;

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const bool ok = resolver.Resolve("cancel-me.invalid", 443, &cancel, &out);
    const std::chrono::milliseconds elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

    MT_EXPECT_TRUE(!ok);
    // Must not ride out the 30s response timeout.
    MT_EXPECT_TRUE(elapsed < std::chrono::milliseconds(2000));
    resolver.Shutdown();
}

// ---- a successful IPv4 lookup carries the requested port through ----
MT_TEST(resolver_resolves_localhost_with_port) {
    HostNetResolver resolver;
    if (!resolver.WorkerReady()) {
        // Entering the host netns needs privileges; skip rather than fail on a
        // restricted builder.
        resolver.Shutdown();
        return;
    }
    std::vector<ResolvedAddr> out;
    if (resolver.Resolve("localhost", 8080, NULL, &out)) {
        for (size_t i = 0; i < out.size(); ++i) {
            // Rust forces Ipv4Only and stamps the caller's port on every entry.
            MT_EXPECT_TRUE(out[i].port == 8080);
        }
    }
    resolver.Shutdown();
}

MT_TEST(host_ns_fd_is_cached) {
    // Rust `OnceLock` — repeated calls return the same fd, so a later
    // unshare on this thread cannot change what we resolve against.
    const int a = HostNsFd();
    const int b = HostNsFd();
    MT_EXPECT_TRUE(a == b);
}

// ---- address_plan.rs: from_config ----
MT_TEST(address_plan_from_default_config_matches_default) {
    cfg::NetworkConfig config;
    auto plan = cfg::NetworkAddressPlanFromConfig(config);
    MT_EXPECT_TRUE(plan.ok());

    NetworkAddressPlan expected = NetworkAddressPlan::Default();
    // Rust's `#[cfg(test)] default()` is literally from_config(&default()).
    MT_EXPECT_TRUE(plan.value().HostInteractionCidr().network ==
                   expected.HostInteractionCidr().network);
    MT_EXPECT_TRUE(plan.value().HostInteractionCidr().prefix ==
                   expected.HostInteractionCidr().prefix);
    MT_EXPECT_TRUE(plan.value().VmIp() == expected.VmIp());
    MT_EXPECT_TRUE(plan.value().TapIp() == expected.TapIp());
    MT_EXPECT_TRUE(plan.value().VmLinkPrefix() == expected.VmLinkPrefix());
}

MT_TEST(address_plan_from_config_honors_custom_pools) {
    cfg::NetworkConfig config;
    config.internal.host_interaction_cidr = "10.21.0.0/16";
    config.internal.veth_cidr = "10.22.0.0/16";

    auto plan = cfg::NetworkAddressPlanFromConfig(config);
    MT_EXPECT_TRUE(plan.ok());
    MT_EXPECT_TRUE(plan.value().HostInteractionCidr().ToString() == "10.21.0.0/16");

    // slot_ips derives from the configured pools: veth offset is idx*2.
    uint32_t host_ip = 0, veth_host = 0, veth_vm = 0;
    auto ips = plan.value().SlotIps(3, &host_ip, &veth_host, &veth_vm);
    MT_EXPECT_TRUE(ips.ok());
    MT_EXPECT_TRUE(Ipv4ToString(host_ip) == "10.21.0.3");
    MT_EXPECT_TRUE(Ipv4ToString(veth_host) == "10.22.0.6");
    MT_EXPECT_TRUE(Ipv4ToString(veth_vm) == "10.22.0.7");

    // Rust `internal_egress_denied_cidrs` rejects the whole pools so one
    // sandbox cannot reach another's namespace addresses.
    std::vector<std::string> denied = plan.value().InternalEgressDeniedCidrs();
    MT_EXPECT_EQ(static_cast<int>(denied.size()), 3);
    MT_EXPECT_TRUE(denied[0] == "10.21.0.0/16");
    MT_EXPECT_TRUE(denied[1] == "10.22.0.0/16");
}

MT_TEST(address_plan_from_config_rejects_invalid_pools) {
    cfg::NetworkConfig config;
    config.internal.host_interaction_cidr = "not-a-cidr";
    auto plan = cfg::NetworkAddressPlanFromConfig(config);
    MT_EXPECT_TRUE(!plan.ok());
}

MT_TEST(address_plan_conflict_patterns_follow_prefix_width) {
    // Rust `network_conflict_pattern`: /0-8 -> one octet, /9-16 -> two,
    // else three.
    Ipv4Cidr eight = Ipv4Cidr::Parse("10.0.0.0/8").value();
    Ipv4Cidr sixteen = Ipv4Cidr::Parse("10.12.0.0/16").value();
    Ipv4Cidr twentyfour = Ipv4Cidr::Parse("10.12.13.0/24").value();
    NetworkAddressPlan plan(eight, sixteen, twentyfour);

    std::vector<std::string> patterns = plan.ConflictPatterns();
    MT_EXPECT_EQ(static_cast<int>(patterns.size()), 3);
    MT_EXPECT_TRUE(patterns[0] == "10.");
    MT_EXPECT_TRUE(patterns[1] == "10.12.");
    MT_EXPECT_TRUE(patterns[2] == "10.12.13.");
}

MT_TEST(address_plan_slot_ips_rejects_out_of_range_offsets) {
    // A /30 holds 4 addresses, so idx 4 is outside it.
    Ipv4Cidr small = Ipv4Cidr::Parse("192.168.5.0/30").value();
    NetworkAddressPlan plan(small, small, small);
    uint32_t a = 0, b = 0, c = 0;
    MT_EXPECT_TRUE(plan.SlotIps(0, &a, &b, &c).ok());
    MT_EXPECT_TRUE(!plan.SlotIps(4, &a, &b, &c).ok());
    // idx 2 needs veth offset 4+5, also outside.
    MT_EXPECT_TRUE(!plan.SlotIps(2, &a, &b, &c).ok());
}

MT_MAIN
