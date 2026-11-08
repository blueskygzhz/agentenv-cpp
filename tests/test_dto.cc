// SPDX-License-Identifier: MIT
#include "microtest.h"
#include "agentenv/api/dto.h"

using namespace agentenv::api;

MT_TEST(create_req_roundtrip) {
    CreateSandboxReq r;
    r.template_id = "python:3.11";
    r.env_vars.push_back("FOO=bar");
    r.timeout_secs = agentenv::core::Optional<int64_t>(static_cast<int64_t>(120));

    auto j = r.ToJson();
    auto s = j.ToString();

    auto pj = agentenv::core::Json::Parse(s);
    MT_EXPECT_TRUE(pj.ok());
    auto pr = CreateSandboxReq::FromJson(pj.value());
    MT_EXPECT_TRUE(pr.ok());
    MT_EXPECT_EQ(pr.value().template_id, std::string("python:3.11"));
    MT_EXPECT_TRUE(pr.value().timeout_secs.has_value());
    MT_EXPECT_EQ(*pr.value().timeout_secs, int64_t(120));
    MT_EXPECT_EQ(static_cast<int>(pr.value().env_vars.size()), 1);
}

// ---- wire keys --------------------------------------------------------------
//
// The keys are upstream's `#[serde(rename)]` spellings. This port previously
// used `template_id`, `timeout_ms` and `env_vars`, none of which appear in the
// schema `dto.h` claims compatibility with -- a real client's body parsed as a
// request with no template at all.

MT_TEST(create_req_uses_the_schemas_key_spellings) {
    CreateSandboxReq r;
    r.template_id = "base";
    r.env_vars.push_back("FOO=bar");
    r.timeout_secs = agentenv::core::Optional<int64_t>(static_cast<int64_t>(30));
    r.allow_internet_access = agentenv::core::Optional<bool>(false);

    const std::string s = r.ToJson().ToString();
    MT_EXPECT_TRUE(s.find("\"templateID\"") != std::string::npos);
    MT_EXPECT_TRUE(s.find("\"envVars\"") != std::string::npos);
    MT_EXPECT_TRUE(s.find("\"timeout\"") != std::string::npos);
    // Snake_case for this body only; `NewColdSandbox` spells it camelCase.
    MT_EXPECT_TRUE(s.find("\"allow_internet_access\"") != std::string::npos);
    // The old spellings must be gone, not merely accompanied.
    MT_EXPECT_TRUE(s.find("\"template_id\"") == std::string::npos);
    MT_EXPECT_TRUE(s.find("\"timeout_ms\"") == std::string::npos);
    MT_EXPECT_TRUE(s.find("\"env_vars\"") == std::string::npos);
}

MT_TEST(create_req_rejects_a_body_with_no_template_id) {
    // `templateID` is the schema's only required field, so a body missing it
    // must not parse into a request that then reports "template  not found".
    agentenv::core::Json j;
    j["env_vars"] = agentenv::core::Json(agentenv::core::JsonArray());
    MT_EXPECT_TRUE(!CreateSandboxReq::FromJson(j).ok());
}

MT_TEST(create_req_reads_the_remaining_schema_fields) {
    // Six fields that the handler had no way to receive: every one of them was
    // accepted by no DTO member and honoured by no code.
    const std::string body =
        "{\"templateID\":\"base\","
        "\"autoPause\":false,"
        "\"autoResume\":{\"enabled\":true},"
        "\"allow_internet_access\":false,"
        "\"network\":{\"allowOut\":[\"10.0.0.0/8\"],\"allowPublicTraffic\":false},"
        "\"metadata\":{\"team\":\"infra\"},"
        "\"customExtensionParams\":{\"tier\":\"gold\"}}";
    const auto parsed = agentenv::core::Json::Parse(body);
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    const auto r = CreateSandboxReq::FromJson(parsed.value());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;

    MT_EXPECT_TRUE(r.value().auto_pause.has_value() && !*r.value().auto_pause);
    MT_EXPECT_TRUE(r.value().auto_resume.has_value() && *r.value().auto_resume);
    MT_EXPECT_TRUE(r.value().allow_internet_access.has_value() &&
                   !*r.value().allow_internet_access);
    MT_EXPECT_TRUE(r.value().network.has_value());
    if (!r.value().network.has_value()) return;
    MT_EXPECT_TRUE(r.value().network->allow_out.has_value());
    MT_EXPECT_TRUE(r.value().network->allow_public_traffic.has_value() &&
                   !*r.value().network->allow_public_traffic);
    MT_EXPECT_EQ(r.value().metadata.find("team")->second, std::string("infra"));
    MT_EXPECT_TRUE(r.value().custom_extension_params.has_value());
}

MT_TEST(an_auto_resume_object_without_enabled_is_refused) {
    // `enabled` is required by the schema, so a present object missing it does
    // not deserialize upstream. Reading it as "disabled" would silently ignore
    // a caller who asked for auto-resume and got a sandbox that never wakes.
    agentenv::core::Json j;
    j["templateID"] = agentenv::core::Json(std::string("base"));
    j["autoResume"] = agentenv::core::Json();  // null is absent, and fine
    MT_EXPECT_TRUE(CreateSandboxReq::FromJson(j).ok());

    const auto parsed = agentenv::core::Json::Parse(
        "{\"templateID\":\"base\",\"autoResume\":{}}");
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    MT_EXPECT_TRUE(!CreateSandboxReq::FromJson(parsed.value()).ok());
}

MT_TEST(a_negative_timeout_is_refused) {
    // Upstream's field is `Option<u32>`, which cannot hold one. Clamping it to
    // zero would turn a malformed body into an explicit "no timeout".
    const auto parsed = agentenv::core::Json::Parse(
        "{\"templateID\":\"base\",\"timeout\":-1}");
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    MT_EXPECT_TRUE(!CreateSandboxReq::FromJson(parsed.value()).ok());
}

MT_TEST(a_bare_boolean_auto_resume_is_refused) {
    // The schema defines `autoResume` as an object with a required `enabled`.
    // Accepting a bare boolean as well would let a shape the server cannot
    // receive from a conforming client decide a real setting -- and the two
    // forms would then disagree about which one is authoritative.
    const auto parsed = agentenv::core::Json::Parse(
        "{\"templateID\":\"base\",\"autoResume\":true}");
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    MT_EXPECT_TRUE(!CreateSandboxReq::FromJson(parsed.value()).ok());
}

MT_TEST(a_present_empty_network_list_stays_present) {
    // `SandboxNetworkEgressPolicy` reads an empty allow list as "allow
    // nothing" and no allow list as "no allowlist constraint". Collapsing the
    // present-empty case to absent would turn a deliberate lockdown into
    // unrestricted egress.
    const auto parsed = agentenv::core::Json::Parse(
        "{\"templateID\":\"base\",\"network\":{\"allowOut\":[],\"denyOut\":[]}}");
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    const auto r = CreateSandboxReq::FromJson(parsed.value());
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_TRUE(r.value().network.has_value());
    if (!r.value().network.has_value()) return;
    MT_EXPECT_TRUE(r.value().network->allow_out.has_value());
    MT_EXPECT_TRUE(r.value().network->allow_out->empty());
    MT_EXPECT_TRUE(r.value().network->deny_out.has_value());
    MT_EXPECT_TRUE(r.value().network->deny_out->empty());
}

MT_TEST(the_two_create_bodies_spell_the_internet_flag_differently) {
    // Upstream's own inconsistency: `NewSandbox` uses `allow_internet_access`
    // and `NewColdSandbox` uses `allowInternetAccess`. Both are reproduced,
    // so each body must accept its own spelling and ignore the other's --
    // otherwise one of the two routes silently drops the flag.
    const auto warm = agentenv::core::Json::Parse(
        "{\"templateID\":\"base\",\"allow_internet_access\":false,"
        "\"allowInternetAccess\":true}");
    MT_EXPECT_TRUE(warm.ok());
    if (!warm.ok()) return;
    const auto warm_req = CreateSandboxReq::FromJson(warm.value());
    MT_EXPECT_TRUE(warm_req.ok());
    if (!warm_req.ok()) return;
    MT_EXPECT_TRUE(warm_req.value().allow_internet_access.has_value());
    if (!warm_req.value().allow_internet_access.has_value()) return;
    MT_EXPECT_TRUE(!*warm_req.value().allow_internet_access);

    const auto cold = agentenv::core::Json::Parse(
        "{\"image\":\"python:3.11-slim\",\"allowInternetAccess\":false,"
        "\"allow_internet_access\":true}");
    MT_EXPECT_TRUE(cold.ok());
    if (!cold.ok()) return;
    const auto cold_req = CreateColdSandboxReq::FromJson(cold.value());
    MT_EXPECT_TRUE(cold_req.ok());
    if (!cold_req.ok()) return;
    MT_EXPECT_TRUE(cold_req.value().allow_internet_access.has_value());
    if (!cold_req.value().allow_internet_access.has_value()) return;
    MT_EXPECT_TRUE(!*cold_req.value().allow_internet_access);
}

MT_TEST(an_omitted_timeout_stays_absent_rather_than_defaulting) {
    // The schema says `default: 15`, but the generated model carries no
    // `#[serde(default)]` -- an omitted timeout reaches the orchestrator as
    // `None` and the node's configured default applies. Baking 15 in here
    // would override an operator's setting with a value nobody chose.
    agentenv::core::Json j;
    j["templateID"] = agentenv::core::Json(std::string("base"));
    const auto r = CreateSandboxReq::FromJson(j);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_TRUE(!r.value().timeout_secs.has_value());
}

// ---- volumeMounts -----------------------------------------------------------
//
// Rust `models::NewSandbox::volume_mounts: Option<Vec<SandboxVolumeMount>>`.
// Until this field existed, a request carrying `volumeMounts` parsed cleanly
// and the mounts were silently dropped -- the caller got a 201 and a sandbox
// with no data disk, which is the worst possible answer.

MT_TEST(create_req_volume_mounts_roundtrip) {
    CreateSandboxReq r;
    r.template_id = "python:3.11";
    std::vector<SandboxVolumeMountModel> mounts;
    SandboxVolumeMountModel m;
    m.name = "pgdata";
    m.path = "/mnt/data";
    mounts.push_back(m);
    r.volume_mounts = agentenv::core::Optional<std::vector<SandboxVolumeMountModel> >(mounts);

    auto pj = agentenv::core::Json::Parse(r.ToJson().ToString());
    MT_EXPECT_TRUE(pj.ok());
    auto pr = CreateSandboxReq::FromJson(pj.value());
    MT_EXPECT_TRUE(pr.ok());
    if (!pr.ok()) return;
    MT_EXPECT_TRUE(pr.value().volume_mounts.has_value());
    if (!pr.value().volume_mounts.has_value()) return;
    MT_EXPECT_EQ(static_cast<int>(pr.value().volume_mounts->size()), 1);
    MT_EXPECT_EQ((*pr.value().volume_mounts)[0].name, std::string("pgdata"));
    MT_EXPECT_EQ((*pr.value().volume_mounts)[0].path, std::string("/mnt/data"));
}

MT_TEST(create_req_distinguishes_an_absent_volume_mounts_list_from_an_empty_one) {
    // Absent means "restore whatever the snapshot captured" upstream; an empty
    // list means "no volumes". Collapsing them would make a restore
    // indistinguishable from an explicit refusal of every volume.
    auto absent = agentenv::core::Json::Parse("{\"templateID\":\"t\"}");
    MT_EXPECT_TRUE(absent.ok());
    auto parsed_absent = CreateSandboxReq::FromJson(absent.value());
    MT_EXPECT_TRUE(parsed_absent.ok());
    MT_EXPECT_TRUE(!parsed_absent.value().volume_mounts.has_value());

    auto empty = agentenv::core::Json::Parse("{\"templateID\":\"t\",\"volumeMounts\":[]}");
    MT_EXPECT_TRUE(empty.ok());
    auto parsed_empty = CreateSandboxReq::FromJson(empty.value());
    MT_EXPECT_TRUE(parsed_empty.ok());
    MT_EXPECT_TRUE(parsed_empty.value().volume_mounts.has_value());
    if (!parsed_empty.value().volume_mounts.has_value()) return;
    MT_EXPECT_TRUE(parsed_empty.value().volume_mounts->empty());
}

MT_TEST(create_req_rejects_a_malformed_volume_mount) {
    // Both halves are non-optional `String` upstream, so a mount missing
    // either is a body that fails to deserialize -- not a mount at the empty
    // path, which normalization would turn into something never asked for.
    const char* bad[] = {
        "{\"templateID\":\"t\",\"volumeMounts\":[{\"name\":\"v\"}]}",
        "{\"templateID\":\"t\",\"volumeMounts\":[{\"path\":\"/mnt/data\"}]}",
        "{\"templateID\":\"t\",\"volumeMounts\":[\"v\"]}",
        "{\"templateID\":\"t\",\"volumeMounts\":{}}",
    };
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        auto j = agentenv::core::Json::Parse(bad[i]);
        MT_EXPECT_TRUE(j.ok());
        if (!j.ok()) continue;
        MT_EXPECT_TRUE(!CreateSandboxReq::FromJson(j.value()).ok());
    }
}

MT_MAIN
