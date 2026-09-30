// SPDX-License-Identifier: MIT
// Tests for sandbox::LocalExecutor (real fork/exec) + NetworkAddressPlan.
#include "microtest.h"

#include <map>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/sandbox/process.h"
#include "agentenv/sandbox/network.h"

using namespace agentenv::sandbox;

MT_TEST(local_executor_echo) {
    std::unique_ptr<Executor> ex = MakeLocalExecutor();
    std::vector<std::string> args;
    args.push_back("hello");
    args.push_back("world");
    auto r = ex->RunCommand("/bin/echo", args);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().exit_code, 0);
    MT_EXPECT_TRUE(r.value().stdout_data == "hello world\n");
}

MT_TEST(local_executor_exit_code) {
    std::unique_ptr<Executor> ex = MakeLocalExecutor();
    std::vector<std::string> args;
    args.push_back("-c");
    args.push_back("exit 7");
    auto r = ex->RunCommand("/bin/sh", args);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().exit_code, 7);
}

MT_TEST(local_executor_stderr_capture) {
    std::unique_ptr<Executor> ex = MakeLocalExecutor();
    std::vector<std::string> args;
    args.push_back("-c");
    args.push_back("echo oops 1>&2");
    auto r = ex->RunCommand("/bin/sh", args);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().stderr_data == "oops\n");
    MT_EXPECT_TRUE(r.value().stdout_data.empty());
}

MT_TEST(local_executor_cwd) {
    std::unique_ptr<Executor> ex = MakeLocalExecutor();
    ProcessOpts opts;
    opts.WithCwd("/tmp");
    auto r = ex->RunCommandWithOpts("/bin/pwd", std::vector<std::string>(), opts);
    MT_EXPECT_TRUE(r.ok());
    // /tmp may be a symlink on some systems; accept prefix match.
    MT_EXPECT_TRUE(r.value().stdout_data.find("/tmp") != std::string::npos);
}

MT_TEST(local_executor_env) {
    std::unique_ptr<Executor> ex = MakeLocalExecutor();
    std::vector<std::string> args;
    args.push_back("-c");
    args.push_back("echo $AENV_TEST_VAR");
    ProcessOpts opts;
    std::map<std::string, std::string> envs;
    envs["AENV_TEST_VAR"] = "xyz123";
    opts.WithEnvs(envs);
    auto r = ex->RunCommandWithOpts("/bin/sh", args, opts);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().stdout_data == "xyz123\n");
}

MT_TEST(local_executor_timeout) {
    std::unique_ptr<Executor> ex = MakeLocalExecutor();
    std::vector<std::string> args;
    args.push_back("10");
    ProcessOpts opts;
    opts.WithTimeoutMs(1000);
    auto r = ex->RunCommandWithOpts("/bin/sleep", args, opts);
    MT_EXPECT_TRUE(!r.ok());  // timed out -> error
}

MT_TEST(local_executor_empty_command) {
    std::unique_ptr<Executor> ex = MakeLocalExecutor();
    auto r = ex->RunCommand("", std::vector<std::string>());
    MT_EXPECT_TRUE(!r.ok());
}

MT_TEST(local_executor_create_dir_all) {
    std::unique_ptr<Executor> ex = MakeLocalExecutor();
    const std::string dir = "/tmp/agentenv-exec-mkdir/nested/deep";
    MT_EXPECT_TRUE(ex->CreateDirAll(dir).ok());
    // An already-existing directory is not an error.
    MT_EXPECT_TRUE(ex->CreateDirAll(dir).ok());
    agentenv::core::fs::RemoveDirAll("/tmp/agentenv-exec-mkdir");
}

// ---- NetworkAddressPlan (address_plan.rs) ----
MT_TEST(cidr_parse_and_normalize) {
    auto c = network::Ipv4Cidr::Parse("10.0.5.7/16");
    MT_EXPECT_TRUE(c.ok());
    // host bits masked off -> 10.0.0.0
    MT_EXPECT_TRUE(network::Ipv4ToString(c.value().network) == "10.0.0.0");
    MT_EXPECT_EQ(static_cast<int>(c.value().prefix), 16);
    MT_EXPECT_TRUE(static_cast<unsigned long long>(c.value().Size()) == 65536ULL);
}

MT_TEST(cidr_parse_bad) {
    MT_EXPECT_TRUE(!network::Ipv4Cidr::Parse("nope").ok());
    MT_EXPECT_TRUE(!network::Ipv4Cidr::Parse("10.0.0.0/40").ok());
    MT_EXPECT_TRUE(!network::Ipv4Cidr::Parse("999.0.0.0/8").ok());
}

MT_TEST(address_plan_slot_ips) {
    network::NetworkAddressPlan plan = network::NetworkAddressPlan::Default();
    uint32_t hi = 0, vh = 0, vv = 0;
    auto r0 = plan.SlotIps(0, &hi, &vh, &vv);
    MT_EXPECT_TRUE(r0.ok());
    // Defaults come from Rust `NetworkInternalConfig` (10.11/10.12), not from
    // an arbitrary 10.0/10.1 pair.
    MT_EXPECT_TRUE(network::Ipv4ToString(hi) == "10.11.0.0");
    MT_EXPECT_TRUE(network::Ipv4ToString(vh) == "10.12.0.0");
    MT_EXPECT_TRUE(network::Ipv4ToString(vv) == "10.12.0.1");

    auto r5 = plan.SlotIps(5, &hi, &vh, &vv);
    MT_EXPECT_TRUE(r5.ok());
    MT_EXPECT_TRUE(network::Ipv4ToString(hi) == "10.11.0.5");
    // veth offset = 5*2 = 10, 11
    MT_EXPECT_TRUE(network::Ipv4ToString(vh) == "10.12.0.10");
    MT_EXPECT_TRUE(network::Ipv4ToString(vv) == "10.12.0.11");
}

MT_TEST(address_plan_vm_tap_ip) {
    network::NetworkAddressPlan plan = network::NetworkAddressPlan::Default();
    // FIXED_NETWORK_VM_LINK_CIDR is 169.254.0.20/30, so vm=+1 and tap=+2 land
    // on .21/.22. These two values are part of the snapshot ABI (they are
    // baked into the guest `ip=` boot argument), so they must not drift.
    MT_EXPECT_TRUE(network::Ipv4ToString(plan.VmIp()) == "169.254.0.21");
    MT_EXPECT_TRUE(network::Ipv4ToString(plan.TapIp()) == "169.254.0.22");
    // /30 -> 255.255.255.252
    MT_EXPECT_TRUE(network::Ipv4ToString(plan.VmLinkMask()) == "255.255.255.252");
    MT_EXPECT_TRUE(plan.VmLinkPrefix() == 30);
}

MT_TEST(address_plan_denied_cidrs) {
    network::NetworkAddressPlan plan = network::NetworkAddressPlan::Default();
    std::vector<std::string> denied = plan.InternalEgressDeniedCidrs();
    MT_EXPECT_EQ(static_cast<int>(denied.size()), 3);
    MT_EXPECT_TRUE(denied[0] == "10.11.0.0/16");
    MT_EXPECT_TRUE(denied[1] == "10.12.0.0/16");
    MT_EXPECT_TRUE(denied[2] == "169.254.0.20/30");
}

MT_MAIN
