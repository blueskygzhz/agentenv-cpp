// SPDX-License-Identifier: MIT
// Tests for storage/ublk-daemon's config resolution, ported against
// `storage/ublk-daemon/src/main.rs`.
//
// These cover the precedence rules rather than the parsing: a parse error is
// loud, but picking the wrong source for a field is silent. The daemon would
// come up with the wrong watermarks or run the wrong resize binary, and
// nothing would say so.
#include "agentenv/storage/ublk-daemon/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>

#include "agentenv/storage/ublk-daemon/lifecycle.h"
#include "microtest.h"

namespace ud = agentenv::storage::ublk_daemon;
namespace core = agentenv::core;

namespace {

/// Writes `body` to a unique temp path and returns it. Removed by `TempFile`'s
/// destructor so a failing assertion does not leave files behind.
class TempFile {
 public:
    explicit TempFile(const std::string& body) {
        char buf[] = "/tmp/agentenv-ud-test-XXXXXX";
        const int fd = ::mkstemp(buf);
        path_ = buf;
        if (fd >= 0) {
            const ssize_t n = ::write(fd, body.data(), body.size());
            (void)n;
            ::close(fd);
        }
    }
    ~TempFile() {
        if (!path_.empty()) ::unlink(path_.c_str());
    }
    const std::string& path() const { return path_; }

 private:
    TempFile(const TempFile&);
    TempFile& operator=(const TempFile&);
    std::string path_;
};

/// Builds an argv for `Cli::Parse`, which takes `char**` as a real main would.
class Args {
 public:
    explicit Args(const std::vector<std::string>& items) : items_(items) {
        ptrs_.push_back(const_cast<char*>("agentenv-ublk-daemon"));
        for (std::size_t i = 0; i < items_.size(); ++i) {
            ptrs_.push_back(const_cast<char*>(items_[i].c_str()));
        }
    }
    int argc() const { return static_cast<int>(ptrs_.size()); }
    char** argv() { return &ptrs_[0]; }

 private:
    std::vector<std::string> items_;
    std::vector<char*> ptrs_;
};

std::vector<std::string> RequiredArgs() {
    std::vector<std::string> v;
    v.push_back("--socket-path");
    v.push_back("/tmp/s.sock");
    v.push_back("--global-config");
    v.push_back("/tmp/g.json");
    v.push_back("--resize-global-config");
    v.push_back("/tmp/r.json");
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Cli
// ---------------------------------------------------------------------------

MT_TEST(cli_requires_the_three_paths) {
    // Named, not `Args empty(std::vector<std::string>())`: that form parses as
    // a function declaration, not a variable.
    std::vector<std::string> none;
    Args empty(none);
    const core::Expected<ud::Cli, std::string> r = ud::Cli::Parse(empty.argc(), empty.argv());
    MT_EXPECT_TRUE(!r.ok());
    // All three are named at once, so one run tells the operator everything
    // that is missing.
    MT_EXPECT_TRUE(r.error().find("--socket-path") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("--global-config") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("--resize-global-config") != std::string::npos);
}

MT_TEST(cli_defaults_match_clap) {
    std::vector<std::string> v = RequiredArgs();
    Args a(v);
    const core::Expected<ud::Cli, std::string> r = ud::Cli::Parse(a.argc(), a.argv());
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().log_level, std::string("info"));
    MT_EXPECT_EQ(r.value().metrics_listen_addr, std::string("0.0.0.0:9103"));
    MT_EXPECT_TRUE(!r.value().enable_pool);
    MT_EXPECT_TRUE(!r.value().config.has_value());
    MT_EXPECT_TRUE(!r.value().pool_low_watermark.has_value());
}

MT_TEST(an_empty_metrics_address_is_kept_not_defaulted) {
    // Empty is how the flag disables metrics; defaulting it would start a
    // listener the operator asked not to have.
    std::vector<std::string> v = RequiredArgs();
    v.push_back("--metrics-listen-addr");
    v.push_back("");
    Args a(v);
    const core::Expected<ud::Cli, std::string> r = ud::Cli::Parse(a.argc(), a.argv());
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().metrics_listen_addr, std::string(""));
}

MT_TEST(a_flag_without_its_value_is_refused) {
    std::vector<std::string> v = RequiredArgs();
    v.push_back("--log-file");
    Args a(v);
    const core::Expected<ud::Cli, std::string> r = ud::Cli::Parse(a.argc(), a.argv());
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("missing value") != std::string::npos);
}

MT_TEST(unknown_arguments_are_refused) {
    std::vector<std::string> v = RequiredArgs();
    v.push_back("--not-a-flag");
    v.push_back("x");
    Args a(v);
    const core::Expected<ud::Cli, std::string> r = ud::Cli::Parse(a.argc(), a.argv());
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("unknown argument") != std::string::npos);
}

MT_TEST(key_equals_value_form_is_accepted) {
    std::vector<std::string> v;
    v.push_back("--socket-path=/tmp/a.sock");
    v.push_back("--global-config=/tmp/g.json");
    v.push_back("--resize-global-config=/tmp/r.json");
    v.push_back("--pool-low-watermark=7");
    Args a(v);
    const core::Expected<ud::Cli, std::string> r = ud::Cli::Parse(a.argc(), a.argv());
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_EQ(r.value().socket_path, std::string("/tmp/a.sock"));
    MT_EXPECT_TRUE(r.value().pool_low_watermark.has_value());
    MT_EXPECT_EQ(*r.value().pool_low_watermark, static_cast<std::size_t>(7));
}

MT_TEST(a_non_numeric_watermark_is_refused) {
    std::vector<std::string> v = RequiredArgs();
    v.push_back("--pool-high-watermark");
    v.push_back("lots");
    Args a(v);
    const core::Expected<ud::Cli, std::string> r = ud::Cli::Parse(a.argc(), a.argv());
    MT_EXPECT_TRUE(!r.ok());
}

// ---------------------------------------------------------------------------
// Pool config precedence
// ---------------------------------------------------------------------------

MT_TEST(no_config_means_the_flag_alone_decides) {
    const core::Optional<ud::DaemonTomlConfig> none;
    ud::PoolConfigOverrides no_overrides;

    const core::Expected<core::Optional<agentenv::warmpool::PoolConfig>, std::string> off =
        ud::LoadPoolConfig(none, false, no_overrides);
    MT_EXPECT_TRUE(off.ok());
    MT_EXPECT_TRUE(!off.value().has_value());

    const core::Expected<core::Optional<agentenv::warmpool::PoolConfig>, std::string> on =
        ud::LoadPoolConfig(none, true, no_overrides);
    MT_EXPECT_TRUE(on.ok());
    MT_EXPECT_TRUE(on.value().has_value());
    MT_EXPECT_EQ(on.value()->low_watermark, static_cast<std::size_t>(2));
    MT_EXPECT_EQ(on.value()->high_watermark, static_cast<std::size_t>(64));
    MT_EXPECT_TRUE(on.value()->startup_prewarm);
}

MT_TEST(an_explicit_enabled_false_beats_the_enable_flag) {
    // This is what lets a host config switch the pool off without editing the
    // service's arguments. Reading it the other way round would ignore the
    // config and silently keep warming devices.
    TempFile f("[pool.block]\nenabled = false\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());

    ud::PoolConfigOverrides no_overrides;
    const core::Expected<core::Optional<agentenv::warmpool::PoolConfig>, std::string> r =
        ud::LoadPoolConfig(cfg.value(), /*force_enable=*/true, no_overrides);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value().has_value());
}

MT_TEST(enabled_true_turns_the_pool_on_without_the_flag) {
    TempFile f("[pool.block]\nenabled = true\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());

    ud::PoolConfigOverrides no_overrides;
    const core::Expected<core::Optional<agentenv::warmpool::PoolConfig>, std::string> r =
        ud::LoadPoolConfig(cfg.value(), /*force_enable=*/false, no_overrides);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
}

MT_TEST(watermarks_come_from_pool_not_pool_block) {
    // Rust reads the watermarks from `[pool]` and only `enabled` /
    // `startup_prewarm` from `[pool.block]`. Reading them from the wrong table
    // would leave both at their defaults with no error.
    TempFile f(
        "[pool]\nlow_watermark = 5\nhigh_watermark = 9\n"
        "[pool.block]\nenabled = true\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());

    ud::PoolConfigOverrides no_overrides;
    const core::Expected<core::Optional<agentenv::warmpool::PoolConfig>, std::string> r =
        ud::LoadPoolConfig(cfg.value(), false, no_overrides);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    MT_EXPECT_EQ(r.value()->low_watermark, static_cast<std::size_t>(5));
    MT_EXPECT_EQ(r.value()->high_watermark, static_cast<std::size_t>(9));
}

MT_TEST(cli_overrides_beat_the_config_file) {
    TempFile f(
        "[pool]\nlow_watermark = 5\nhigh_watermark = 9\n"
        "[pool.block]\nenabled = true\nstartup_prewarm = true\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());

    ud::PoolConfigOverrides overrides;
    overrides.low_watermark = core::Optional<std::size_t>(1);
    overrides.high_watermark = core::Optional<std::size_t>(3);
    overrides.startup_prewarm = core::Optional<bool>(false);

    const core::Expected<core::Optional<agentenv::warmpool::PoolConfig>, std::string> r =
        ud::LoadPoolConfig(cfg.value(), false, overrides);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    MT_EXPECT_EQ(r.value()->low_watermark, static_cast<std::size_t>(1));
    MT_EXPECT_EQ(r.value()->high_watermark, static_cast<std::size_t>(3));
    MT_EXPECT_TRUE(!r.value()->startup_prewarm);
}

MT_TEST(maintenance_is_always_off_for_this_daemon) {
    // The daemon refills inline from acquire/release because prewarming needs
    // the request's current overlaybd image. A background worker would
    // duplicate that against a stale one, so this is not configurable.
    const core::Optional<ud::DaemonTomlConfig> none;
    ud::PoolConfigOverrides no_overrides;
    const core::Expected<core::Optional<agentenv::warmpool::PoolConfig>, std::string> a =
        ud::LoadPoolConfig(none, true, no_overrides);
    MT_EXPECT_TRUE(a.ok());
    MT_EXPECT_TRUE(a.value().has_value());
    MT_EXPECT_TRUE(!a.value()->maintenance_enabled);

    TempFile f("[pool]\nlow_watermark = 4\n[pool.block]\nenabled = true\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());
    const core::Expected<core::Optional<agentenv::warmpool::PoolConfig>, std::string> b =
        ud::LoadPoolConfig(cfg.value(), false, no_overrides);
    MT_EXPECT_TRUE(b.ok());
    MT_EXPECT_TRUE(b.value().has_value());
    MT_EXPECT_TRUE(!b.value()->maintenance_enabled);
}

MT_TEST(a_negative_watermark_is_refused) {
    TempFile f("[pool]\nlow_watermark = -1\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    // Refused at read time: converting it would wrap into an enormous ceiling.
    MT_EXPECT_TRUE(!cfg.ok());
}

// ---------------------------------------------------------------------------
// Resize tool
// ---------------------------------------------------------------------------

MT_TEST(no_config_means_no_resize_tool) {
    const core::Optional<ud::DaemonTomlConfig> none;
    const core::Expected<core::Optional<ud::ResizeToolSpec>, std::string> r =
        ud::LoadResizeToolConfig(core::Optional<std::string>(), none);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value().has_value());
}

MT_TEST(resize_tool_path_expands_the_home_placeholder) {
    ::unsetenv("AENV_HOME_PATH");
    ::unsetenv("AENV_DEPS_PATH");
    TempFile f("home_path = \"/opt/aenv\"\ndeps_path = \"$AENV_HOME/env\"\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());

    const core::Expected<core::Optional<ud::ResizeToolSpec>, std::string> r =
        ud::LoadResizeToolConfig(core::Optional<std::string>(f.path()), cfg.value());
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    MT_EXPECT_EQ(r.value()->binary,
                 std::string("/opt/aenv/env/overlaybd/bin/overlaybd-resize"));
    MT_EXPECT_EQ(r.value()->timeout_secs, static_cast<uint64_t>(120));
}

MT_TEST(the_environment_beats_the_config_file) {
    TempFile f("home_path = \"/opt/aenv\"\ndeps_path = \"/from/file\"\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());

    ::setenv("AENV_DEPS_PATH", "/from/env", 1);
    const core::Expected<core::Optional<ud::ResizeToolSpec>, std::string> r =
        ud::LoadResizeToolConfig(core::Optional<std::string>(f.path()), cfg.value());
    ::unsetenv("AENV_DEPS_PATH");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    MT_EXPECT_EQ(r.value()->binary,
                 std::string("/from/env/overlaybd/bin/overlaybd-resize"));
}

MT_TEST(a_blank_environment_value_counts_as_unset) {
    // Exporting `AENV_DEPS_PATH=` must not resolve every dependency against
    // the filesystem root.
    TempFile f("home_path = \"/opt/aenv\"\ndeps_path = \"/from/file\"\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());

    ::setenv("AENV_DEPS_PATH", "   ", 1);
    const core::Expected<core::Optional<ud::ResizeToolSpec>, std::string> r =
        ud::LoadResizeToolConfig(core::Optional<std::string>(f.path()), cfg.value());
    ::unsetenv("AENV_DEPS_PATH");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    MT_EXPECT_EQ(r.value()->binary,
                 std::string("/from/file/overlaybd/bin/overlaybd-resize"));
}

MT_TEST(a_zero_resize_timeout_is_refused) {
    // Zero would make every resize fail at once, which reads as a broken tool
    // rather than a config mistake.
    TempFile f("home_path = \"/opt/aenv\"\n[ublk.overlaybd]\nresize_timeout_secs = 0\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());

    const core::Expected<core::Optional<ud::ResizeToolSpec>, std::string> r =
        ud::LoadResizeToolConfig(core::Optional<std::string>(f.path()), cfg.value());
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("resize_timeout_secs must be > 0") != std::string::npos);
}

MT_TEST(a_relative_deps_path_resolves_against_the_config_dir) {
    ::unsetenv("AENV_DEPS_PATH");
    ::unsetenv("AENV_HOME_PATH");
    TempFile f("deps_path = \"./env\"\n");
    const core::Expected<core::Optional<ud::DaemonTomlConfig>, std::string> cfg =
        ud::LoadDaemonConfig(core::Optional<std::string>(f.path()));
    MT_EXPECT_TRUE(cfg.ok());

    const core::Expected<core::Optional<ud::ResizeToolSpec>, std::string> r =
        ud::LoadResizeToolConfig(core::Optional<std::string>(f.path()), cfg.value());
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_value());
    // The config lives in /tmp, so `./env` is /tmp/./env -- not the process's
    // working directory, which is what a plain relative path would have meant.
    MT_EXPECT_EQ(r.value()->binary,
                 std::string("/tmp/./env/overlaybd/bin/overlaybd-resize"));
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

MT_TEST(resolve_relative_to_leaves_absolute_paths_alone) {
    MT_EXPECT_EQ(ud::ResolveRelativeTo("/base", "/abs/path"), std::string("/abs/path"));
    MT_EXPECT_EQ(ud::ResolveRelativeTo("/base", "rel"), std::string("/base/rel"));
    // A base that already ends in a slash must not produce a double one.
    MT_EXPECT_EQ(ud::ResolveRelativeTo("/base/", "rel"), std::string("/base/rel"));
}

MT_TEST(the_home_placeholder_is_replaced_everywhere_it_appears) {
    MT_EXPECT_EQ(ud::ResolveConfigPath("/h", "/c", "$AENV_HOME/a/$AENV_HOME/b"),
                 std::string("/h/a//h/b"));
    // No placeholder: the path is only resolved, not rewritten.
    MT_EXPECT_EQ(ud::ResolveConfigPath("/h", "/c", "plain"), std::string("/c/plain"));
}

// ---------------------------------------------------------------------------
// Log level
// ---------------------------------------------------------------------------

MT_TEST(log_levels_parse_and_off_is_distinct) {
    const core::Expected<ud::LogLevelSpec, std::string> off = ud::ParseLogLevel("off");
    MT_EXPECT_TRUE(off.ok());
    // `off` is carried separately rather than folded into Error: a daemon told
    // to be silent must not still print errors.
    MT_EXPECT_TRUE(off.value().off);

    const core::Expected<ud::LogLevelSpec, std::string> dbg = ud::ParseLogLevel("debug");
    MT_EXPECT_TRUE(dbg.ok());
    MT_EXPECT_TRUE(!dbg.value().off);
    MT_EXPECT_TRUE(dbg.value().level == agentenv::core::LogLevel::Debug);
}

MT_TEST(an_unknown_log_level_is_refused_not_defaulted) {
    // Defaulting a typo to `info` would hide the `debug` someone was turning
    // on, and they would conclude the logging itself is broken.
    const core::Expected<ud::LogLevelSpec, std::string> r = ud::ParseLogLevel("verbose");
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("verbose") != std::string::npos);
}
MT_MAIN
