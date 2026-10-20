// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/main.rs — the CLI surface and the config
// resolution that runs before the daemon serves anything.
//
// Split out of `main.cc` because the precedence rules below are the part worth
// testing: they decide which binary gets run and how big the warm pool is,
// from three sources (flag, environment, TOML) with a different winner for
// each field. A mistake here is not a crash, it is a daemon that quietly uses
// the wrong tool or the wrong watermarks.
#ifndef AGENTENV_STORAGE_UBLK_DAEMON_CONFIG_H_
#define AGENTENV_STORAGE_UBLK_DAEMON_CONFIG_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/warm-pool/pool.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

/// Rust `HOME_PATH_PLACEHOLDER` / the `DEFAULT_*` consts of main.rs.
extern const char* const kHomePathPlaceholder;
extern const char* const kDefaultHomePath;
extern const char* const kDefaultDepsPath;
static const uint64_t kDefaultResizeTimeoutSecs = 120;

/// Rust `Cli`.
///
/// Mirrors the flag set exactly, including which flags are optional: the
/// `Optional` fields are the ones clap leaves as `None`, and that difference
/// is what `LoadPoolConfig` reads as "no override" rather than "zero".
struct Cli {
    /// `--socket-path`, required.
    std::string socket_path;
    /// `--global-config`, required.
    std::string global_config;
    /// `--resize-global-config`, required.
    std::string resize_global_config;
    /// `--config`.
    core::Optional<std::string> config;
    /// `--log-level`, default "info".
    std::string log_level;
    /// `--log-file`.
    core::Optional<std::string> log_file;
    /// `--metrics-listen-addr`, default "0.0.0.0:9103". Empty disables it.
    std::string metrics_listen_addr;
    /// `--enable-pool`.
    bool enable_pool;
    /// `--pool-low-watermark`.
    core::Optional<std::size_t> pool_low_watermark;
    /// `--pool-high-watermark`.
    core::Optional<std::size_t> pool_high_watermark;
    /// `--pool-startup-prewarm`.
    core::Optional<bool> pool_startup_prewarm;
    /// `--p2p-publish-url`.
    core::Optional<std::string> p2p_publish_url;

    Cli()
        : log_level("info"),
          metrics_listen_addr("0.0.0.0:9103"),
          enable_pool(false) {}

    /// `Cli::parse`. Rejects unknown flags and missing required values rather
    /// than falling back to a default, because every required flag here names
    /// a path the daemon cannot invent.
    static core::Expected<Cli, std::string> Parse(int argc, char** argv);

    /// clap's generated `--help` text.
    static std::string Usage();
};

/// Rust `DaemonTomlConfig` and its nested tables.
///
/// Every field is optional in the same places Rust's are. `pool_block_present`
/// exists because Rust distinguishes a missing `[pool.block]` from one that is
/// present with all fields defaulted -- `enabled` falls back to the
/// `--enable-pool` flag only in the first case.
struct DaemonTomlConfig {
    core::Optional<std::string> home_path;
    core::Optional<std::string> deps_path;

    bool pool_present;
    core::Optional<std::size_t> pool_low_watermark;
    core::Optional<std::size_t> pool_high_watermark;

    bool pool_block_present;
    core::Optional<bool> pool_block_enabled;
    core::Optional<bool> pool_block_startup_prewarm;

    core::Optional<uint64_t> ublk_overlaybd_resize_timeout_secs;

    DaemonTomlConfig() : pool_present(false), pool_block_present(false) {}
};

/// Rust `PoolConfigOverrides` — the CLI's say over the TOML's.
struct PoolConfigOverrides {
    core::Optional<std::size_t> low_watermark;
    core::Optional<std::size_t> high_watermark;
    core::Optional<bool> startup_prewarm;
};

/// Rust `ResizeToolSpec`, as main.rs builds it.
///
/// Declared here rather than in a runtime header because `runtime.rs` is not
/// ported: this is the one part of it main.rs needs, and inventing the rest
/// would be a shape with nothing behind it.
struct ResizeToolSpec {
    std::string binary;
    core::Optional<std::string> lib_dir;
    uint64_t timeout_secs;

    ResizeToolSpec() : timeout_secs(kDefaultResizeTimeoutSecs) {}
};

/// Rust `load_daemon_config` — absent path means absent config, which is not
/// an error; an unreadable or malformed file is.
core::Expected<core::Optional<DaemonTomlConfig>, std::string> LoadDaemonConfig(
    const core::Optional<std::string>& path);

/// Rust `load_pool_config`.
///
/// Returns an absent config when the pool is off. Precedence per field is
/// override, then TOML, then the default -- and `maintenance_enabled` is
/// always false, because this daemon refills inline from acquire/release and
/// the generic background worker would fight it.
core::Expected<core::Optional<warmpool::PoolConfig>, std::string> LoadPoolConfig(
    const core::Optional<DaemonTomlConfig>& config, bool force_enable,
    const PoolConfigOverrides& overrides);

/// Rust `load_resize_tool_config`.
///
/// Absent when there is no TOML config at all: the spec's binary path is
/// derived from `deps_path`, and without a config there is nothing to derive
/// it from.
core::Expected<core::Optional<ResizeToolSpec>, std::string> LoadResizeToolConfig(
    const core::Optional<std::string>& config_path,
    const core::Optional<DaemonTomlConfig>& config);

/// Rust `env_path` — a trimmed, non-empty environment variable.
///
/// An empty or whitespace-only value is treated as unset, so exporting
/// `AENV_DEPS_PATH=` does not resolve every dependency against the filesystem
/// root.
core::Optional<std::string> EnvPath(const char* name);

/// Rust `resolve_config_path` — expand `$AENV_HOME`, then resolve relatively.
std::string ResolveConfigPath(const std::string& home_path, const std::string& config_dir,
                              const std::string& raw);

/// Rust `resolve_relative_to`.
std::string ResolveRelativeTo(const std::string& base_dir, const std::string& path);

/// Rust `default_pool_config`.
warmpool::PoolConfig DefaultPoolConfig();

/// Rust `apply_pool_overrides`.
warmpool::PoolConfig ApplyPoolOverrides(warmpool::PoolConfig config,
                                         const PoolConfigOverrides& overrides);

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_DAEMON_CONFIG_H_
