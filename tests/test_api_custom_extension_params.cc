// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs ::
//       sandboxes_sandbox_id_custom_extension_params_{get,patch}
//
// The orchestrator half of this was ported and reachable only from its own
// test: `PatchSandboxCustomExtensionParams` calls the extension's
// `hook_patch_params`, guards the sandbox state and writes the result back,
// and no URL led to it. These two routes are how a client reconfigures a
// running sandbox, so the whole hook path was dead in production.
//
// Most of what follows is the status mapping, because this route does not use
// the shared `ToApiError`. Two of its answers disagree with it on purpose, and
// both disagreements change what a client does next.
#include "microtest.h"

#include <memory>
#include <string>

#include "agentenv/api/impls.h"
#include "orchestrator_fakes.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT
using agentenv::orchestrator::SandboxMetadata;
using agentenv::orchestrator::SandboxState;
using agentenv::testing::FakeSandbox;
using agentenv::testing::Fixture;
using agentenv::testing::NowMs;

namespace custom_extension = agentenv::sandbox::custom_extension;

namespace {

/// A sandbox in `state`, registered with a handle so the patch path can find
/// it. The handle matters: without one the orchestrator reports a conflict
/// rather than reaching the hook.
SandboxMetadata Register(Fixture& f, SandboxState state) {
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = state;
    m.resources.cpu_count  = 2;
    m.resources.memory_mib = 512;
    m.created_at_ms = NowMs();
    MT_EXPECT_TRUE(f.store->Add(m).ok());
    std::unique_ptr<sandbox::SandboxBackend> backend(new FakeSandbox(&f.script));
    f.orch->RegisterSandboxHandle(
        m.id, orchestrator::SandboxHandlePtr(
                  new orchestrator::SandboxHandle(std::move(backend))));
    return m;
}

/// An extension that answers `hook_patch_params` however a test needs.
class FakeExtension : public custom_extension::Client {
 public:
    /// What the hook returns on success. Absent means "empty params", which
    /// the orchestrator treats as authoritative -- a hook may clear them.
    core::Optional<custom_extension::Params> result;
    /// Set to answer with a status instead, as a real extension refusing or
    /// breaking would.
    core::Optional<int> status;
    std::string last_patch;
    int         calls = 0;

    core::Expected<core::Optional<custom_extension::Params>, core::AnyError>
    HookPatchParams(const core::SandboxId&,
                    const custom_extension::Params& patch) override {
        ++calls;
        last_patch = patch.json_bytes;
        if (status.has_value()) {
            // Built the way the real client builds it, so the status rides in
            // the `AnyError` chain as a payload rather than as text. That is
            // the whole point of the case: the API layer has to read the
            // status back out.
            const custom_extension::HookStatusError hook(
                custom_extension::hook::kPatchParams, *status, "refused by policy");
            return core::make_unexpected(
                hook.ToAnyError().context("patch params hook failed"));
        }
        return result;
    }

    // Unused by this route.
    core::Expected<core::Optional<std::string>, core::AnyError> HookStartFresh(
        const core::SandboxId&, const custom_extension::SandboxInstanceId&,
        const std::string&, const std::string&,
        const core::Optional<custom_extension::Params>&) override {
        return core::Optional<std::string>();
    }
    core::Expected<core::Unit, core::AnyError> HookStartResume(
        const core::SandboxId&, const custom_extension::SandboxInstanceId&,
        const std::string&, const std::string&,
        const core::Optional<custom_extension::Params>&) override {
        return core::Unit();
    }
    void HookStop(const core::SandboxId&,
                  const custom_extension::SandboxInstanceId&) override {}
};

custom_extension::Params Params(const std::string& json) {
    custom_extension::Params p;
    p.json_bytes = json;
    return p;
}

}  // namespace

MT_TEST(reading_the_params_of_a_sandbox_that_never_set_any_is_an_empty_object) {
    // Upstream's `params_map_to_model(None)` is `HashMap::new()`, which
    // serializes as `{}`. Not `null`: the response is documented as a map, and
    // a client deserializing into one would fail on null for the ordinary case
    // of a sandbox with no extension params.
    Fixture f;
    const SandboxMetadata m = Register(f, SandboxState::Running);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, ApiError> r =
        impl.GetSandboxCustomExtensionParams(m.id.ToString());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_TRUE(r.value().kind() == core::Json::Kind::Object);
    MT_EXPECT_EQ(r.value().ToString(), std::string("{}"));
}

MT_TEST(reading_the_params_returns_what_was_stored) {
    Fixture f;
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = SandboxState::Running;
    m.created_at_ms = NowMs();
    m.custom_extension_params =
        core::Optional<custom_extension::Params>(Params("{\"team\":\"beta\"}"));
    MT_EXPECT_TRUE(f.store->Add(m).ok());

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, ApiError> r =
        impl.GetSandboxCustomExtensionParams(m.id.ToString());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_TRUE(r.value().kind() == core::Json::Kind::Object);
    MT_EXPECT_TRUE(r.value().ToString().find("beta") != std::string::npos);
}

MT_TEST(reading_the_params_of_an_unknown_sandbox_is_a_404) {
    Fixture f;
    ApiImpl impl(f.orch);

    const core::Expected<core::Json, ApiError> missing =
        impl.GetSandboxCustomExtensionParams(core::SandboxId::Fresh().ToString());
    MT_EXPECT_TRUE(!missing.ok());
    if (missing.ok()) return;
    MT_EXPECT_EQ(missing.error().code, 404);

    // An unparseable id is also a 404, not a 400: upstream's
    // `let Ok(id) = SandboxId::parse_str(..) else { sandbox_not_found(..) }`.
    // A 400 would distinguish "malformed id" from "no such sandbox", which
    // tells an unauthorized caller which ids are well-formed.
    const core::Expected<core::Json, ApiError> malformed =
        impl.GetSandboxCustomExtensionParams("not-a-uuid");
    MT_EXPECT_TRUE(!malformed.ok());
    if (malformed.ok()) return;
    MT_EXPECT_EQ(malformed.error().code, 404);
}

MT_TEST(a_patch_returns_the_full_params_the_hook_decided_on) {
    // The response is not the patch. The extension owns the merge -- it may
    // drop keys, add its own or normalise values -- so a client cannot
    // reconstruct the result from what it sent, and echoing the patch back
    // would be a plausible-looking lie.
    Fixture f;
    const SandboxMetadata m = Register(f, SandboxState::Running);
    std::shared_ptr<FakeExtension> ext(new FakeExtension());
    ext->result = core::Optional<custom_extension::Params>(
        Params("{\"team\":\"beta\",\"tier\":\"gold\"}"));
    f.orch->SetCustomExtensionClient(ext);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, core::AnyError> patch =
        core::Json::Parse("{\"team\":\"beta\"}");
    MT_EXPECT_TRUE(patch.ok());
    if (!patch.ok()) return;

    const core::Expected<core::Json, ApiError> r =
        impl.PatchSandboxCustomExtensionParams(m.id.ToString(), patch.value());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    // `tier` was never sent; it is in the answer because the hook put it there.
    MT_EXPECT_TRUE(r.value().ToString().find("gold") != std::string::npos);

    // The patch reached the hook verbatim.
    MT_EXPECT_EQ(ext->calls, 1);
    MT_EXPECT_TRUE(ext->last_patch.find("beta") != std::string::npos);

    // And it was stored, so the next GET agrees with the PATCH's answer.
    const core::Expected<core::Json, ApiError> read =
        impl.GetSandboxCustomExtensionParams(m.id.ToString());
    MT_EXPECT_TRUE(read.ok());
    if (!read.ok()) return;
    MT_EXPECT_EQ(read.value().ToString(), r.value().ToString());
}

MT_TEST(a_patch_a_hook_cleared_leaves_an_empty_object) {
    // A hook returning `None` means "no params", and the orchestrator takes
    // that as authoritative. The response is `{}` rather than the previous
    // value, which is the only way a caller can see that the clear happened.
    Fixture f;
    const SandboxMetadata m = Register(f, SandboxState::Running);
    std::shared_ptr<FakeExtension> ext(new FakeExtension());
    f.orch->SetCustomExtensionClient(ext);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, ApiError> r =
        impl.PatchSandboxCustomExtensionParams(m.id.ToString(),
                                               core::Json(core::JsonObject()));
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(r.value().ToString(), std::string("{}"));
    // An empty patch still runs the hook: it is the extension's chance to
    // rewrite params, not a no-op the API can shortcut.
    MT_EXPECT_EQ(ext->calls, 1);
}

MT_TEST(a_patch_of_a_paused_sandbox_is_a_409_not_a_400) {
    // This is the first place the route disagrees with `ToApiError`, which
    // maps `InvalidSandboxState` to 400. A 400 tells the client its request
    // was wrong and not to retry it; the request is fine and a resume makes it
    // work, so upstream answers 409.
    Fixture f;
    const SandboxMetadata m = Register(f, SandboxState::Paused);
    std::shared_ptr<FakeExtension> ext(new FakeExtension());
    f.orch->SetCustomExtensionClient(ext);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, ApiError> r =
        impl.PatchSandboxCustomExtensionParams(m.id.ToString(),
                                               core::Json(core::JsonObject()));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 409);
    // Upstream's wording, with the state that blocked it -- an operator needs
    // to know it was *paused* rather than, say, forking.
    MT_EXPECT_TRUE(r.error().message.find(
                       "custom extension params cannot be patched from paused state") !=
                   std::string::npos);
    // The hook was never called: the state guard runs first, so a paused
    // sandbox does not reach the extension at all.
    MT_EXPECT_EQ(ext->calls, 0);
}

MT_TEST(an_extension_that_rejects_the_patch_is_the_callers_fault) {
    // The second disagreement with `ToApiError`, which calls every
    // `SandboxOperationFailed` a 500. A 4xx from the extension means the
    // extension *considered* this patch and refused it, so the caller has to
    // change the request -- a 500 would have them retry it unchanged forever.
    Fixture f;
    const SandboxMetadata m = Register(f, SandboxState::Running);
    std::shared_ptr<FakeExtension> ext(new FakeExtension());
    ext->status = core::Optional<int>(403);
    f.orch->SetCustomExtensionClient(ext);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, ApiError> r =
        impl.PatchSandboxCustomExtensionParams(m.id.ToString(),
                                               core::Json(core::JsonObject()));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 400);
}

MT_TEST(an_extension_that_breaks_is_this_deployments_fault) {
    // The other side of the same split: 5xx from the extension is not
    // something the caller can fix, so it is a 500 carrying the hook's own
    // message rather than a 400 blaming the request.
    Fixture f;
    const SandboxMetadata m = Register(f, SandboxState::Running);
    std::shared_ptr<FakeExtension> ext(new FakeExtension());
    ext->status = core::Optional<int>(502);
    f.orch->SetCustomExtensionClient(ext);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, ApiError> r =
        impl.PatchSandboxCustomExtensionParams(m.id.ToString(),
                                               core::Json(core::JsonObject()));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 500);
    // The hook's message, which names the status and the operation. This is
    // what the structured `source` on `OrchestratorError` exists for: with the
    // chain flattened to text, the 502 and the 403 above are indistinguishable
    // and both would have to be guessed at from a message format.
    MT_EXPECT_TRUE(r.error().message.find("502") != std::string::npos);
}

MT_TEST(a_patch_without_a_configured_extension_is_refused_not_applied) {
    // No extension means nobody can approve the merge. Applying the patch
    // verbatim would be the dangerous answer: the params are forwarded to
    // lifecycle hooks, so storing unvetted ones changes how the sandbox
    // behaves with nothing having agreed to it.
    //
    // 400 rather than 500, because there is no hook error in the chain --
    // upstream's `None => Status400_BadRequest` arm.
    Fixture f;
    const SandboxMetadata m = Register(f, SandboxState::Running);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, ApiError> r =
        impl.PatchSandboxCustomExtensionParams(m.id.ToString(),
                                               core::Json(core::JsonObject()));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 400);
    MT_EXPECT_TRUE(r.error().message.find("custom extension is not configured") !=
                   std::string::npos);

    // Nothing was stored.
    const core::Expected<core::Json, ApiError> read =
        impl.GetSandboxCustomExtensionParams(m.id.ToString());
    MT_EXPECT_TRUE(read.ok());
    if (!read.ok()) return;
    MT_EXPECT_EQ(read.value().ToString(), std::string("{}"));
}

MT_TEST(a_patch_of_a_sandbox_with_no_live_handle_is_a_409) {
    // A sandbox the store knows about but this node holds no handle for -- it
    // was adopted from the store, or its handle went away. The params cannot
    // be patched because the hook needs the running instance, and that is a
    // conflict rather than a server fault: retrying after the handle is back
    // works, so a 500 would hide a recoverable state behind a bug report.
    Fixture f;
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = SandboxState::Running;
    m.created_at_ms = NowMs();
    MT_EXPECT_TRUE(f.store->Add(m).ok());  // no RegisterSandboxHandle

    std::shared_ptr<FakeExtension> ext(new FakeExtension());
    f.orch->SetCustomExtensionClient(ext);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, ApiError> r =
        impl.PatchSandboxCustomExtensionParams(m.id.ToString(),
                                               core::Json(core::JsonObject()));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 409);
    MT_EXPECT_EQ(ext->calls, 0);
}

MT_TEST(a_patch_body_that_is_not_an_object_is_a_400) {
    // The body *is* the patch map upstream (`HashMap<String, Object>`), so an
    // array or a scalar is not a patch at all. Refused before the hook, which
    // would otherwise be handed a document it cannot merge.
    Fixture f;
    const SandboxMetadata m = Register(f, SandboxState::Running);
    std::shared_ptr<FakeExtension> ext(new FakeExtension());
    f.orch->SetCustomExtensionClient(ext);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, core::AnyError> array = core::Json::Parse("[1,2]");
    MT_EXPECT_TRUE(array.ok());
    if (!array.ok()) return;

    const core::Expected<core::Json, ApiError> r =
        impl.PatchSandboxCustomExtensionParams(m.id.ToString(), array.value());
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 400);
    MT_EXPECT_EQ(ext->calls, 0);
}

MT_TEST(a_patch_of_an_unknown_sandbox_is_a_404) {
    Fixture f;
    std::shared_ptr<FakeExtension> ext(new FakeExtension());
    f.orch->SetCustomExtensionClient(ext);

    ApiImpl impl(f.orch);
    const core::Expected<core::Json, ApiError> r =
        impl.PatchSandboxCustomExtensionParams(core::SandboxId::Fresh().ToString(),
                                               core::Json(core::JsonObject()));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_EQ(r.error().code, 404);
    MT_EXPECT_EQ(ext->calls, 0);
}

MT_MAIN
