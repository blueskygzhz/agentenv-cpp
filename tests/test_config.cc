// SPDX-License-Identifier: MIT
#include "microtest.h"
#include "agentenv/core/config.h"

using namespace agentenv::core;

MT_TEST(config_parse_string) {
    std::string toml =
        "server_bind_addr = \"127.0.0.1:9000\"\n"
        "log_level = 3\n"
        "sandbox_backend = \"mock\"\n"
        "[snapshot]\n"
        "repo_kind = \"local\"\n"
        "repo_path = \"/tmp/snap\"\n";

    auto res = Config::ParseString(toml);
    MT_EXPECT_TRUE(res.ok());
    Config c = res.value();
    MT_EXPECT_EQ(c.server_bind_addr, std::string("127.0.0.1:9000"));
    MT_EXPECT_EQ(c.log_level, 3);
    MT_EXPECT_EQ(c.snapshot_repo_path, std::string("/tmp/snap"));
}

MT_MAIN
