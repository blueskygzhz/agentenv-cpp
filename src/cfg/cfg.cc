// SPDX-License-Identifier: MIT
// Rust: src/cfg.rs
#include "agentenv/cfg.h"

#include <cstdlib>
#include <sstream>
#include <sys/stat.h>

#include <limits>

namespace agentenv {
namespace cfg {

const char* const kEnvConfigPath = "AENV_CONFIG_PATH";
const char* const kTestAccessTokenHashSeed = "agentenv-unit-test-access-token-seed";
const char* const kHomePathPlaceholder = "$AENV_HOME";
const char* const kRuntimePathPlaceholder = "$AENV_RUNTIME";

namespace {

const int64_t kI64Max = std::numeric_limits<int64_t>::max();
const int32_t kI32Max = std::numeric_limits<int32_t>::max();

std::string Trim(const std::string& raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && (raw[begin] == ' ' || raw[begin] == '\t' || raw[begin] == '\r' ||
                           raw[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (raw[end - 1] == ' ' || raw[end - 1] == '\t' || raw[end - 1] == '\r' ||
                           raw[end - 1] == '\n')) {
        --end;
    }
    return raw.substr(begin, end - begin);
}

std::string ReplaceAll(const std::string& text, const std::string& needle,
                       const std::string& replacement) {
    if (needle.empty()) return text;
    std::string out;
    std::size_t cursor = 0;
    while (true) {
        const std::size_t hit = text.find(needle, cursor);
        if (hit == std::string::npos) {
            out.append(text, cursor, std::string::npos);
            return out;
        }
        out.append(text, cursor, hit - cursor);
        out += replacement;
        cursor = hit + needle.size();
    }
}

bool FileExists(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0;
}

/// Realpath-based canonicalization; `false` when the path does not resolve.
/// Rust uses `std::fs::canonicalize(...).ok()`.
bool Canonicalize(const std::string& path, std::string* out) {
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (resolved == nullptr) return false;
    *out = resolved;
    ::free(resolved);
    return true;
}

core::Optional<std::string> EnvVar(const char* name) {
    const char* raw = ::getenv(name);
    if (raw == nullptr) return core::Optional<std::string>();
    return std::string(raw);
}

/// Rust `parse_trimmed_string` applied to an env var, treating blank as unset.
core::Optional<std::string> EnvTrimmedNonEmpty(const char* name) {
    core::Optional<std::string> raw = EnvVar(name);
    if (!raw.has_value()) return core::Optional<std::string>();
    const std::string trimmed = Trim(*raw);
    if (trimmed.empty()) return core::Optional<std::string>();
    return trimmed;
}

bool EnvBool(const char* name, bool* out) {
    core::Optional<std::string> raw = EnvTrimmedNonEmpty(name);
    if (!raw.has_value()) return false;
    if (*raw == "true" || *raw == "1") {
        *out = true;
        return true;
    }
    if (*raw == "false" || *raw == "0") {
        *out = false;
        return true;
    }
    return false;
}

bool EnvUint64(const char* name, uint64_t* out) {
    core::Optional<std::string> raw = EnvTrimmedNonEmpty(name);
    if (!raw.has_value()) return false;
    char* end = nullptr;
    const unsigned long long value = ::strtoull(raw->c_str(), &end, 10);
    if (end != raw->c_str() + raw->size()) return false;
    *out = static_cast<uint64_t>(value);
    return true;
}

/// Rust `confique::env::parse::list_by_comma`.
std::vector<std::string> SplitByComma(const std::string& raw) {
    std::vector<std::string> out;
    std::size_t cursor = 0;
    while (true) {
        const std::size_t comma = raw.find(',', cursor);
        const std::string piece =
            raw.substr(cursor, comma == std::string::npos ? std::string::npos : comma - cursor);
        out.push_back(piece);
        if (comma == std::string::npos) break;
        cursor = comma + 1;
    }
    return out;
}

/// Minimal SemVer check matching what `semver::Version::parse` accepts, plus
/// the upstream rejection of build metadata. Also rejects `../escape`, which is
/// the security-relevant case the Rust test pins.
bool ParseSemVerNoBuild(const std::string& raw, bool* has_build) {
    *has_build = false;
    if (raw.find('+') != std::string::npos) {
        // `1.2.3+rebuilt` parses as SemVer but carries build metadata.
        *has_build = true;
        const std::size_t plus = raw.find('+');
        std::string core_part = raw.substr(0, plus);
        bool ignored = false;
        return ParseSemVerNoBuild(core_part, &ignored);
    }

    // Split off an optional pre-release (`-alpha.1`).
    std::string core_part = raw;
    const std::size_t dash = raw.find('-');
    if (dash != std::string::npos) {
        core_part = raw.substr(0, dash);
        const std::string pre = raw.substr(dash + 1);
        if (pre.empty()) return false;
        for (std::size_t i = 0; i < pre.size(); ++i) {
            const char c = pre[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (!ok) return false;
        }
    }

    // Exactly major.minor.patch, all numeric and non-empty.
    int dots = 0;
    std::size_t cursor = 0;
    while (true) {
        const std::size_t dot = core_part.find('.', cursor);
        const std::string piece = core_part.substr(
            cursor, dot == std::string::npos ? std::string::npos : dot - cursor);
        if (piece.empty()) return false;
        for (std::size_t i = 0; i < piece.size(); ++i) {
            if (piece[i] < '0' || piece[i] > '9') return false;
        }
        if (dot == std::string::npos) break;
        ++dots;
        cursor = dot + 1;
    }
    return dots == 2;
}

}  // namespace

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

bool PathIsAbsolute(const std::string& path) { return !path.empty() && path[0] == '/'; }

std::string CollapseCurDirComponents(const std::string& path) {
    // Rust compares `Path`s by `components()`, and `Components` silently skips
    // `CurDir` (`.`) segments and repeated separators. So `/a/./env` and
    // `/a/env` are *equal* paths upstream, while plain string comparison in C++
    // would see them as different. Folding `.` here keeps the ported
    // `normalize`/`resolve_*` results byte-identical to what the Rust tests
    // assert against.
    //
    // `..` is deliberately preserved: `Components` yields `ParentDir` as a real
    // component, so `/a/../b` is NOT equal to `/b` upstream. Resolving `..`
    // is the separate job of `LexicallyNormalizePath`.
    if (path.empty()) return path;
    if (path.find('.') == std::string::npos && path.find("//") == std::string::npos) {
        return path;  // fast path: nothing to fold
    }

    const bool absolute = path[0] == '/';
    std::vector<std::string> kept;
    std::size_t cursor = 0;
    while (cursor <= path.size()) {
        const std::size_t slash = path.find('/', cursor);
        const std::string component =
            path.substr(cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
        if (!component.empty() && component != ".") kept.push_back(component);
        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }

    std::string out;
    if (absolute) out.push_back('/');
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i > 0) out.push_back('/');
        out += kept[i];
    }
    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

std::string PathJoin(const std::string& base, const std::string& leaf) {
    // Rust `Path::join` replaces the base entirely when the leaf is absolute.
    if (PathIsAbsolute(leaf)) return CollapseCurDirComponents(leaf);
    if (base.empty()) return CollapseCurDirComponents(leaf);
    if (leaf.empty()) return CollapseCurDirComponents(base);
    const std::string joined =
        base[base.size() - 1] == '/' ? base + leaf : base + "/" + leaf;
    return CollapseCurDirComponents(joined);
}

std::string PathParent(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string ResolveRelativeTo(const std::string& base_dir, const std::string& path) {
    if (PathIsAbsolute(path)) return CollapseCurDirComponents(path);
    return PathJoin(base_dir, path);
}

std::string ResolvePath(const std::string& home_path, const std::string& config_dir,
                        const std::string& raw) {
    std::string expanded = raw;
    if (raw.find(kHomePathPlaceholder) != std::string::npos) {
        expanded = ReplaceAll(raw, kHomePathPlaceholder, home_path);
    }
    return ResolveRelativeTo(config_dir, expanded);
}

std::string ResolveRuntimePath(const std::string& home_path, const std::string& runtime_path,
                               const std::string& config_dir, const std::string& raw) {
    std::string expanded = ReplaceAll(raw, kHomePathPlaceholder, home_path);
    expanded = ReplaceAll(expanded, kRuntimePathPlaceholder, runtime_path);
    return ResolveRelativeTo(config_dir, expanded);
}

void ResolveOptionalConfigPathOr(core::Optional<std::string>* path, const std::string& home_path,
                                 const std::string& config_dir,
                                 const std::string& default_relative_path) {
    if (path->has_value()) {
        *path = ResolvePath(home_path, config_dir, **path);
    } else {
        *path = PathJoin(home_path, default_relative_path);
    }
}

// ---------------------------------------------------------------------------
// SetupDependencyManifest
// ---------------------------------------------------------------------------

const ManifestDownload& ManifestVirtualizationDownloads::ForMode(
    core::VirtualizationMode mode) const {
    switch (mode) {
        case core::VirtualizationMode::Kvm:
            return kvm;
        case core::VirtualizationMode::Pvm:
            return pvm;
    }
    return kvm;
}

const SetupDependencyManifest& SetupDependencyManifest::Get() {
    // Rust embeds config/deps_manifest.toml with `include_str!` and parses it
    // into a `LazyLock`. The C++ port carries the same pinned versions inline;
    // regenerate alongside the upstream manifest.
    static SetupDependencyManifest* manifest = nullptr;
    if (manifest == nullptr) {
        manifest = new SetupDependencyManifest();
        manifest->firecracker.kvm.version = "1.13.1";
        manifest->firecracker.pvm.version = "1.13.1-pvm";
        manifest->kernel.kvm.version = "6.1.102";
        manifest->kernel.pvm.version = "6.1.102-pvm";
        manifest->tools.version = "0.1.0";
        manifest->overlaybd.version = "0.6.12";
        manifest->regclient.version = "0.9.4";
    }
    return *manifest;
}

std::string RegctlPath(const std::string& deps_path) {
    return PathJoin(PathJoin(PathJoin(deps_path, "regctl"),
                             SetupDependencyManifest::Get().regclient.version),
                    "regctl");
}

// ---------------------------------------------------------------------------
// Small per-struct behavior
// ---------------------------------------------------------------------------

std::string SandboxConfig::DebugString() const {
    // Rust hand-written Debug impl: the seed is never printed.
    std::ostringstream oss;
    oss << "SandboxConfig { access_token_hash_seed: ";
    if (access_token_hash_seed.has_value()) {
        oss << "Some(\"<redacted>\")";
    } else {
        oss << "None";
    }
    oss << " }";
    return oss.str();
}

void ClusterConfig::Normalize() {
    if (!scheduler_endpoint.has_value()) return;
    const std::string trimmed = Trim(*scheduler_endpoint);
    if (trimmed.empty()) {
        scheduler_endpoint.reset();
    } else {
        scheduler_endpoint = trimmed;
    }
}

core::Expected<core::Unit, std::string> SandboxProxyConfig::Normalize() {
    std::vector<std::string> normalized;
    normalized.reserve(domains.size());
    for (std::size_t i = 0; i < domains.size(); ++i) {
        std::string domain = Trim(domains[i]);
        // Rust `trim_end_matches('.')`.
        while (!domain.empty() && domain[domain.size() - 1] == '.') {
            domain.erase(domain.size() - 1);
        }
        if (domain.empty()) continue;

        core::Optional<std::string> valid = NormalizeDnsName(domain);
        if (!valid.has_value()) {
            std::ostringstream oss;
            oss << "sandbox_proxy.domains contains invalid domain \"" << domain << "\"";
            return core::make_unexpected(oss.str());
        }
        bool seen = false;
        for (std::size_t j = 0; j < normalized.size(); ++j) {
            if (normalized[j] == *valid) {
                seen = true;
                break;
            }
        }
        if (!seen) normalized.push_back(*valid);
    }
    // Keep user order: domains[0] is the advertised sandbox response domain.
    domains = normalized;
    return core::Unit();
}

core::Expected<core::Unit, std::string> PoolTomlConfig::Validate(
    const std::string& name, const warmpool::PoolConfig& pool) {
    if (pool.low_watermark > pool.high_watermark) {
        std::ostringstream oss;
        oss << "invalid " << name << " pool config: low_watermark (" << pool.low_watermark
            << ") must be <= high_watermark (" << pool.high_watermark << ")";
        return core::make_unexpected(oss.str());
    }
    return core::Unit();
}

// ---------------------------------------------------------------------------
// AppConfig: resolved accessors
// ---------------------------------------------------------------------------

const ManifestDownload& AppConfig::ManifestFirecracker() const {
    return SetupDependencyManifest::Get().firecracker.ForMode(virtualization_mode);
}

const ManifestDownload& AppConfig::ManifestKernel() const {
    return SetupDependencyManifest::Get().kernel.ForMode(virtualization_mode);
}

std::string AppConfig::ResolvedFirecrackerBinaryPath() const {
    if (firecracker.binary_path.has_value()) return *firecracker.binary_path;
    const std::string version = firecracker.version.has_value() ? *firecracker.version
                                                                : ManifestFirecracker().version;
    return PathJoin(PathJoin(PathJoin(deps_path, "firecracker"), version), "firecracker");
}

std::string AppConfig::ResolvedKernelImagePath() const {
    if (kernel.image_path.has_value()) return *kernel.image_path;
    const std::string version =
        kernel.version.has_value() ? *kernel.version : ManifestKernel().version;
    return PathJoin(PathJoin(PathJoin(deps_path, "kernel"), version), "vmlinux.bin");
}

std::string AppConfig::ResolvedToolsVersion() const {
    if (tools.version.has_value()) return *tools.version;
    return SetupDependencyManifest::Get().tools.version;
}

core::Expected<std::string, std::string> AppConfig::ResolvedToolsDrivePathForVersion(
    const std::string& version) const {
    bool has_build = false;
    if (!ParseSemVerNoBuild(version, &has_build)) {
        std::ostringstream oss;
        oss << "invalid tools drive version '" << version
            << "': expected SemVer without build metadata";
        return core::make_unexpected(oss.str());
    }
    if (has_build) {
        std::ostringstream oss;
        oss << "invalid tools drive version '" << version
            << "': build metadata is not supported";
        return core::make_unexpected(oss.str());
    }
    return PathJoin(PathJoin(PathJoin(deps_path, "tools"), version), "tools.ext4");
}

core::Expected<std::string, std::string> AppConfig::ResolvedToolsDrivePath() const {
    return ResolvedToolsDrivePathForVersion(ResolvedToolsVersion());
}

std::string AppConfig::ResolvedOverlaybdOciConverterId() const {
    const std::string version = overlaybd.has_value()
                                    ? overlaybd->version
                                    : SetupDependencyManifest::Get().overlaybd.version;
    return std::string("overlaybd-oci:") + version + ":agentenv-cache-v1";
}

std::string AppConfig::ResolvedOverlaybdConvertGlobalConfigPath() const {
    return PathJoin(PathParent(ublk.overlaybd.global_config_path),
                    "convert-overlaybd-global.json");
}

std::string AppConfig::ResolvedOverlaybdResizeGlobalConfigPath() const {
    return PathJoin(PathParent(ublk.overlaybd.global_config_path),
                    "resize-overlaybd-global.json");
}

core::Optional<std::string> AppConfig::ResolvedCpuTemplateHelper() const {
    const std::string version = firecracker.version.has_value() ? *firecracker.version
                                                                : ManifestFirecracker().version;
    const std::string path =
        PathJoin(PathJoin(PathJoin(deps_path, "firecracker"), version), "cpu-template-helper");
    if (!FileExists(path)) return core::Optional<std::string>();
    return path;
}

std::string AppConfig::ResolvedRegctlBinary() const { return RegctlPath(deps_path); }

ResolvedImageCacheConfig AppConfig::ImageCacheLayout() const { return image.cache.Layout(); }

warmpool::PoolConfig AppConfig::NetworkPoolConfig() const {
    warmpool::PoolConfig out;
    out.low_watermark = pool.low_watermark;
    out.high_watermark = pool.high_watermark;
    out.maintenance_enabled = pool.network.enabled && pool.network.maintenance_enabled;
    out.startup_prewarm = pool.network.startup_prewarm;
    return out;
}

core::Optional<warmpool::PoolConfig> AppConfig::BlockPoolConfig() const {
    if (!pool.block.enabled) return core::Optional<warmpool::PoolConfig>();
    warmpool::PoolConfig out;
    out.low_watermark = pool.low_watermark;
    out.high_watermark = pool.high_watermark;
    // The ublk daemon uses async request-time refill because the reusable
    // device shape is image/size dependent.
    out.maintenance_enabled = false;
    out.startup_prewarm = pool.block.startup_prewarm;
    return out;
}

core::Optional<ResolvedFirecrackerPoolConfig> AppConfig::FirecrackerPoolConfig() const {
    if (!pool.firecracker.enabled) return core::Optional<ResolvedFirecrackerPoolConfig>();
    ResolvedFirecrackerPoolConfig out;
    out.pool.low_watermark = pool.low_watermark;
    out.pool.high_watermark = pool.high_watermark;
    out.pool.maintenance_enabled = pool.firecracker.maintenance_enabled;
    out.pool.startup_prewarm = pool.firecracker.startup_prewarm;
    out.fill_concurrency = pool.firecracker.fill_concurrency;
    return out;
}

// ---------------------------------------------------------------------------
// AppConfig: normalize
// ---------------------------------------------------------------------------

void AppConfig::NormalizeDependencyOverridePaths(const std::string& config_dir) {
    const std::string home = home_path;
    if (firecracker.binary_path.has_value()) {
        firecracker.binary_path = ResolvePath(home, config_dir, *firecracker.binary_path);
    }
    if (kernel.image_path.has_value()) {
        kernel.image_path = ResolvePath(home, config_dir, *kernel.image_path);
    }
    if (tools.drive_path.has_value()) {
        tools.drive_path = ResolvePath(home, config_dir, *tools.drive_path);
    }
}

core::Expected<core::Unit, std::string> AppConfig::Normalize(const std::string& config_dir) {
    // Resolve config-owned filesystem paths relative to the active config file.
    home_path = ResolveRelativeTo(config_dir, home_path);
    runtime_path = ResolvePath(home_path, config_dir, runtime_path);
    deps_path = ResolvePath(home_path, config_dir, deps_path);

    NormalizeDependencyOverridePaths(config_dir);
    ResolveOptionalConfigPathOr(&firecracker.work_dir, home_path, config_dir, "firecracker-work");
    ResolveOptionalConfigPathOr(&firecracker.serial_dir, home_path, config_dir, "logs/serial");

    ResolveOptionalConfigPathOr(&ublk.daemon_binary_path, home_path, config_dir,
                                "ublk/uvm-ublk-daemon");
    ublk.daemon_socket_path =
        ResolveRuntimePath(home_path, runtime_path, config_dir, ublk.daemon_socket_path);
    ResolveOptionalConfigPathOr(&ublk.daemon_log_path, home_path, config_dir,
                                "logs/ublk-daemon.log");

    ublk.overlaybd.global_config_path =
        ResolvePath(home_path, config_dir, ublk.overlaybd.global_config_path);

    ImageConfig::Normalize(&image, config_dir, home_path);

    memory_snapshot.overlaybd_global_config_path =
        ResolvePath(home_path, config_dir, memory_snapshot.overlaybd_global_config_path);
    orchestrator.persisted_sandbox_store_path =
        ResolvePath(home_path, config_dir, orchestrator.persisted_sandbox_store_path);

    snapshot.local_cache_path = ResolvePath(home_path, config_dir, snapshot.local_cache_path);

    if (snapshot.repository_backend == SnapshotRepositoryBackendKind::PosixFs) {
        // Rust `get_or_insert_with(PosixFsBackendConfig::default)`.
        if (!backend.posix_fs.has_value()) backend.posix_fs = PosixFsBackendConfig();
        backend.posix_fs->snapshot_store =
            ResolvePath(home_path, config_dir, backend.posix_fs->snapshot_store);
    }

    p2p.store_dir = ResolvePath(home_path, config_dir, p2p.store_dir);

    // Dirty-page tracking is a KVM-only default. Disable it before validation
    // so an existing PVM configuration needs no new override.
    if (virtualization_mode == core::VirtualizationMode::Pvm) {
        memory_snapshot.track_dirty_pages = false;
    }

    cluster.Normalize();
    core::Expected<core::Unit, std::string> proxy = sandbox_proxy.Normalize();
    if (!proxy.has_value()) return core::make_unexpected(proxy.error());

    return core::Unit();
}

// ---------------------------------------------------------------------------
// AppConfig: validate
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> AppConfig::ValidateTemplateBuilder() const {
    const TemplateBuildConfig& builder = template_build;
    if (builder.max_concurrent_builds == 0) {
        return core::make_unexpected(
            std::string("template_build.max_concurrent_builds must be greater than 0"));
    }
    if (Trim(builder.builder_image).empty()) {
        return core::make_unexpected(
            std::string("template_build.builder_image must not be empty"));
    }
    if (builder.builder_cpu_count < 1 || builder.builder_cpu_count > 255) {
        return core::make_unexpected(
            std::string("template_build.builder_cpu_count must be between 1 and 255"));
    }
    if (builder.builder_memory_mb < 256 ||
        static_cast<int64_t>(builder.builder_memory_mb) > static_cast<int64_t>(kI32Max)) {
        std::ostringstream oss;
        oss << "template_build.builder_memory_mb must be between 256 and " << kI32Max << " MiB";
        return core::make_unexpected(oss.str());
    }
    if (builder.cache_size_mb < 1024) {
        return core::make_unexpected(
            std::string("template_build.cache_size_mb must be at least 1024 MiB"));
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> AppConfig::ValidateVolumeLimits() const {
    if (volume.max_size_mb == 0) {
        return core::make_unexpected(
            std::string("volume.max_size_mb must be greater than 0"));
    }
    const uint64_t max_size_limit = UINT64_MAX / (1024ULL * 1024ULL);
    if (volume.max_size_mb > max_size_limit) {
        std::ostringstream oss;
        oss << "volume.max_size_mb must be at most " << max_size_limit;
        return core::make_unexpected(oss.str());
    }
    if (volume.max_volume_count == 0 || volume.max_volume_count > kMaxVolumeMounts) {
        std::ostringstream oss;
        oss << "volume.max_volume_count must be between 1 and " << kMaxVolumeMounts;
        return core::make_unexpected(oss.str());
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> AppConfig::ValidateDiskRateLimit() const {
    const DiskRateLimitConfig& cfg = machine.disk_rate_limit;
    // A disabled section is ignored at runtime, so even internally inconsistent
    // or out-of-range values must not block startup.
    if (!cfg.enabled) return core::Unit();

    if (cfg.bandwidth_burst_bytes > 0 && cfg.bandwidth_bytes_per_sec == 0) {
        return core::make_unexpected(std::string(
            "machine.disk_rate_limit: bandwidth_burst_bytes is set but "
            "bandwidth_bytes_per_sec is 0; a burst requires a nonzero sustained limit"));
    }
    if (cfg.iops_burst > 0 && cfg.iops == 0) {
        return core::make_unexpected(std::string(
            "machine.disk_rate_limit: iops_burst is set but iops is 0; "
            "a burst requires a nonzero sustained limit"));
    }

    struct Field {
        const char* name;
        uint64_t value;
    };
    const Field fields[4] = {
        {"bandwidth_bytes_per_sec", cfg.bandwidth_bytes_per_sec},
        {"bandwidth_burst_bytes", cfg.bandwidth_burst_bytes},
        {"iops", cfg.iops},
        {"iops_burst", cfg.iops_burst},
    };
    for (int i = 0; i < 4; ++i) {
        if (fields[i].value > static_cast<uint64_t>(kI64Max)) {
            std::ostringstream oss;
            oss << "machine.disk_rate_limit." << fields[i].name << " (" << fields[i].value
                << ") exceeds the maximum supported value " << kI64Max;
            return core::make_unexpected(oss.str());
        }
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> AppConfig::ValidateMemorySnapshotOptions() const {
    if (!memory_snapshot.track_dirty_pages) return core::Unit();
    if (virtualization_mode == core::VirtualizationMode::Pvm) {
        return core::make_unexpected(std::string(
            "memory_snapshot.track_dirty_pages=true is disabled in PVM mode because this "
            "combination has not been tested"));
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> AppConfig::ValidateMemorySnapshotBackgroundDownload()
    const {
    const uint64_t kMaxBlockSize = 64ULL * 1024ULL * 1024ULL;
    const std::size_t kMaxConcurrency = 16;
    const uint64_t kMaxScratchBytes = 256ULL * 1024ULL * 1024ULL;
    const std::size_t kMaxInflightBlocks = 128;
    const uint64_t kMaxGlobalScratchBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

    const MemorySnapshotBackgroundDownloadConfig& cfg = memory_snapshot.background_download;

    if (cfg.concurrency == 0) {
        return core::make_unexpected(
            std::string("memory_snapshot.background_download.concurrency must be > 0"));
    }
    if (cfg.concurrency > kMaxConcurrency) {
        std::ostringstream oss;
        oss << "memory_snapshot.background_download.concurrency must be <= " << kMaxConcurrency;
        return core::make_unexpected(oss.str());
    }
    if (static_cast<uint64_t>(cfg.block_size) > kMaxBlockSize) {
        std::ostringstream oss;
        oss << "memory_snapshot.background_download.block_size must be <= " << kMaxBlockSize;
        return core::make_unexpected(oss.str());
    }
    if (static_cast<uint64_t>(cfg.block_size) * static_cast<uint64_t>(cfg.concurrency) >
        kMaxScratchBytes) {
        std::ostringstream oss;
        oss << "memory_snapshot.background_download block_size * concurrency must be <= "
            << kMaxScratchBytes << " bytes";
        return core::make_unexpected(oss.str());
    }
    if (cfg.max_inflight_blocks == 0) {
        return core::make_unexpected(
            std::string("memory_snapshot.background_download.max_inflight_blocks must be > 0"));
    }
    if (cfg.max_inflight_blocks > kMaxInflightBlocks) {
        std::ostringstream oss;
        oss << "memory_snapshot.background_download.max_inflight_blocks must be <= "
            << kMaxInflightBlocks;
        return core::make_unexpected(oss.str());
    }

    // Backend-wide scratch budget: every in-flight chunk may allocate one
    // block_size buffer, so the global product must stay bounded too.
    const uint64_t block_size = static_cast<uint64_t>(cfg.block_size);
    const uint64_t inflight = static_cast<uint64_t>(cfg.max_inflight_blocks);
    if (block_size != 0 && inflight > UINT64_MAX / block_size) {
        return core::make_unexpected(
            std::string("memory_snapshot.background_download scratch overflow"));
    }
    const uint64_t global_scratch = block_size * inflight;
    if (global_scratch > kMaxGlobalScratchBytes) {
        std::ostringstream oss;
        oss << "memory_snapshot.background_download block_size * max_inflight_blocks must be <= "
            << kMaxGlobalScratchBytes << " bytes";
        return core::make_unexpected(oss.str());
    }

    if (cfg.delay < 0) {
        return core::make_unexpected(
            std::string("memory_snapshot.background_download.delay must be >= 0"));
    }
    if (cfg.try_cnt < 1) {
        return core::make_unexpected(
            std::string("memory_snapshot.background_download.try_cnt must be >= 1"));
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> AppConfig::ValidateOverlaybdGlobalConfigPaths() const {
    const std::string names[4] = {
        "ublk.overlaybd.global_config_path",
        "memory_snapshot.overlaybd_global_config_path",
        "derived convert overlaybd global config path",
        "derived resize overlaybd global config path",
    };
    const std::string raw[4] = {
        ublk.overlaybd.global_config_path,
        memory_snapshot.overlaybd_global_config_path,
        ResolvedOverlaybdConvertGlobalConfigPath(),
        ResolvedOverlaybdResizeGlobalConfigPath(),
    };

    std::string normalized[4];
    std::string canonical[4];
    bool has_canonical[4];
    for (int i = 0; i < 4; ++i) {
        normalized[i] = LexicallyNormalizePath(raw[i]);
        has_canonical[i] = Canonicalize(normalized[i], &canonical[i]);
    }

    for (int left = 0; left < 4; ++left) {
        for (int right = left + 1; right < 4; ++right) {
            const bool lexical_alias = normalized[left] == normalized[right];
            const bool canonical_alias = has_canonical[left] && has_canonical[right] &&
                                         canonical[left] == canonical[right];
            if (lexical_alias || canonical_alias) {
                std::ostringstream oss;
                oss << names[left] << " (\"" << normalized[left] << "\") and " << names[right]
                    << " (\"" << normalized[right] << "\") must be different";
                return core::make_unexpected(oss.str());
            }
        }
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> AppConfig::ValidatePoolConfig() const {
    const warmpool::PoolConfig network_pool = NetworkPoolConfig();
    if (network_pool.maintenance_enabled) {
        core::Expected<core::Unit, std::string> result =
            PoolTomlConfig::Validate("network", network_pool);
        if (!result.has_value()) return core::make_unexpected(result.error());
    }
    core::Optional<warmpool::PoolConfig> block = BlockPoolConfig();
    if (block.has_value()) {
        core::Expected<core::Unit, std::string> result = PoolTomlConfig::Validate("block", *block);
        if (!result.has_value()) return core::make_unexpected(result.error());
    }
    core::Optional<ResolvedFirecrackerPoolConfig> fc = FirecrackerPoolConfig();
    if (fc.has_value()) {
        core::Expected<core::Unit, std::string> result =
            PoolTomlConfig::Validate("firecracker", fc->pool);
        if (!result.has_value()) return core::make_unexpected(result.error());
        if (fc->fill_concurrency == 0) {
            return core::make_unexpected(
                std::string("invalid firecracker pool config: fill_concurrency must be > 0"));
        }
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> AppConfig::Validate() const {
    // Order matches Rust `AppConfig::validate` so the first error a misconfigured
    // node sees is identical.
    core::Expected<core::Unit, std::string> step = ValidatePoolConfig();
    if (!step.has_value()) return step;

    step = image.cache.gc.Validate();
    if (!step.has_value()) return step;

    step = NetworkConfig::Validate(network);
    if (!step.has_value()) return step;

    if (ublk.overlaybd.resize_timeout_secs == 0) {
        return core::make_unexpected(
            std::string("invalid ublk.overlaybd config: resize_timeout_secs must be > 0"));
    }

    step = ValidateMemorySnapshotOptions();
    if (!step.has_value()) return step;

    if (ublk.overlaybd.remote_io_workers == 0) {
        return core::make_unexpected(
            std::string("invalid ublk.overlaybd config: remote_io_workers must be > 0"));
    }

    step = ValidateMemorySnapshotBackgroundDownload();
    if (!step.has_value()) return step;
    step = ValidateOverlaybdGlobalConfigPaths();
    if (!step.has_value()) return step;
    step = ValidateDiskRateLimit();
    if (!step.has_value()) return step;
    step = ValidateVolumeLimits();
    if (!step.has_value()) return step;
    step = ValidateTemplateBuilder();
    if (!step.has_value()) return step;

    return core::Unit();
}

// ---------------------------------------------------------------------------
// AppConfig: env + TOML layering
// ---------------------------------------------------------------------------

void AppConfig::ApplyEnvOverrides() {
    core::Optional<std::string> value;

    // Rust `parse_env = parse_required_path` trims but keeps the value.
    value = EnvTrimmedNonEmpty("AENV_HOME_PATH");
    if (value.has_value()) home_path = *value;
    value = EnvTrimmedNonEmpty("AENV_RUNTIME_PATH");
    if (value.has_value()) runtime_path = *value;
    value = EnvTrimmedNonEmpty("AENV_DEPS_PATH");
    if (value.has_value()) deps_path = *value;

    value = EnvTrimmedNonEmpty("AENV_VIRTUALIZATION_MODE");
    if (value.has_value()) {
        core::Expected<core::VirtualizationMode, std::string> mode =
            core::VirtualizationModeParse(*value);
        // An invalid value is ignored here; `Validate` is not the right place
        // either, so the file/default value simply wins.
        if (mode.has_value()) virtualization_mode = mode.value();
    }

    value = EnvTrimmedNonEmpty("AENV_FIRECRACKER_WORK_DIR");
    if (value.has_value()) firecracker.work_dir = *value;
    value = EnvTrimmedNonEmpty("AENV_FIRECRACKER_SERIAL_DIR");
    if (value.has_value()) firecracker.serial_dir = *value;

    value = EnvTrimmedNonEmpty("AENV_SNAPSHOT_STORE");
    if (value.has_value()) {
        if (!backend.posix_fs.has_value()) backend.posix_fs = PosixFsBackendConfig();
        backend.posix_fs->snapshot_store = *value;
    }

    value = EnvTrimmedNonEmpty("AENV_SANDBOX_ACCESS_TOKEN_HASH_SEED");
    if (value.has_value()) sandbox.access_token_hash_seed = *value;

    value = EnvTrimmedNonEmpty("AENV_SANDBOX_PROXY_DOMAINS");
    if (value.has_value()) sandbox_proxy.domains = SplitByComma(*value);

    value = EnvTrimmedNonEmpty("AENV_SNAPSHOT_LOCAL_CACHE_PATH");
    if (value.has_value()) snapshot.local_cache_path = *value;

    value = EnvTrimmedNonEmpty("AENV_UBLK_DAEMON_BINARY_PATH");
    if (value.has_value()) ublk.daemon_binary_path = *value;
    value = EnvTrimmedNonEmpty("AENV_UBLK_DAEMON_METRICS_LISTEN_ADDR");
    if (value.has_value()) ublk.daemon_metrics_listen_addr = *value;

    bool flag = false;
    if (EnvBool("AGENTENV_MEMORY_SNAPSHOT_TRACK_DIRTY_PAGES", &flag)) {
        memory_snapshot.track_dirty_pages = flag;
    }
    if (EnvBool("AENV_OBSERVABILITY_SCHEDULER_REPORT_ENABLED", &flag)) {
        observability.scheduler_report.enabled = flag;
    }
    uint64_t number = 0;
    if (EnvUint64("AENV_OBSERVABILITY_REPORT_INTERVAL_SECS", &number)) {
        observability.scheduler_report.interval_secs = number;
    }

    value = EnvTrimmedNonEmpty("AENV_OBSERVABILITY_SCHEDULER_ENDPOINT");
    if (value.has_value()) cluster.scheduler_endpoint = *value;

    value = EnvTrimmedNonEmpty("AENV_NODE_ID");
    if (value.has_value()) node_identity.node_id = *value;
    value = EnvTrimmedNonEmpty("AENV_CLUSTER_ID");
    if (value.has_value()) node_identity.cluster_id = *value;
    value = EnvTrimmedNonEmpty("AENV_SERVICE_INSTANCE_ID");
    if (value.has_value()) node_identity.service_instance_id = *value;

    value = EnvTrimmedNonEmpty("AENV_PERSISTED_SANDBOX_STORE_PATH");
    if (value.has_value()) orchestrator.persisted_sandbox_store_path = *value;

    value = EnvTrimmedNonEmpty("AENV_CUSTOM_EXTENSION_URL");
    if (value.has_value()) custom_extension.url = *value;
}

core::Expected<core::Unit, std::string> AppConfig::LoadFrom(const core::TomlTable& table) {
#define AENV_STR(KEY, FIELD)                                                    \
    do {                                                                        \
        const core::TomlValue* v = table.Find(KEY);                             \
        if (v != nullptr) {                                                     \
            core::Expected<std::string, std::string> p = v->AsString();         \
            if (!p.has_value())                                                 \
                return core::make_unexpected(std::string(KEY) + ": " + p.error()); \
            (FIELD) = p.value();                                                \
        }                                                                       \
    } while (false)

#define AENV_OPT_STR(KEY, FIELD)                                                \
    do {                                                                        \
        const core::TomlValue* v = table.Find(KEY);                             \
        if (v != nullptr) {                                                     \
            core::Expected<std::string, std::string> p = v->AsString();         \
            if (!p.has_value())                                                 \
                return core::make_unexpected(std::string(KEY) + ": " + p.error()); \
            (FIELD) = p.value();                                                \
        }                                                                       \
    } while (false)

#define AENV_BOOL(KEY, FIELD)                                                   \
    do {                                                                        \
        const core::TomlValue* v = table.Find(KEY);                             \
        if (v != nullptr) {                                                     \
            core::Expected<bool, std::string> p = v->AsBoolean();               \
            if (!p.has_value())                                                 \
                return core::make_unexpected(std::string(KEY) + ": " + p.error()); \
            (FIELD) = p.value();                                                \
        }                                                                       \
    } while (false)

#define AENV_NUM(KEY, FIELD, TYPE)                                              \
    do {                                                                        \
        const core::TomlValue* v = table.Find(KEY);                             \
        if (v != nullptr) {                                                     \
            core::Expected<long long, std::string> p = v->AsInteger();          \
            if (!p.has_value())                                                 \
                return core::make_unexpected(std::string(KEY) + ": " + p.error()); \
            (FIELD) = static_cast<TYPE>(p.value());                             \
        }                                                                       \
    } while (false)

#define AENV_UNUM(KEY, FIELD, TYPE)                                             \
    do {                                                                        \
        const core::TomlValue* v = table.Find(KEY);                             \
        if (v != nullptr) {                                                     \
            core::Expected<long long, std::string> p = v->AsInteger();          \
            if (!p.has_value())                                                 \
                return core::make_unexpected(std::string(KEY) + ": " + p.error()); \
            if (p.value() < 0)                                                  \
                return core::make_unexpected(std::string(KEY) +                 \
                                             " must not be negative");          \
            (FIELD) = static_cast<TYPE>(p.value());                             \
        }                                                                       \
    } while (false)

    AENV_STR("home_path", home_path);
    AENV_STR("runtime_path", runtime_path);
    AENV_STR("deps_path", deps_path);
    {
        const core::TomlValue* v = table.Find("virtualization_mode");
        if (v != nullptr) {
            core::Expected<std::string, std::string> raw = v->AsString();
            if (!raw.has_value()) {
                return core::make_unexpected(std::string("virtualization_mode: ") + raw.error());
            }
            core::Expected<core::VirtualizationMode, std::string> mode =
                core::VirtualizationModeParse(raw.value());
            if (!mode.has_value()) return core::make_unexpected(mode.error());
            virtualization_mode = mode.value();
        }
    }

    // [firecracker]
    AENV_OPT_STR("firecracker.binary_path", firecracker.binary_path);
    AENV_OPT_STR("firecracker.boot_args", firecracker.boot_args);
    {
        const core::TomlValue* v = table.Find("firecracker.allowed_extra_boot_args_prefixes");
        if (v != nullptr) {
            core::Expected<std::vector<std::string>, std::string> p = v->AsStringArray();
            if (!p.has_value()) {
                return core::make_unexpected(
                    std::string("firecracker.allowed_extra_boot_args_prefixes: ") + p.error());
            }
            firecracker.allowed_extra_boot_args_prefixes = p.value();
        }
    }
    AENV_UNUM("firecracker.socket_timeout_secs", firecracker.socket_timeout_secs, uint64_t);
    AENV_UNUM("firecracker.socket_poll_ms", firecracker.socket_poll_ms, uint64_t);
    AENV_OPT_STR("firecracker.version", firecracker.version);
    AENV_OPT_STR("firecracker.url", firecracker.url);
    AENV_OPT_STR("firecracker.work_dir", firecracker.work_dir);
    AENV_OPT_STR("firecracker.serial_dir", firecracker.serial_dir);
    AENV_OPT_STR("firecracker.log_level", firecracker.log_level);

    // [kernel] / [tools] / [overlaybd]
    AENV_OPT_STR("kernel.image_path", kernel.image_path);
    AENV_OPT_STR("kernel.version", kernel.version);
    AENV_OPT_STR("kernel.url", kernel.url);
    AENV_OPT_STR("tools.drive_path", tools.drive_path);
    AENV_OPT_STR("tools.version", tools.version);
    AENV_OPT_STR("tools.url", tools.url);
    AENV_UNUM("tools.control_plane_port", tools.control_plane_port, uint16_t);
    {
        const core::TomlValue* version = table.Find("overlaybd.version");
        if (version != nullptr) {
            core::Expected<std::string, std::string> p = version->AsString();
            if (!p.has_value()) {
                return core::make_unexpected(std::string("overlaybd.version: ") + p.error());
            }
            OverlaybdDependencyConfig dep;
            dep.version = p.value();
            const core::TomlValue* url = table.Find("overlaybd.url");
            if (url != nullptr) {
                core::Expected<std::string, std::string> u = url->AsString();
                if (!u.has_value()) {
                    return core::make_unexpected(std::string("overlaybd.url: ") + u.error());
                }
                dep.url = u.value();
            }
            const core::TomlValue* package_url = table.Find("overlaybd.package_url");
            if (package_url != nullptr) {
                core::Expected<std::string, std::string> u = package_url->AsString();
                if (!u.has_value()) {
                    return core::make_unexpected(std::string("overlaybd.package_url: ") +
                                                 u.error());
                }
                dep.package_url = u.value();
            }
            overlaybd = dep;
        }
    }

    // [machine]
    AENV_UNUM("machine.vcpu_count", machine.vcpu_count, uint32_t);
    AENV_UNUM("machine.mem_size_mib", machine.mem_size_mib, uint32_t);
    AENV_BOOL("machine.disk_rate_limit.enabled", machine.disk_rate_limit.enabled);
    AENV_UNUM("machine.disk_rate_limit.bandwidth_bytes_per_sec",
              machine.disk_rate_limit.bandwidth_bytes_per_sec, uint64_t);
    AENV_UNUM("machine.disk_rate_limit.bandwidth_burst_bytes",
              machine.disk_rate_limit.bandwidth_burst_bytes, uint64_t);
    AENV_UNUM("machine.disk_rate_limit.iops", machine.disk_rate_limit.iops, uint64_t);
    AENV_UNUM("machine.disk_rate_limit.iops_burst", machine.disk_rate_limit.iops_burst, uint64_t);

    // [backend]
    {
        const core::TomlValue* v = table.Find("backend.posix_fs.snapshot_store");
        if (v != nullptr || table.HasTable("backend.posix_fs")) {
            if (!backend.posix_fs.has_value()) backend.posix_fs = PosixFsBackendConfig();
            if (v != nullptr) {
                core::Expected<std::string, std::string> p = v->AsString();
                if (!p.has_value()) {
                    return core::make_unexpected(
                        std::string("backend.posix_fs.snapshot_store: ") + p.error());
                }
                backend.posix_fs->snapshot_store = p.value();
            }
        }
    }
    if (table.HasTable("backend.oss")) {
        OssBackendConfig oss;
        AENV_STR("backend.oss.endpoint", oss.endpoint);
        AENV_STR("backend.oss.bucket", oss.bucket);
        AENV_OPT_STR("backend.oss.prefix", oss.prefix);
        // Rust serde accepts three spellings for these two keys.
        AENV_OPT_STR("backend.oss.credential_process", oss.credential_process);
        AENV_OPT_STR("backend.oss.credentialProcess", oss.credential_process);
        AENV_OPT_STR("backend.oss.credential-process", oss.credential_process);
        AENV_OPT_STR("backend.oss.access_key_id", oss.access_key_id);
        AENV_OPT_STR("backend.oss.access_key_secret", oss.access_key_secret);
        AENV_OPT_STR("backend.oss.security_token", oss.security_token);
        AENV_OPT_STR("backend.oss.region", oss.region);
        {
            const char* keys[3] = {"backend.oss.addressing_style", "backend.oss.addressingStyle",
                                   "backend.oss.addressing-style"};
            for (int i = 0; i < 3; ++i) {
                const core::TomlValue* v = table.Find(keys[i]);
                if (v == nullptr) continue;
                core::Expected<std::string, std::string> p = v->AsString();
                if (!p.has_value()) {
                    return core::make_unexpected(std::string(keys[i]) + ": " + p.error());
                }
                if (p.value() == "path") {
                    oss.addressing_style = OssAddressingStyle::Path;
                } else if (p.value() == "virtual") {
                    oss.addressing_style = OssAddressingStyle::Virtual;
                } else {
                    return core::make_unexpected(std::string(keys[i]) +
                                                 ": expected \"path\" or \"virtual\"");
                }
            }
        }
        AENV_UNUM("backend.oss.cache_max_size_gb", oss.cache_max_size_gb, uint64_t);
        backend.oss = oss;
    }

    // [envd] / [sandbox] / [volume]
    AENV_STR("envd.version", envd.version);
    AENV_UNUM("envd.init_timeout_secs", envd.init_timeout_secs, uint64_t);
    AENV_UNUM("envd.poll_ms", envd.poll_ms, uint64_t);
    AENV_OPT_STR("sandbox.access_token_hash_seed", sandbox.access_token_hash_seed);
    AENV_UNUM("volume.max_size_mb", volume.max_size_mb, uint64_t);
    AENV_UNUM("volume.max_volume_count", volume.max_volume_count, std::size_t);

    // [orchestrator]
    AENV_UNUM("orchestrator.metrics_interval_secs", orchestrator.metrics_interval_secs, uint64_t);
    AENV_UNUM("orchestrator.metrics_retention_secs", orchestrator.metrics_retention_secs,
              uint64_t);
    AENV_UNUM("orchestrator.auto_evict_interval_ms", orchestrator.auto_evict_interval_ms,
              uint64_t);
    AENV_UNUM("orchestrator.default_sandbox_timeout_secs",
              orchestrator.default_sandbox_timeout_secs, uint64_t);
    AENV_UNUM("orchestrator.auto_resume_min_sandbox_timeout_secs",
              orchestrator.auto_resume_min_sandbox_timeout_secs, uint64_t);
    AENV_STR("orchestrator.persisted_sandbox_store_path",
             orchestrator.persisted_sandbox_store_path);

    // [snapshot]
    AENV_STR("snapshot.local_cache_path", snapshot.local_cache_path);
    {
        const core::TomlValue* v = table.Find("snapshot.repository_backend");
        if (v != nullptr) {
            core::Expected<std::string, std::string> p = v->AsString();
            if (!p.has_value()) {
                return core::make_unexpected(std::string("snapshot.repository_backend: ") +
                                             p.error());
            }
            if (p.value() == "posix_fs") {
                snapshot.repository_backend = SnapshotRepositoryBackendKind::PosixFs;
            } else if (p.value() == "oss") {
                snapshot.repository_backend = SnapshotRepositoryBackendKind::Oss;
            } else {
                return core::make_unexpected(
                    std::string("snapshot.repository_backend: expected \"posix_fs\" or \"oss\""));
            }
        }
    }
    AENV_BOOL("snapshot.p2p_enabled", snapshot.p2p_enabled);
    AENV_BOOL("snapshot.image_publish.enabled", snapshot.image_publish.enabled);
    AENV_BOOL("snapshot.publish_compression.enabled", snapshot.publish_compression.enabled);
    {
        const core::TomlValue* v = table.Find("snapshot.publish_compression.algorithm");
        if (v != nullptr) {
            core::Expected<std::string, std::string> p = v->AsString();
            if (!p.has_value()) {
                return core::make_unexpected(
                    std::string("snapshot.publish_compression.algorithm: ") + p.error());
            }
            if (p.value() == "lz4") {
                snapshot.publish_compression.algorithm = OverlaybdCompressionAlgorithm::Lz4;
            } else if (p.value() == "zstd") {
                snapshot.publish_compression.algorithm = OverlaybdCompressionAlgorithm::Zstd;
            } else {
                return core::make_unexpected(std::string(
                    "snapshot.publish_compression.algorithm: expected \"lz4\" or \"zstd\""));
            }
        }
    }
    AENV_UNUM("snapshot.publish_compression.workers", snapshot.publish_compression.workers,
              std::size_t);
    AENV_BOOL("snapshot.memory_startup_pack.enabled", snapshot.memory_startup_pack.enabled);
    AENV_UNUM("snapshot.memory_startup_pack.record_min_window_ms",
              snapshot.memory_startup_pack.record_min_window_ms, uint64_t);
    AENV_UNUM("snapshot.memory_startup_pack.record_quiet_ms",
              snapshot.memory_startup_pack.record_quiet_ms, uint64_t);
    AENV_UNUM("snapshot.memory_startup_pack.record_max_window_ms",
              snapshot.memory_startup_pack.record_max_window_ms, uint64_t);
    AENV_UNUM("snapshot.memory_startup_pack.record_budget_secs",
              snapshot.memory_startup_pack.record_budget_secs, uint64_t);
    AENV_UNUM("snapshot.memory_startup_pack.max_pack_bytes",
              snapshot.memory_startup_pack.max_pack_bytes, uint64_t);
    AENV_BOOL("snapshot.memory_startup_pack.consume_enabled",
              snapshot.memory_startup_pack.consume_enabled);
    AENV_UNUM("snapshot.memory_startup_pack.consume_timeout_secs",
              snapshot.memory_startup_pack.consume_timeout_secs, uint64_t);

    // [ublk]
    AENV_OPT_STR("ublk.daemon_binary_path", ublk.daemon_binary_path);
    AENV_STR("ublk.daemon_socket_path", ublk.daemon_socket_path);
    AENV_OPT_STR("ublk.daemon_log_path", ublk.daemon_log_path);
    AENV_STR("ublk.daemon_metrics_listen_addr", ublk.daemon_metrics_listen_addr);
    AENV_STR("ublk.overlaybd.global_config_path", ublk.overlaybd.global_config_path);
    AENV_BOOL("ublk.overlaybd.read_only", ublk.overlaybd.read_only);
    {
        const core::TomlValue* v = table.Find("ublk.overlaybd.runtime_upper_mode");
        if (v != nullptr) {
            core::Expected<std::string, std::string> p = v->AsString();
            if (!p.has_value()) {
                return core::make_unexpected(std::string("ublk.overlaybd.runtime_upper_mode: ") +
                                             p.error());
            }
            if (p.value() == "sparse") {
                ublk.overlaybd.runtime_upper_mode = storage::overlaybd::UpperMode::Sparse;
            } else if (p.value() == "logStructured") {
                ublk.overlaybd.runtime_upper_mode = storage::overlaybd::UpperMode::LogStructured;
            } else if (p.value() == "hybridLogStructured") {
                ublk.overlaybd.runtime_upper_mode = storage::overlaybd::UpperMode::HybridLogStructured;
            } else {
                return core::make_unexpected(
                    std::string("ublk.overlaybd.runtime_upper_mode: unsupported value \"") +
                    p.value() + "\"");
            }
        }
    }
    AENV_BOOL("ublk.overlaybd.allow_shrink", ublk.overlaybd.allow_shrink);
    AENV_UNUM("ublk.overlaybd.resize_timeout_secs", ublk.overlaybd.resize_timeout_secs, uint64_t);
    AENV_UNUM("ublk.overlaybd.remote_io_workers", ublk.overlaybd.remote_io_workers, uint64_t);
    AENV_BOOL("ublk.overlaybd.download_enable", ublk.overlaybd.download_enable);
    AENV_UNUM("ublk.overlaybd.p2p_lookup_timeout_ms", ublk.overlaybd.p2p_lookup_timeout_ms,
              uint64_t);
    AENV_UNUM("ublk.overlaybd.p2p_fetch_range_timeout_ms",
              ublk.overlaybd.p2p_fetch_range_timeout_ms, uint64_t);

    // [observability] / [cluster] / [node_identity]
    AENV_BOOL("observability.enabled", observability.enabled);
    AENV_BOOL("observability.scheduler_report.enabled", observability.scheduler_report.enabled);
    AENV_UNUM("observability.scheduler_report.interval_secs",
              observability.scheduler_report.interval_secs, uint64_t);
    AENV_OPT_STR("cluster.scheduler_endpoint", cluster.scheduler_endpoint);
    AENV_OPT_STR("node_identity.node_id", node_identity.node_id);
    AENV_OPT_STR("node_identity.cluster_id", node_identity.cluster_id);
    AENV_OPT_STR("node_identity.service_instance_id", node_identity.service_instance_id);

    // [template_build]
    AENV_UNUM("template_build.max_concurrent_builds", template_build.max_concurrent_builds,
              std::size_t);
    AENV_STR("template_build.builder_image", template_build.builder_image);
    AENV_UNUM("template_build.builder_cpu_count", template_build.builder_cpu_count, uint32_t);
    AENV_UNUM("template_build.builder_memory_mb", template_build.builder_memory_mb, uint32_t);
    AENV_UNUM("template_build.cache_size_mb", template_build.cache_size_mb, uint64_t);

    // [memory_snapshot]
    AENV_STR("memory_snapshot.overlaybd_global_config_path",
             memory_snapshot.overlaybd_global_config_path);
    AENV_BOOL("memory_snapshot.track_dirty_pages", memory_snapshot.track_dirty_pages);
    AENV_BOOL("memory_snapshot.background_download.enable",
              memory_snapshot.background_download.enable);
    AENV_NUM("memory_snapshot.background_download.delay", memory_snapshot.background_download.delay,
             int32_t);
    AENV_NUM("memory_snapshot.background_download.delay_extra",
             memory_snapshot.background_download.delay_extra, int32_t);
    AENV_NUM("memory_snapshot.background_download.try_cnt",
             memory_snapshot.background_download.try_cnt, int32_t);
    AENV_UNUM("memory_snapshot.background_download.block_size",
              memory_snapshot.background_download.block_size, uint32_t);
    AENV_UNUM("memory_snapshot.background_download.concurrency",
              memory_snapshot.background_download.concurrency, std::size_t);
    AENV_UNUM("memory_snapshot.background_download.max_inflight_blocks",
              memory_snapshot.background_download.max_inflight_blocks, std::size_t);

    // [pool]
    AENV_UNUM("pool.low_watermark", pool.low_watermark, std::size_t);
    AENV_UNUM("pool.high_watermark", pool.high_watermark, std::size_t);
    AENV_BOOL("pool.network.enabled", pool.network.enabled);
    AENV_BOOL("pool.network.maintenance_enabled", pool.network.maintenance_enabled);
    AENV_BOOL("pool.network.startup_prewarm", pool.network.startup_prewarm);
    AENV_BOOL("pool.block.enabled", pool.block.enabled);
    AENV_BOOL("pool.block.maintenance_enabled", pool.block.maintenance_enabled);
    AENV_BOOL("pool.block.startup_prewarm", pool.block.startup_prewarm);
    AENV_BOOL("pool.firecracker.enabled", pool.firecracker.enabled);
    AENV_BOOL("pool.firecracker.maintenance_enabled", pool.firecracker.maintenance_enabled);
    AENV_BOOL("pool.firecracker.startup_prewarm", pool.firecracker.startup_prewarm);
    AENV_UNUM("pool.firecracker.fill_concurrency", pool.firecracker.fill_concurrency,
              std::size_t);

    // [p2p]
    AENV_BOOL("p2p.enabled", p2p.enabled);
    {
        const core::TomlValue* v = table.Find("p2p.transport");
        if (v != nullptr) {
            core::Expected<std::string, std::string> p = v->AsString();
            if (!p.has_value()) {
                return core::make_unexpected(std::string("p2p.transport: ") + p.error());
            }
            if (p.value() == "disabled") {
                p2p.transport = p2p::P2pTransportKind::Disabled;
            } else if (p.value() == "iroh") {
                p2p.transport = p2p::P2pTransportKind::Iroh;
            } else {
                return core::make_unexpected(
                    std::string("p2p.transport: expected \"disabled\" or \"iroh\""));
            }
        }
    }
    AENV_STR("p2p.store_dir", p2p.store_dir);
    AENV_STR("p2p.listen_addr", p2p.listen_addr);
    AENV_UNUM("p2p.lookup_timeout_ms", p2p.lookup_timeout_ms, uint64_t);
    AENV_UNUM("p2p.fetch_timeout_ms", p2p.fetch_timeout_ms, uint64_t);
    AENV_UNUM("p2p.peer_discovery_refresh_interval_secs",
              p2p.peer_discovery_refresh_interval_secs, uint64_t);

    // [sandbox_proxy] / [custom_extension]
    {
        const core::TomlValue* v = table.Find("sandbox_proxy.domains");
        if (v != nullptr) {
            core::Expected<std::vector<std::string>, std::string> p = v->AsStringArray();
            if (!p.has_value()) {
                return core::make_unexpected(std::string("sandbox_proxy.domains: ") + p.error());
            }
            sandbox_proxy.domains = p.value();
        }
    }
    AENV_OPT_STR("custom_extension.url", custom_extension.url);
    AENV_UNUM("custom_extension.timeout_ms", custom_extension.timeout_ms, uint64_t);

#undef AENV_STR
#undef AENV_OPT_STR
#undef AENV_BOOL
#undef AENV_NUM
#undef AENV_UNUM

    // [image] and [network] own their own key sets.
    core::Expected<core::Unit, std::string> nested = image.LoadFrom(table);
    if (!nested.has_value()) return nested;
    nested = network.LoadFrom(table);
    if (!nested.has_value()) return nested;

    return core::Unit();
}

// ---------------------------------------------------------------------------
// ConfigManager
// ---------------------------------------------------------------------------

namespace {
ConfigManager* g_global_config_manager = nullptr;
}  // namespace

core::Optional<std::string> ConfigManager::EnvPath(const char* name) {
    return EnvTrimmedNonEmpty(name);
}

std::string ConfigManager::DefaultConfigPath() {
    // Rust uses `env!("CARGO_MANIFEST_DIR")/config/default.toml`. There is no
    // build-time source root in the C++ build, so honour an explicit override
    // and otherwise fall back to the installed location.
    core::Optional<std::string> root = EnvTrimmedNonEmpty("AENV_SOURCE_ROOT");
    if (root.has_value()) return PathJoin(PathJoin(*root, "config"), "default.toml");
    return "/etc/aenv/default.toml";
}

core::Expected<AppConfig, std::string> ConfigManager::LoadConfigFile(const std::string& path) {
    const std::string config_dir = PathParent(path);

    AppConfig config;
    core::Expected<core::TomlTable, std::string> table = core::TomlTable::ParseFile(path);
    if (!table.has_value()) {
        return core::make_unexpected(std::string("load config ") + path + ": " + table.error());
    }
    core::Expected<core::Unit, std::string> loaded = config.LoadFrom(table.value());
    if (!loaded.has_value()) {
        return core::make_unexpected(std::string("load config ") + path + ": " + loaded.error());
    }
    // confique layers `.env()` over `.file(path)`, so env wins.
    config.ApplyEnvOverrides();

    core::Expected<core::Unit, std::string> normalized = config.Normalize(config_dir);
    if (!normalized.has_value()) return core::make_unexpected(normalized.error());
    core::Expected<core::Unit, std::string> validated = config.Validate();
    if (!validated.has_value()) return core::make_unexpected(validated.error());

    return config;
}

core::Expected<ConfigManager, std::string> ConfigManager::New() {
    core::Optional<std::string> from_env = EnvPath(kEnvConfigPath);
    const std::string config_path = from_env.has_value() ? *from_env : DefaultConfigPath();
    core::Expected<AppConfig, std::string> config = LoadConfigFile(config_path);
    if (!config.has_value()) return core::make_unexpected(config.error());

    ConfigManager manager;
    manager.config_ = config.value();
    manager.config_path_ = config_path;
    return manager;
}

core::Expected<ConfigManager, std::string> ConfigManager::NewFromPath(const std::string& path) {
    core::Expected<AppConfig, std::string> config = LoadConfigFile(path);
    if (!config.has_value()) return core::make_unexpected(config.error());

    ConfigManager manager;
    manager.config_ = config.value();
    manager.config_path_ = path;
    return manager;
}

core::Expected<const ConfigManager*, std::string> ConfigManager::InitGlobal() {
    if (g_global_config_manager != nullptr) {
        return const_cast<const ConfigManager*>(g_global_config_manager);
    }
    core::Expected<ConfigManager, std::string> manager = New();
    if (!manager.has_value()) return core::make_unexpected(manager.error());
    g_global_config_manager = new ConfigManager(manager.value());
    return const_cast<const ConfigManager*>(g_global_config_manager);
}

core::Expected<const ConfigManager*, std::string> ConfigManager::InitGlobalFromPath(
    const std::string& path) {
    if (g_global_config_manager != nullptr) {
        return const_cast<const ConfigManager*>(g_global_config_manager);
    }
    core::Expected<ConfigManager, std::string> manager = NewFromPath(path);
    if (!manager.has_value()) return core::make_unexpected(manager.error());
    g_global_config_manager = new ConfigManager(manager.value());
    return const_cast<const ConfigManager*>(g_global_config_manager);
}

const ConfigManager* ConfigManager::Global() { return g_global_config_manager; }

const AppConfig* ConfigManager::GlobalConfig() {
    return g_global_config_manager == nullptr ? nullptr : &g_global_config_manager->config_;
}

void ConfigManager::ResetGlobalForTesting() {
    delete g_global_config_manager;
    g_global_config_manager = nullptr;
}

}  // namespace cfg
}  // namespace agentenv
