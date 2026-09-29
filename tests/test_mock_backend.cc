// SPDX-License-Identifier: MIT
#include "microtest.h"
#include "agentenv/sandbox/mock.h"

using namespace agentenv;
using namespace agentenv::sandbox;

MT_TEST(mock_boot_and_shutdown) {
    MockBackend b;
    LaunchPlan p;
    p.sandbox_id = core::SandboxId::Fresh();
    p.template_id = "t1";

    auto boot = b.Boot(p);
    MT_EXPECT_TRUE(boot.ok());
    MT_EXPECT_EQ(boot.value().sandbox_id.ToString(), p.sandbox_id.ToString());

    auto stop = b.Shutdown(p.sandbox_id);
    MT_EXPECT_TRUE(stop.ok());
}

MT_TEST(mock_exec_echo) {
    MockBackend b;
    ExecSpec s;
    s.cmd = {"echo", "hi"};
    auto r = b.Exec(core::SandboxId::Fresh(), s);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().exit_code, 0);
}

MT_MAIN
