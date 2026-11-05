// SPDX-License-Identifier: MIT
#include "microtest.h"
#include "agentenv/api/dto.h"

using namespace agentenv::api;

MT_TEST(create_req_roundtrip) {
    CreateSandboxReq r;
    r.template_id = "python:3.11";
    r.env_vars.push_back("FOO=bar");
    r.timeout_ms = 120000;

    auto j = r.ToJson();
    auto s = j.ToString();

    auto pj = agentenv::core::Json::Parse(s);
    MT_EXPECT_TRUE(pj.ok());
    auto pr = CreateSandboxReq::FromJson(pj.value());
    MT_EXPECT_TRUE(pr.ok());
    MT_EXPECT_EQ(pr.value().template_id, std::string("python:3.11"));
    MT_EXPECT_EQ(pr.value().timeout_ms, int64_t(120000));
    MT_EXPECT_EQ(static_cast<int>(pr.value().env_vars.size()), 1);
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
    auto absent = agentenv::core::Json::Parse("{\"template_id\":\"t\"}");
    MT_EXPECT_TRUE(absent.ok());
    auto parsed_absent = CreateSandboxReq::FromJson(absent.value());
    MT_EXPECT_TRUE(parsed_absent.ok());
    MT_EXPECT_TRUE(!parsed_absent.value().volume_mounts.has_value());

    auto empty = agentenv::core::Json::Parse("{\"template_id\":\"t\",\"volumeMounts\":[]}");
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
        "{\"template_id\":\"t\",\"volumeMounts\":[{\"name\":\"v\"}]}",
        "{\"template_id\":\"t\",\"volumeMounts\":[{\"path\":\"/mnt/data\"}]}",
        "{\"template_id\":\"t\",\"volumeMounts\":[\"v\"]}",
        "{\"template_id\":\"t\",\"volumeMounts\":{}}",
    };
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        auto j = agentenv::core::Json::Parse(bad[i]);
        MT_EXPECT_TRUE(j.ok());
        if (!j.ok()) continue;
        MT_EXPECT_TRUE(!CreateSandboxReq::FromJson(j.value()).ok());
    }
}

MT_MAIN
