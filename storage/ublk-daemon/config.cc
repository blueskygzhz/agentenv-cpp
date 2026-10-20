// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/main.rs
#include "agentenv/storage/ublk-daemon/config.h"

#include <stdlib.h>

#include <sstream>

#include "agentenv/core/toml.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

const char* const kHomePathPlaceholder = "$AENV_HOME";
const char* const kDefaultHomePath = "/var/lib/aenv";
const char* const kDefaultDepsPath = "./env";

namespace {

std::string Trim(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
                     s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

/// `Path::parent`, falling back to "." as Rust's `unwrap_or_else` does.
std::string ParentDir(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return std::string(".");
    if (slash == 0) return std::string("/");
    return path.substr(0, slash);
}

/// `Path::join`, which drops the base when the appended part is absolute.
std::string Join(const std::string& base, const std::string& leaf) {
    if (leaf.empty()) return base;
    if (leaf[0] == '/') return leaf;
    if (base.empty()) return leaf;
    if (base[base.size() - 1] == '/') return base + leaf;
    return base + "/" + leaf;
}

core::Expected<std::size_t, std::string> ParseUsize(const std::string& flag,
                                                    const std::string& text) {
    if (text.empty()) {
        return core::make_unexpected(flag + ": expected a number");
    }
    std::size_t out = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return core::make_unexpected(flag + ": `" + text + "` is not a number");
        }
        out = out * 10 + static_cast<std::size_t>(text[i] - '0');
    }
    return out;
}

core::Expected<bool, std::string> ParseBool(const std::string& flag,
                                            const std::string& text) {
    if (text == "true" || text == "1") return true;
    if (text == "false" || text == "0") return false;
    return core::make_unexpected(flag + ": `" + text + "` is not a boolean");
}

/// Reads an optional unsigned TOML field, refusing a negative value.
///
/// A negative watermark would wrap when converted, so it is rejected where it
/// is read instead of becoming an enormous pool ceiling.
core::Expected<core::Optional<std::size_t>, std::string> TomlUsize(
    const core::TomlTable& table, const std::string& key) {
    const core::TomlValue* v = table.Find(key);
    if (v == 0) return core::Optional<std::size_t>();
    const core::Expected<long long, std::string> n = v->AsInteger();
    if (!n.ok()) return core::make_unexpected(key + ": " + n.error());
    if (n.value() < 0) return core::make_unexpected(key + ": must not be negative");
    return core::Optional<std::size_t>(static_cast<std::size_t>(n.value()));
}

core::Expected<core::Optional<bool>, std::string> TomlBool(const core::TomlTable& table,
                                                           const std::string& key) {
    const core::TomlValue* v = table.Find(key);
    if (v == 0) return core::Optional<bool>();
    const core::Expected<bool, std::string> b = v->AsBoolean();
    if (!b.ok()) return core::make_unexpected(key + ": " + b.error());
    return core::Optional<bool>(b.value());
}

core::Expected<core::Optional<std::string>, std::string> TomlString(
    const core::TomlTable& table, const std::string& key) {
    const core::TomlValue* v = table.Find(key);
    if (v == 0) return core::Optional<std::string>();
    const core::Expected<std::string, std::string> s = v->AsString();
    if (!s.ok()) return core::make_unexpected(key + ": " + s.error());
    return core::Optional<std::string>(s.value());
}

}  // namespace

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

std::string Cli::Usage() {
    return std::string(
        "Centralized ublk device manager daemon. Manages all ublk devices in a single "
        "process.\n"
        "\n"
        "Usage: agentenv-ublk-daemon [OPTIONS] --socket-path <PATH> "
        "--global-config <PATH> --resize-global-config <PATH>\n"
        "\n"
        "Options:\n"
        "      --socket-path <PATH>           Unix socket for control communication\n"
        "      --global-config <PATH>         overlaybd global config JSON\n"
        "      --resize-global-config <PATH>  overlaybd global config for overlaybd-resize\n"
        "      --config <PATH>                AgentENV TOML config; pool settings are read\n"
        "                                     from [pool.block]\n"
        "      --log-level <LEVEL>            off, error, warn, info, debug, trace "
        "[default: info]\n"
        "      --log-file <PATH>              log file; stderr when omitted\n"
        "      --metrics-listen-addr <ADDR>   Prometheus listen address; empty disables\n"
        "                                     [default: 0.0.0.0:9103]\n"
        "      --enable-pool                  enable the warm pool for overlaybd devices\n"
        "      --pool-low-watermark <N>       override the warm pool low watermark\n"
        "      --pool-high-watermark <N>      override the warm pool high watermark\n"
        "      --pool-startup-prewarm <BOOL>  override prewarm-after-first-image-use\n"
        "      --p2p-publish-url <URL>        endpoint used to publish overlaybd layers\n"
        "  -h, --help                         print this help\n");
}

core::Expected<Cli, std::string> Cli::Parse(int argc, char** argv) {
    Cli out;
    bool have_socket = false;
    bool have_global = false;
    bool have_resize_global = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i] == 0 ? "" : argv[i]);
        if (arg == "-h" || arg == "--help") {
            return core::make_unexpected(std::string("--help"));
        }

        // Every flag below takes a value. Reading it through one helper keeps
        // "flag present but value missing" from being read as an empty path,
        // which would otherwise reach the daemon as a socket at "".
        std::string name = arg;
        std::string value;
        bool have_value = false;
        const std::size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
            have_value = true;
        }

        if (name == "--enable-pool") {
            if (have_value) {
                const core::Expected<bool, std::string> b = ParseBool(name, value);
                if (!b.ok()) return core::make_unexpected(b.error());
                out.enable_pool = b.value();
            } else {
                out.enable_pool = true;
            }
            continue;
        }

        if (!have_value) {
            if (i + 1 >= argc) {
                return core::make_unexpected(name + ": missing value");
            }
            value = argv[++i] == 0 ? "" : argv[i];
        }

        if (name == "--socket-path") {
            out.socket_path = value;
            have_socket = true;
        } else if (name == "--global-config") {
            out.global_config = value;
            have_global = true;
        } else if (name == "--resize-global-config") {
            out.resize_global_config = value;
            have_resize_global = true;
        } else if (name == "--config") {
            out.config = core::Optional<std::string>(value);
        } else if (name == "--log-level") {
            out.log_level = value;
        } else if (name == "--log-file") {
            out.log_file = core::Optional<std::string>(value);
        } else if (name == "--metrics-listen-addr") {
            // Not defaulted away when empty: an empty value is how the flag
            // disables metrics, which is different from omitting it.
            out.metrics_listen_addr = value;
        } else if (name == "--pool-low-watermark") {
            const core::Expected<std::size_t, std::string> n = ParseUsize(name, value);
            if (!n.ok()) return core::make_unexpected(n.error());
            out.pool_low_watermark = core::Optional<std::size_t>(n.value());
        } else if (name == "--pool-high-watermark") {
            const core::Expected<std::size_t, std::string> n = ParseUsize(name, value);
            if (!n.ok()) return core::make_unexpected(n.error());
            out.pool_high_watermark = core::Optional<std::size_t>(n.value());
        } else if (name == "--pool-startup-prewarm") {
            const core::Expected<bool, std::string> b = ParseBool(name, value);
            if (!b.ok()) return core::make_unexpected(b.error());
            out.pool_startup_prewarm = core::Optional<bool>(b.value());
        } else if (name == "--p2p-publish-url") {
            out.p2p_publish_url = core::Optional<std::string>(value);
        } else {
            return core::make_unexpected(std::string("unknown argument: ") + name);
        }
    }

    // Required flags are checked together so the first run reports all of
    // them, rather than one per invocation.
    std::vector<std::string> missing;
    if (!have_socket) missing.push_back("--socket-path");
    if (!have_global) missing.push_back("--global-config");
    if (!have_resize_global) missing.push_back("--resize-global-config");
    if (!missing.empty()) {
        std::ostringstream o;
        o << "missing required argument(s):";
        for (std::size_t i = 0; i < missing.size(); ++i) o << " " << missing[i];
        return core::make_unexpected(o.str());
    }
    return out;
}

// ---------------------------------------------------------------------------
// TOML
// ---------------------------------------------------------------------------

core::Expected<core::Optional<DaemonTomlConfig>, std::string> LoadDaemonConfig(
    const core::Optional<std::string>& path) {
    if (!path.has_value()) return core::Optional<DaemonTomlConfig>();

    const core::Expected<core::TomlTable, std::string> table =
        core::TomlTable::ParseFile(*path);
    if (!table.ok()) {
        return core::make_unexpected(std::string("read daemon config ") + *path + ": " +
                                     table.error());
    }

    DaemonTomlConfig cfg;
    const core::Expected<core::Optional<std::string>, std::string> home =
        TomlString(table.value(), "home_path");
    if (!home.ok()) return core::make_unexpected(home.error());
    cfg.home_path = home.value();

    const core::Expected<core::Optional<std::string>, std::string> deps =
        TomlString(table.value(), "deps_path");
    if (!deps.ok()) return core::make_unexpected(deps.error());
    cfg.deps_path = deps.value();

    cfg.pool_present = table.value().HasTable("pool");
    const core::Expected<core::Optional<std::size_t>, std::string> low =
        TomlUsize(table.value(), "pool.low_watermark");
    if (!low.ok()) return core::make_unexpected(low.error());
    cfg.pool_low_watermark = low.value();

    const core::Expected<core::Optional<std::size_t>, std::string> high =
        TomlUsize(table.value(), "pool.high_watermark");
    if (!high.ok()) return core::make_unexpected(high.error());
    cfg.pool_high_watermark = high.value();

    cfg.pool_block_present = table.value().HasTable("pool.block");
    const core::Expected<core::Optional<bool>, std::string> enabled =
        TomlBool(table.value(), "pool.block.enabled");
    if (!enabled.ok()) return core::make_unexpected(enabled.error());
    cfg.pool_block_enabled = enabled.value();

    const core::Expected<core::Optional<bool>, std::string> prewarm =
        TomlBool(table.value(), "pool.block.startup_prewarm");
    if (!prewarm.ok()) return core::make_unexpected(prewarm.error());
    cfg.pool_block_startup_prewarm = prewarm.value();

    const core::Expected<core::Optional<std::size_t>, std::string> timeout =
        TomlUsize(table.value(), "ublk.overlaybd.resize_timeout_secs");
    if (!timeout.ok()) return core::make_unexpected(timeout.error());
    if (timeout.value().has_value()) {
        cfg.ublk_overlaybd_resize_timeout_secs =
            core::Optional<uint64_t>(static_cast<uint64_t>(*timeout.value()));
    }
    return core::Optional<DaemonTomlConfig>(cfg);
}

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------

warmpool::PoolConfig DefaultPoolConfig() {
    warmpool::PoolConfig c;
    c.low_watermark = 2;
    c.high_watermark = 64;
    // Never true for this daemon: it refills overlaybd devices inline from
    // acquire/release, because prewarming needs an async ublk control path and
    // the request's current overlaybd image. The generic synchronous
    // background worker would duplicate that work against a stale image.
    c.maintenance_enabled = false;
    c.startup_prewarm = true;
    return c;
}

warmpool::PoolConfig ApplyPoolOverrides(warmpool::PoolConfig config,
                                         const PoolConfigOverrides& overrides) {
    if (overrides.low_watermark.has_value()) config.low_watermark = *overrides.low_watermark;
    if (overrides.high_watermark.has_value()) config.high_watermark = *overrides.high_watermark;
    if (overrides.startup_prewarm.has_value()) {
        config.startup_prewarm = *overrides.startup_prewarm;
    }
    return config;
}

core::Expected<core::Optional<warmpool::PoolConfig>, std::string> LoadPoolConfig(
    const core::Optional<DaemonTomlConfig>& config, bool force_enable,
    const PoolConfigOverrides& overrides) {
    if (!config.has_value()) {
        // Rust `force_enable.then(|| ...)`: with no config at all, the flag is
        // the only thing that can turn the pool on.
        if (!force_enable) return core::Optional<warmpool::PoolConfig>();
        return core::Optional<warmpool::PoolConfig>(
            ApplyPoolOverrides(DefaultPoolConfig(), overrides));
    }

    // `enabled` comes from `[pool.block]` and falls back to the flag only when
    // the table does not say. An explicit `enabled = false` therefore beats
    // `--enable-pool`, which is what lets a host config switch the pool off
    // without changing the service's arguments.
    const bool enabled = config->pool_block_enabled.has_value()
                             ? *config->pool_block_enabled
                             : force_enable;
    if (!enabled) return core::Optional<warmpool::PoolConfig>();

    warmpool::PoolConfig out;
    out.low_watermark = overrides.low_watermark.has_value()
                            ? *overrides.low_watermark
                            : (config->pool_low_watermark.has_value()
                                   ? *config->pool_low_watermark
                                   : 2);
    out.high_watermark = overrides.high_watermark.has_value()
                             ? *overrides.high_watermark
                             : (config->pool_high_watermark.has_value()
                                    ? *config->pool_high_watermark
                                    : 64);
    out.maintenance_enabled = false;  // see DefaultPoolConfig
    out.startup_prewarm = overrides.startup_prewarm.has_value()
                              ? *overrides.startup_prewarm
                              : (config->pool_block_startup_prewarm.has_value()
                                     ? *config->pool_block_startup_prewarm
                                     : true);
    return core::Optional<warmpool::PoolConfig>(out);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

core::Optional<std::string> EnvPath(const char* name) {
    const char* raw = ::getenv(name);
    if (raw == 0) return core::Optional<std::string>();
    const std::string trimmed = Trim(std::string(raw));
    if (trimmed.empty()) return core::Optional<std::string>();
    return core::Optional<std::string>(trimmed);
}

std::string ResolveRelativeTo(const std::string& base_dir, const std::string& path) {
    if (!path.empty() && path[0] == '/') return path;
    return Join(base_dir, path);
}

std::string ResolveConfigPath(const std::string& home_path, const std::string& config_dir,
                              const std::string& raw) {
    std::string expanded = raw;
    const std::size_t at = expanded.find(kHomePathPlaceholder);
    if (at != std::string::npos) {
        // Replaces every occurrence, as Rust's `str::replace` does.
        const std::string placeholder(kHomePathPlaceholder);
        std::size_t pos = 0;
        std::string out;
        while (true) {
            const std::size_t hit = expanded.find(placeholder, pos);
            if (hit == std::string::npos) {
                out += expanded.substr(pos);
                break;
            }
            out += expanded.substr(pos, hit - pos);
            out += home_path;
            pos = hit + placeholder.size();
        }
        expanded = out;
    }
    return ResolveRelativeTo(config_dir, expanded);
}

core::Expected<core::Optional<ResizeToolSpec>, std::string> LoadResizeToolConfig(
    const core::Optional<std::string>& config_path,
    const core::Optional<DaemonTomlConfig>& config) {
    if (!config.has_value()) return core::Optional<ResizeToolSpec>();

    const std::string config_dir =
        config_path.has_value() ? ParentDir(*config_path) : std::string(".");

    // The environment wins over the file, and the file over the default. The
    // home path is resolved first because the deps path may name it through
    // the `$AENV_HOME` placeholder.
    const core::Optional<std::string> home_env = EnvPath("AENV_HOME_PATH");
    const std::string home_raw = home_env.has_value()
                                     ? *home_env
                                     : (config->home_path.has_value() ? *config->home_path
                                                                      : kDefaultHomePath);
    const std::string home_path = ResolveRelativeTo(config_dir, home_raw);

    const core::Optional<std::string> deps_env = EnvPath("AENV_DEPS_PATH");
    const std::string deps_raw = deps_env.has_value()
                                     ? *deps_env
                                     : (config->deps_path.has_value() ? *config->deps_path
                                                                      : kDefaultDepsPath);
    const std::string deps_path = ResolveConfigPath(home_path, config_dir, deps_raw);

    const uint64_t timeout = config->ublk_overlaybd_resize_timeout_secs.has_value()
                                 ? *config->ublk_overlaybd_resize_timeout_secs
                                 : kDefaultResizeTimeoutSecs;
    if (timeout == 0) {
        // A zero timeout would make every resize fail immediately, which looks
        // like a broken tool rather than a config mistake.
        return core::make_unexpected(
            std::string("invalid ublk.overlaybd config: resize_timeout_secs must be > 0"));
    }

    ResizeToolSpec spec;
    spec.binary = Join(deps_path, "overlaybd/bin/overlaybd-resize");
    spec.timeout_secs = timeout;
    return core::Optional<ResizeToolSpec>(spec);
}

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
