// SPDX-License-Identifier: MIT
// Rust: `tokio::process::Command::{env, spawn}` / `Child::{try_wait, kill}`.
#include "agentenv/core/process.h"

#include <unistd.h>

#include <chrono>
#include <thread>

#include "microtest.h"

using namespace agentenv::core;

MT_TEST(run_with_options_applies_env_overrides_in_order) {
    process::RunOptions options;
    options.env.push_back(std::make_pair(std::string("AENV_PROC_TEST"), std::string("one")));
    options.env.push_back(std::make_pair(std::string("AENV_PROC_TEST"), std::string("two")));
    options.env.push_back(std::make_pair(std::string("GOMAXPROCS"), std::string("4")));
    auto out = process::RunWithOptions(
        {"/bin/sh", "-c", "printf '%s %s' \"$AENV_PROC_TEST\" \"$GOMAXPROCS\""}, options);
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value().stdout_text, std::string("two 4"));
    // Inherited variables survive.
    auto path = process::RunWithOptions({"/bin/sh", "-c", "test -n \"$PATH\""}, options);
    MT_EXPECT_TRUE(path.ok());
    MT_EXPECT_EQ(path.value().code, 0);
}

MT_TEST(child_spawn_captures_streams_and_exit_code) {
    auto child = process::Child::Spawn({"/bin/sh", "-c", "echo out; echo err >&2; exit 3"},
                                       process::RunOptions());
    MT_EXPECT_TRUE(child.ok());
    auto code = child.value()->Wait();
    MT_EXPECT_TRUE(code.ok());
    MT_EXPECT_EQ(code.value(), 3);
    MT_EXPECT_EQ(child.value()->StdoutBytes(), std::string("out\n"));
    MT_EXPECT_EQ(child.value()->StderrBytes(), std::string("err\n"));
    // Reaped: try_wait keeps reporting the status.
    auto again = child.value()->TryWait();
    MT_EXPECT_TRUE(again.ok() && again.value().has_value());
    MT_EXPECT_EQ(*again.value(), 3);
}

MT_TEST(child_spawn_reports_exec_failure_as_error) {
    auto child = process::Child::Spawn({"/nonexistent/aenv-no-such-binary"},
                                       process::RunOptions());
    MT_EXPECT_TRUE(!child.ok());
}

MT_TEST(child_try_wait_and_kill) {
    auto child = process::Child::Spawn({"/bin/sh", "-c", "sleep 30"}, process::RunOptions());
    MT_EXPECT_TRUE(child.ok());
    auto running = child.value()->TryWait();
    MT_EXPECT_TRUE(running.ok());
    MT_EXPECT_TRUE(!running.value().has_value());
    child.value()->StartKill();
    auto code = child.value()->Wait();
    MT_EXPECT_TRUE(code.ok());
    MT_EXPECT_EQ(code.value(), 128 + 9);
}

// kill_on_drop: destroying the handle must not leave the child running.
MT_TEST(child_drop_kills_running_process) {
    const std::string marker = "/tmp/aenv-child-drop-" + std::to_string(::getpid());
    ::unlink(marker.c_str());
    {
        auto child = process::Child::Spawn(
            {"/bin/sh", "-c", "sleep 1; touch " + marker}, process::RunOptions());
        MT_EXPECT_TRUE(child.ok());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    MT_EXPECT_TRUE(::access(marker.c_str(), F_OK) != 0);
}

MT_MAIN
