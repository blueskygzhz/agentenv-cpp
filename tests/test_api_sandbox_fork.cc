// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs :: sandboxes_sandbox_id_fork_post
#include "microtest.h"

#include <memory>
#include <string>

#include "agentenv/api/impls.h"
#include "orchestrator_fakes.h"

using namespace agentenv;
using namespace agentenv::api;
using agentenv::orchestrator::SandboxMetadata;
using agentenv::orchestrator::SandboxState;
using agentenv::testing::FakeSandbox;
using agentenv::testing::Fixture;
using agentenv::testing::NowMs;

namespace {

/// A Running sandbox plus its handle, as the fork path expects to find.
SandboxMetadata RegisterRunning(Fixture& f) {
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = SandboxState::Running;
    m.resources.cpu_count  = 2;
    m.resources.memory_mib = 512;
    m.created_at_ms = NowMs();
    m.timeout_ms = core::Optional<int64_t>(60 * 1000);
    MT_EXPECT_TRUE(f.store->Add(m).ok());
    std::unique_ptr<sandbox::SandboxBackend> backend(new FakeSandbox(&f.script));
    f.orch->RegisterSandboxHandle(
        m.id, orchestrator::SandboxHandlePtr(
                  new orchestrator::SandboxHandle(std::move(backend))));
    f.orch->UpsertProxyRoute(m.id, orchestrator::ProxyTarget("10.0.0.1"));
    return m;
}

}  // namespace

MT_TEST(fork_handler_defaults_to_one_child_with_no_body) {
    Fixture f;
    f.script.succeed_fork = true;
    SandboxMetadata source = RegisterRunning(f);

    // Rust's body is `Option<SandboxForkRequest>`, and an absent `count`
    // defaults to 1 rather than 0 -- a fork of nothing is not a meaningful
    // request, so the default has to be a usable one.
    ForkSandboxReq req;
    ApiImpl impl(f.orch);
    const core::Expected<ForkSandboxResp, ApiError> r =
        impl.ForkSandbox(source.id.ToString(), req);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(r.value().results.size()), 1);
    MT_EXPECT_TRUE(r.value().results[0].sandbox.has_value());
    MT_EXPECT_TRUE(!r.value().results[0].error.has_value());
}

MT_TEST(fork_handler_reports_one_entry_per_child) {
    Fixture f;
    f.script.succeed_fork = true;
    SandboxMetadata source = RegisterRunning(f);

    ForkSandboxReq req;
    req.count = core::Optional<uint32_t>(3);
    ApiImpl impl(f.orch);
    const core::Expected<ForkSandboxResp, ApiError> r =
        impl.ForkSandbox(source.id.ToString(), req);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(r.value().results.size()), 3);
    for (std::size_t i = 0; i < r.value().results.size(); ++i) {
        // Exactly one side is set per entry. A caller distinguishes success
        // from failure by which one is present, so both-or-neither would be
        // unreadable.
        MT_EXPECT_TRUE(r.value().results[i].sandbox.has_value() !=
                       r.value().results[i].error.has_value());
    }
}

MT_TEST(fork_handler_returns_201_with_failures_in_the_body) {
    Fixture f;
    f.script.succeed_fork = true;
    f.script.fork_child_outcomes.push_back(true);
    f.script.fork_child_outcomes.push_back(false);
    SandboxMetadata source = RegisterRunning(f);

    ForkSandboxReq req;
    req.count = core::Optional<uint32_t>(2);
    ApiImpl impl(f.orch);
    const core::Expected<ForkSandboxResp, ApiError> r =
        impl.ForkSandbox(source.id.ToString(), req);

    // The call succeeds even though a child did not. The children are
    // independent, so a single status could not express "one of two booted" --
    // and failing the request would discard the one that worked.
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(r.value().results.size()), 2);

    int ok_count = 0, err_count = 0;
    for (std::size_t i = 0; i < r.value().results.size(); ++i) {
        if (r.value().results[i].sandbox.has_value()) ++ok_count;
        if (r.value().results[i].error.has_value()) ++err_count;
    }
    MT_EXPECT_EQ(ok_count, 1);
    MT_EXPECT_EQ(err_count, 1);
}

MT_TEST(fork_handler_rejects_a_count_outside_one_to_hundred) {
    Fixture f;
    f.script.succeed_fork = true;
    SandboxMetadata source = RegisterRunning(f);
    ApiImpl impl(f.orch);

    // Rust validates `(1..=100).contains(&count)`. Both ends are checked
    // because the failures differ in kind: 0 asks for nothing, and an
    // unbounded count would capture once and then try to boot that many VMs.
    ForkSandboxReq zero;
    zero.count = core::Optional<uint32_t>(0);
    const core::Expected<ForkSandboxResp, ApiError> r0 =
        impl.ForkSandbox(source.id.ToString(), zero);
    MT_EXPECT_TRUE(!r0.ok());
    if (!r0.ok()) {
        MT_EXPECT_EQ(r0.error().code, 400);
        MT_EXPECT_EQ(r0.error().message,
                     std::string("count must be between 1 and 100"));
    }

    ForkSandboxReq too_many;
    too_many.count = core::Optional<uint32_t>(101);
    const core::Expected<ForkSandboxResp, ApiError> r1 =
        impl.ForkSandbox(source.id.ToString(), too_many);
    MT_EXPECT_TRUE(!r1.ok());
    if (!r1.ok()) MT_EXPECT_EQ(r1.error().code, 400);

    // The boundaries themselves are accepted.
    ForkSandboxReq one;
    one.count = core::Optional<uint32_t>(1);
    MT_EXPECT_TRUE(impl.ForkSandbox(source.id.ToString(), one).ok());
}

MT_TEST(fork_handler_404s_before_validating_the_body) {
    Fixture f;
    ApiImpl impl(f.orch);

    // A bad count against a sandbox that does not exist is a 404, not a 400.
    // Rust resolves the sandbox first, and the ordering is what a client needs
    // to tell "fix your request" from "that machine is gone".
    ForkSandboxReq bad;
    bad.count = core::Optional<uint32_t>(0);
    const core::Expected<ForkSandboxResp, ApiError> r =
        impl.ForkSandbox(core::SandboxId::Fresh().ToString(), bad);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) MT_EXPECT_EQ(r.error().code, 404);

    // An unparseable id is the same 404 rather than a 400.
    const core::Expected<ForkSandboxResp, ApiError> bad_id =
        impl.ForkSandbox("not-a-uuid", bad);
    MT_EXPECT_TRUE(!bad_id.ok());
    if (!bad_id.ok()) MT_EXPECT_EQ(bad_id.error().code, 404);
}

MT_TEST(fork_handler_409s_when_the_source_is_not_running) {
    Fixture f;
    f.script.succeed_fork = true;
    SandboxMetadata source = RegisterRunning(f);
    // Paused, so the Running -> Forking claim fails with a state conflict.
    source.state = SandboxState::Paused;
    MT_EXPECT_TRUE(f.store->Update(source).ok());

    ForkSandboxReq req;
    ApiImpl impl(f.orch);
    const core::Expected<ForkSandboxResp, ApiError> r =
        impl.ForkSandbox(source.id.ToString(), req);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        // 409, not the 400 the generic error mapping assigns
        // `InvalidSandboxState`. A paused sandbox becomes forkable once
        // resumed, and 400 would tell the client to stop retrying. Rust makes
        // this exception for this handler alone, with this wording.
        MT_EXPECT_EQ(r.error().code, 409);
        MT_EXPECT_EQ(r.error().message,
                     std::string("sandbox cannot be forked from paused state"));
    }
}

MT_TEST(fork_handler_refuses_a_source_with_volume_mounts) {
    Fixture f;
    f.script.succeed_fork = true;
    SandboxMetadata source = RegisterRunning(f);
    source.volume_mounts["/data"] = "vol-a";
    MT_EXPECT_TRUE(f.store->Update(source).ok());

    ForkSandboxReq req;
    ApiImpl impl(f.orch);
    const core::Expected<ForkSandboxResp, ApiError> r =
        impl.ForkSandbox(source.id.ToString(), req);

    // Refused, not forked without the volumes. Rust clones each exclusive
    // volume per child so the parent keeps sole ownership of its own; with no
    // VolumeManager here there is nothing to clone with, and proceeding would
    // give parent and child the same volume id -- two live VMs writing one
    // image. 501 says the build cannot do it, which is distinct from the 400
    // a malformed request gets.
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        MT_EXPECT_EQ(r.error().code, 501);
        MT_EXPECT_TRUE(r.error().message.find("volume mounts") != std::string::npos);
    }
    // The backend was never asked to fork, so the source was never paused.
    MT_EXPECT_EQ(f.script.fork_calls, 0);
}

MT_TEST(fork_handler_converts_the_timeout_from_seconds) {
    Fixture f;
    f.script.succeed_fork = true;
    SandboxMetadata source = RegisterRunning(f);

    ForkSandboxReq req;
    req.timeout_secs = core::Optional<uint32_t>(30);
    ApiImpl impl(f.orch);
    const core::Expected<ForkSandboxResp, ApiError> r =
        impl.ForkSandbox(source.id.ToString(), req);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_TRUE(r.value().results[0].sandbox.has_value());

    // The wire field is seconds and the metadata is milliseconds. Passing the
    // number through unscaled would give a 30-second request a 30-millisecond
    // timeout -- the sandbox would be reaped before it answered anything.
    const core::Expected<core::Optional<SandboxMetadata>,
                         orchestrator::StoreError> child =
        f.store->Get(core::SandboxId::Parse(
                         r.value().results[0].sandbox->sandbox_id).value());
    MT_EXPECT_TRUE(child.ok() && child.value());
    if (child.ok() && child.value()) {
        MT_EXPECT_TRUE(child.value()->timeout_ms.has_value());
        if (child.value()->timeout_ms.has_value()) {
            MT_EXPECT_EQ(*child.value()->timeout_ms, static_cast<int64_t>(30000));
        }
    }
}

MT_TEST(fork_handler_without_a_timeout_inherits_the_source) {
    Fixture f;
    f.script.succeed_fork = true;
    SandboxMetadata source = RegisterRunning(f);  // timeout_ms = 60000

    // An absent timeout is `UseExisting`, not "no timeout". Clearing it would
    // leave the child running until something else reaped it.
    ForkSandboxReq req;
    ApiImpl impl(f.orch);
    const core::Expected<ForkSandboxResp, ApiError> r =
        impl.ForkSandbox(source.id.ToString(), req);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;

    const core::Expected<core::Optional<SandboxMetadata>,
                         orchestrator::StoreError> child =
        f.store->Get(core::SandboxId::Parse(
                         r.value().results[0].sandbox->sandbox_id).value());
    MT_EXPECT_TRUE(child.ok() && child.value());
    if (child.ok() && child.value() && child.value()->timeout_ms.has_value()) {
        MT_EXPECT_EQ(*child.value()->timeout_ms, static_cast<int64_t>(60 * 1000));
    }
}

MT_TEST(fork_request_round_trips_and_omits_absent_fields) {
    ForkSandboxReq empty;
    const std::string json = empty.ToJson().ToString();
    // Omitted rather than written as 0: Rust has `skip_serializing_if` on
    // both, and 0 means something different for each -- an invalid count, and
    // "expire immediately" rather than "inherit the source's timeout".
    MT_EXPECT_TRUE(json.find("count") == std::string::npos);
    MT_EXPECT_TRUE(json.find("timeout") == std::string::npos);

    ForkSandboxReq full;
    full.count = core::Optional<uint32_t>(4);
    full.timeout_secs = core::Optional<uint32_t>(30);
    const core::Expected<core::Json, core::AnyError> parsed =
        core::Json::Parse(full.ToJson().ToString());
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    const core::Expected<ForkSandboxReq, core::AnyError> back =
        ForkSandboxReq::FromJson(parsed.value());
    MT_EXPECT_TRUE(back.ok());
    if (!back.ok()) return;
    MT_EXPECT_TRUE(back.value().count.has_value());
    MT_EXPECT_EQ(static_cast<int>(*back.value().count), 4);
    MT_EXPECT_TRUE(back.value().timeout_secs.has_value());
    MT_EXPECT_EQ(static_cast<int>(*back.value().timeout_secs), 30);

    // An empty object parses into both-absent rather than failing: the body
    // itself is optional upstream.
    const core::Expected<core::Json, core::AnyError> none = core::Json::Parse("{}");
    MT_EXPECT_TRUE(none.ok());
    if (!none.ok()) return;
    const core::Expected<ForkSandboxReq, core::AnyError> decoded =
        ForkSandboxReq::FromJson(none.value());
    MT_EXPECT_TRUE(decoded.ok());
    if (decoded.ok()) {
        MT_EXPECT_TRUE(!decoded.value().count.has_value());
        MT_EXPECT_TRUE(!decoded.value().timeout_secs.has_value());
    }
}

MT_TEST(fork_result_entry_writes_only_the_side_that_is_set) {
    ForkSandboxResultEntry ok_entry;
    GetSandboxResp sb;
    sb.sandbox_id = "sb-1";
    sb.state = "running";
    ok_entry.sandbox = core::Optional<GetSandboxResp>(sb);
    const std::string ok_json = ok_entry.ToJson().ToString();
    MT_EXPECT_TRUE(ok_json.find("sandbox") != std::string::npos);
    // Absent, not null. A caller testing for the presence of `error` would
    // read a null as "an error is present but empty".
    MT_EXPECT_TRUE(ok_json.find("error") == std::string::npos);

    ForkSandboxResultEntry err_entry;
    err_entry.error = core::Optional<ApiError>(ApiError::Make(500, "boom"));
    const std::string err_json = err_entry.ToJson().ToString();
    MT_EXPECT_TRUE(err_json.find("error") != std::string::npos);
    MT_EXPECT_TRUE(err_json.find("\"sandbox\"") == std::string::npos);
}

MT_TEST(fork_response_is_a_bare_array) {
    ForkSandboxResp resp;
    ForkSandboxResultEntry e;
    GetSandboxResp sb;
    sb.sandbox_id = "sb-1";
    e.sandbox = core::Optional<GetSandboxResp>(sb);
    resp.results.push_back(e);

    // Rust's response body is `Vec<SandboxForkResult>`, so the top level is an
    // array rather than an object wrapping one.
    const std::string json = resp.ToJson().ToString();
    MT_EXPECT_TRUE(!json.empty());
    MT_EXPECT_EQ(json[0], '[');
}

MT_MAIN
