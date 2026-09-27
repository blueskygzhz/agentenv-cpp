// SPDX-License-Identifier: MIT
// Tests for sandbox::EnvdInstance::WaitForReady + custom_extension::HookGuard.
#include "microtest.h"

#include <ctime>

#include "agentenv/sandbox/envd.h"
#include "agentenv/sandbox/custom_extension.h"

using namespace agentenv::sandbox;

static int64_t mono_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

MT_TEST(envd_instance_addresses) {
    EnvdInstance e("http://10.0.0.5:50051");
    MT_EXPECT_TRUE(e.BasePath() == "http://10.0.0.5:50051");
    MT_EXPECT_TRUE(e.GrpcAddress() == "http://10.0.0.5:50051");
}

MT_TEST(envd_ready_immediately) {
    EnvdInstance e("http://x");
    int probes = 0;
    auto r = e.WaitForReady(2000, 10, [&](int32_t) { ++probes; return true; });
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(probes, 1);
}

MT_TEST(envd_ready_after_n_probes) {
    EnvdInstance e("http://x");
    int probes = 0;
    auto r = e.WaitForReady(5000, 5, [&](int32_t) { ++probes; return probes >= 3; });
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(probes, 3);
}

MT_TEST(envd_deadline_bounds_hung_probe) {
    // A probe that "hangs" for its whole timeout (simulated by sleeping the
    // probe_timeout) must still let WaitForReady return by the deadline.
    EnvdInstance e("http://x");
    const int64_t start = mono_ms();
    auto r = e.WaitForReady(/*timeout*/ 50, /*retry*/ 1, [](int32_t probe_timeout_ms) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = static_cast<long>(probe_timeout_ms) * 1000000L;
        nanosleep(&ts, nullptr);
        return false;  // never healthy
    });
    const int64_t elapsed = mono_ms() - start;
    MT_EXPECT_TRUE(!r.ok());
    // Rust asserts it finishes well under 500ms.
    MT_EXPECT_TRUE(elapsed < 500);
}

MT_TEST(envd_null_probe_times_out) {
    EnvdInstance e("http://x");
    auto r = e.WaitForReady(20, 1, EnvdInstance::HealthProbe());
    MT_EXPECT_TRUE(!r.ok());
}

// ---- custom_extension ----
namespace {
class RecordingClient : public custom_extension::Client {
 public:
    int stop_calls = 0;
    std::string last_event;
    agentenv::core::Expected<agentenv::core::Unit, std::string>
    Notify(const std::string& event, const custom_extension::Params&) override {
        last_event = event;
        if (event == custom_extension::hook::kStop) ++stop_calls;
        return agentenv::core::Unit{};
    }
};
}  // namespace

MT_TEST(params_is_empty) {
    custom_extension::Params p;
    MT_EXPECT_TRUE(custom_extension::ParamsIsEmpty(p));
    p.json_bytes = "{}";
    MT_EXPECT_TRUE(custom_extension::ParamsIsEmpty(p));
    p.json_bytes = "null";
    MT_EXPECT_TRUE(custom_extension::ParamsIsEmpty(p));
    p.json_bytes = "{\"a\":1}";
    MT_EXPECT_TRUE(!custom_extension::ParamsIsEmpty(p));
}

MT_TEST(instance_id_unique) {
    custom_extension::SandboxInstanceId a = custom_extension::SandboxInstanceId::New();
    custom_extension::SandboxInstanceId b = custom_extension::SandboxInstanceId::New();
    MT_EXPECT_TRUE(a.ToString() != b.ToString());
    MT_EXPECT_TRUE(a.ToString().size() > 0);
}

MT_TEST(hook_guard_fires_stop_on_destruction) {
    RecordingClient client;
    agentenv::core::SandboxId sid = agentenv::core::SandboxId::Fresh();
    {
        custom_extension::HookGuard guard(&client, sid,
                                          custom_extension::SandboxInstanceId::New());
        MT_EXPECT_EQ(client.stop_calls, 0);
    }
    MT_EXPECT_EQ(client.stop_calls, 1);
    MT_EXPECT_TRUE(client.last_event == custom_extension::hook::kStop);
}

MT_TEST(hook_guard_disarm_suppresses_stop) {
    RecordingClient client;
    agentenv::core::SandboxId sid = agentenv::core::SandboxId::Fresh();
    {
        custom_extension::HookGuard guard(&client, sid,
                                          custom_extension::SandboxInstanceId::New());
        guard.Disarm();
    }
    MT_EXPECT_EQ(client.stop_calls, 0);
}

MT_TEST(hook_guard_move_transfers_obligation) {
    RecordingClient client;
    agentenv::core::SandboxId sid = agentenv::core::SandboxId::Fresh();
    {
        custom_extension::HookGuard g1(&client, sid,
                                       custom_extension::SandboxInstanceId::New());
        custom_extension::HookGuard g2(std::move(g1));
        // moved-from g1 must not fire; g2 fires once.
    }
    MT_EXPECT_EQ(client.stop_calls, 1);
}

MT_MAIN
