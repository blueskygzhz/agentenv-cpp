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

MT_MAIN
