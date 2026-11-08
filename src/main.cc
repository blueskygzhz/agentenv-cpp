// SPDX-License-Identifier: MIT
// Top-level agentenv binary — wires the orchestrator + backend + api::Server.
// This is the C++11 analog of Rust `src/bin/agentenv.rs`.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "agentenv/api/auth_middleware.h"
#include "agentenv/api/proxy.h"
#include "agentenv/api/server.h"
#include "agentenv/api_key.h"
#include "agentenv/core/config.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/logging.h"
#include "agentenv/image/cache_service.h"
#include "agentenv/image/resolver.h"
#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/service.h"
#include "agentenv/cfg.h"
#include "agentenv/snapshot/repository/backends.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/volume.h"
#include "agentenv/p2p/http_transport.h"
#include "agentenv/p2p/node.h"
#include "agentenv/p2p/scheduler_http.h"
#include "agentenv/privileges.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/factory_impl.h"
#include "agentenv/sandbox/firecracker/pool.h"
#include "agentenv/sandbox/network.h"
#include "agentenv/sandbox/network/manager.h"
#include "agentenv/setup.h"
#include "agentenv/setup/deps.h"
#include "agentenv/setup/packages.h"

namespace ae = agentenv;

namespace {

/// Adapts `api::Server` to the contract `agentenv-p2p` declares.
///
/// The indirection is a build constraint, not a design preference:
/// `agentenv-api` already links `agentenv-p2p` transitively (api ->
/// orchestrator -> snapshot -> p2p), so `p2p` naming `api::Server` directly
/// would close a CMake cycle. `p2p::P2pBlobServer` is the narrowest contract
/// that avoids it, and this is the one place in the tree that can see both
/// types — the same reason `SchedulerWireTransport` is injected rather than
/// linked.
///
/// Note what is deliberately *not* done here: no `Use` layer is installed.
/// The P2P surface authenticates with a cluster-wide shared token, not the
/// node's API key, and `api::Server::Use` runs before routing for every
/// request — sharing the API's server would gate the blob routes with the
/// wrong credential entirely. A separate server on `p2p.listen_addr` is also
/// what lets the blob port stay off any public interface.
class ApiServerBlobServer : public ae::p2p::P2pBlobServer {
 public:
    ApiServerBlobServer() : server_(ae::api::MakeServer()) {}

    bool valid() const { return !!server_; }

    void Route(const std::string& method, const std::string& path_pattern,
               Handler handler) override {
        if (!server_) return;
        // The pattern passes straight through: `P2pBlobServer` documents the
        // same `{name}` placeholder convention `api::Server` matches.
        server_->Route(method, path_pattern,
                       [handler](const ae::api::HttpRequest& request,
                                 ae::api::HttpResponse* response) {
            ae::p2p::P2pServerRequest translated;
            translated.method = request.method;
            translated.path = request.path;
            translated.body = request.body;
            // `api` keys headers in an unordered_map and `p2p` in a map; the
            // casing is preserved either way, and the p2p side looks them up
            // case-insensitively.
            for (std::unordered_map<std::string, std::string>::const_iterator it =
                     request.headers.begin();
                 it != request.headers.end(); ++it) {
                translated.headers[it->first] = it->second;
            }

            ae::p2p::P2pServerResponse produced;
            handler(translated, &produced);

            response->status = produced.status;
            response->body = produced.body;
            for (std::map<std::string, std::string>::const_iterator it =
                     produced.headers.begin();
                 it != produced.headers.end(); ++it) {
                response->headers[it->first] = it->second;
            }
        });
    }

    ae::core::Expected<ae::core::Unit, std::string>
        ListenAndServe(const std::string& bind_addr) override {
        if (!server_) {
            return ae::core::make_unexpected(
                std::string("no HTTP server backend is compiled in"));
        }
        const ae::core::Expected<ae::core::Unit, ae::core::AnyError> served =
            server_->ListenAndServe(bind_addr);
        if (!served.ok()) {
            return ae::core::make_unexpected(served.error().chain());
        }
        return ae::core::Unit{};
    }

    void Stop() override {
        if (server_) server_->Stop();
    }

 private:
    std::unique_ptr<ae::api::Server> server_;
};

/// Rust `shutdown_signal()` — SIGINT/SIGTERM start a graceful shutdown.
///
/// Rust can `await` a signal; a C++11 handler may only touch
/// `volatile sig_atomic_t`, so the handler records the signal and the main
/// thread acts on it. `Server::Stop()` is the one call that is safe from a
/// handler-adjacent path because it only flips a flag and shuts the listening
/// fd down, which is what unblocks `ListenAndServe`.
volatile sig_atomic_t g_shutdown_signal = 0;
ae::api::Server* g_server_for_signal = NULL;

void HandleShutdownSignal(int signum) {
    g_shutdown_signal = signum;
    if (g_server_for_signal != NULL) {
        g_server_for_signal->Stop();
    }
}

void InstallShutdownHandlers(ae::api::Server* server) {
    g_server_for_signal = server;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &HandleShutdownSignal;
    ::sigemptyset(&sa.sa_mask);
    // No SA_RESTART: the point is to interrupt the accept loop.
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, NULL);
    ::sigaction(SIGTERM, &sa, NULL);

    // A client that disappears mid-write must not kill the process. Rust gets
    // this from tokio; here the default SIGPIPE disposition would terminate.
    struct sigaction ignore;
    std::memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore.sa_mask);
    ignore.sa_flags = 0;
    ::sigaction(SIGPIPE, &ignore, NULL);
}

/// Rust `p2p::transport_from_config(config, &identity)`.
///
/// Rust passes `&AppConfig` straight through because `cfg` and `p2p` are one
/// crate. Here `agentenv-cfg` links `agentenv-p2p`, so the projection happens
/// at the call site instead; see the note on `p2p::P2pFactoryInputs`.
ae::p2p::P2pFactoryInputs BuildP2pInputs(const ae::cfg::AppConfig& app_cfg,
                                        const ae::core::NodeIdentity& identity) {
    ae::p2p::P2pRawConfig raw;
    raw.enabled = app_cfg.p2p.enabled;
    raw.transport = app_cfg.p2p.transport;
    raw.store_dir = app_cfg.p2p.store_dir;
    raw.listen_addr = app_cfg.p2p.listen_addr;
    raw.lookup_timeout_ms = app_cfg.p2p.lookup_timeout_ms;
    raw.fetch_timeout_ms = app_cfg.p2p.fetch_timeout_ms;
    raw.peer_discovery_refresh_interval_secs =
        app_cfg.p2p.peer_discovery_refresh_interval_secs;

    ae::p2p::P2pFactoryInputs inputs;
    inputs.p2p = ae::p2p::ResolvedP2pConfig::FromConfig(raw);
    inputs.scheduler_endpoint = app_cfg.cluster.scheduler_endpoint;
    inputs.node_id = identity.id;
    inputs.cluster_id = identity.cluster_id.ToString();
    // Only read by the `http` transport; the iroh arm ignores both.
    inputs.p2p_advertise_addr = app_cfg.p2p.advertise_addr;
    inputs.p2p_shared_token = app_cfg.p2p.shared_token;
    return inputs;
}

/// Rust `NodeIdentity::from_config(&config.node_identity)`.
///
/// `cfg::NodeIdentityConfig` and `core::NodeIdentityConfig` are two distinct
/// types with identical fields (Rust has one). Copying field-by-field here is
/// the adapter that absence implies.
ae::core::NodeIdentityConfig ToCoreIdentityConfig(
    const ae::cfg::NodeIdentityConfig& from) {
    ae::core::NodeIdentityConfig to;
    to.node_id = from.node_id;
    to.cluster_id = from.cluster_id;
    to.service_instance_id = from.service_instance_id;
    return to;
}

}  // namespace

namespace {

/// Rust `ServerCli` (clap). `--setup-only` provisioning is not ported, so it
/// is refused rather than mistaken for a config path and a normal start.
struct ServerCli {
    bool setup_host = false;
    std::string runtime_user = "aenv";
    std::string runtime_group = "aenv";
    bool runtime_account_given = false;
    std::string config_path;
};

bool ParseServerCli(int argc, char** argv, ServerCli* cli, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        bool has_inline = false;
        std::string name = arg;
        const std::size_t eq = arg.find('=');
        if (arg.compare(0, 2, "--") == 0 && eq != std::string::npos) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
            has_inline = true;
        }
        const bool takes_value = name == "--config" || name == "--runtime-user" ||
                                 name == "--runtime-group";
        if (takes_value && !has_inline) {
            if (i + 1 >= argc) {
                *error = "a value is required for '" + name + "'";
                return false;
            }
            value = argv[++i];
        }
        if (name == "--setup-host") {
            cli->setup_host = true;
        } else if (name == "--setup-only") {
            *error = "--setup-only provisioning is not available in this build";
            return false;
        } else if (name == "--config") {
            cli->config_path = value;
        } else if (name == "--runtime-user") {
            cli->runtime_user = value;
            cli->runtime_account_given = true;
        } else if (name == "--runtime-group") {
            cli->runtime_group = value;
            cli->runtime_account_given = true;
        } else if (arg.compare(0, 1, "-") != 0 && cli->config_path.empty()) {
            // Positional config path, kept for existing deployments of this
            // port; Rust only knows `--config`.
            cli->config_path = arg;
        } else {
            *error = "unexpected argument '" + arg + "'";
            return false;
        }
    }
    if (cli->runtime_account_given && !cli->setup_host) {
        // clap `requires = "setup_host"`.
        *error = "--runtime-user/--runtime-group require --setup-host";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    ServerCli cli;
    std::string cli_error;
    if (!ParseServerCli(argc, argv, &cli, &cli_error)) {
        std::fprintf(stderr, "error: %s\n", cli_error.c_str());
        return 2;
    }
    // Rust: `--config`, else `AENV_CONFIG_PATH` (via `init_global`).
    std::string cfg_path = cli.config_path;
    if (cfg_path.empty()) {
        const char* from_env = std::getenv("AENV_CONFIG_PATH");
        cfg_path = (from_env != NULL && *from_env != '\0') ? from_env
                                                          : "/etc/agentenv/config.toml";
    }

    // 1) Load config (falls back to defaults if missing).
    ae::core::Config cfg;
    auto cfg_res = ae::core::Config::Load(cfg_path);
    if (cfg_res.ok()) cfg = cfg_res.value();
    else AGENTENV_WARN("using defaults: " << cfg_res.error().chain());

    ae::core::Logger::Get().SetLevel(
        static_cast<ae::core::LogLevel>(cfg.log_level));

    // 2) Wire dependencies.
    //
    // Rust `Orchestrator::with_file_backed_store_and_factory`: an in-memory
    // metadata store over a file-backed persister, restored through
    // `Orchestrator::New`.
    //
    // The store being in-memory is not the same as losing state across a
    // restart -- the records live in the persister, and `New` replays them
    // into the store before the orchestrator exists. Constructing the
    // orchestrator directly, as this used to, skips that replay: every paused
    // sandbox from the previous run would be invisible while its artifacts
    // stayed on disk, so nothing could resume them and nothing would collect
    // them either.
    //
    // Rust `ConfigManager::init_global_from_path`: installed as the global so
    // the process-wide singletons that read `global_config()` --
    // `SlotNetworkManager::Global()` takes `runtime_path` and the network pool
    // from it -- see the same config the factory does.
    ae::cfg::AppConfig app_cfg;
    {
        auto mgr = ae::cfg::ConfigManager::InitGlobalFromPath(cfg_path);
        if (mgr.ok()) {
            app_cfg = mgr.value()->config();
        } else {
            AGENTENV_WARN("app config defaults: " << mgr.error());
        }
    }

    // Rust `server.rs:75-79`.
    if (cli.setup_host) {
        auto host = ae::setup::EnsureHost(app_cfg.deps_path, cli.runtime_user,
                                          cli.runtime_group);
        if (!host.ok()) {
            AGENTENV_ERROR("host setup failed: " << host.error());
            return 1;
        }
        AGENTENV_INFO("host setup complete");
        return 0;
    }

    // Rust `server.rs:81-82`, before anything that can spawn a process. The
    // server keeps CAP_NET_ADMIN / CAP_SYS_ADMIN for the operations that need
    // them (each runs in a capability-scoped thread); clearing the ambient set
    // is what stops every ordinary child — firecracker, `ip`, `mount` helpers —
    // from inheriting them across `execve`.
    {
        auto required = ae::privileges::RequireRuntimeCapabilities();
        if (!required.ok()) {
            AGENTENV_ERROR(required.error());
            return 1;
        }
        auto cleared = ae::privileges::ClearAmbientCapabilities();
        if (!cleared.ok()) {
            AGENTENV_ERROR(cleared.error());
            return 1;
        }
    }

    // Rust `setup::ensure_environment` (`server.rs:94`), its networking steps:
    // the runtime directory, reclaiming namespaces a previous run left
    // mounted, the IPv4-forwarding precondition, and the capacity report.
    // All run before the network manager exists, so no live slot can be
    // mistaken for a stale one.
    {
        auto runtime_dir = ae::core::fs::CreateDirAll(app_cfg.runtime_path);
        if (!runtime_dir.ok()) {
            AGENTENV_ERROR("create AENV runtime directory " << app_cfg.runtime_path << ": "
                                                            << runtime_dir.error());
            return 1;
        }
        auto netns = ae::sandbox::network::PrepareRuntime(app_cfg.runtime_path);
        if (!netns.ok()) {
            AGENTENV_ERROR("prepare AENV network namespace runtime directory: " << netns.error());
            return 1;
        }
        auto forwarding = ae::setup::CheckIpForwarding();
        if (!forwarding.ok()) {
            AGENTENV_ERROR(forwarding.error());
            return 1;
        }
        ae::setup::network_capacity::Check();
    }

    // Rust `setup::ensure_environment`, the rest of it. These steps existed
    // here as functions but nothing called them, so a node came up without
    // its virtualization/ublk preconditions checked, without its dependencies
    // installed and without the generated overlaybd configs the ublk daemon
    // reads. Every one of those surfaced later as a launch failure that named
    // the launch rather than the unmet prerequisite.
    // Hoisted: the tools-drive binder installed further down needs the same
    // manifest, and re-reading it there could pick up a different file.
    ae::setup::deps::Manifest deps_manifest;
    {
        const ae::core::Optional<std::string> manifest_path = ae::setup::FindDepsManifest();
        if (!manifest_path.has_value()) {
            AGENTENV_ERROR("cannot locate config/deps_manifest.toml; set "
                           "AGENTENV_DEPS_MANIFEST or run from a tree that contains it");
            return 1;
        }
        const auto manifest = ae::setup::deps::Manifest::ParseFile(*manifest_path);
        if (!manifest.ok()) {
            AGENTENV_ERROR("parse " << *manifest_path << ": " << manifest.error());
            return 1;
        }
        deps_manifest = manifest.value();

        // Step 1. Runtime packages are validated, never installed, on the
        // normal start path: installing would need root, which this process
        // has deliberately just given up.
        auto packages = ae::setup::packages::CheckRuntime(*manifest_path);
        if (!packages.ok()) {
            AGENTENV_ERROR(packages.error());
            return 1;
        }

        // Steps 2, 3 and 6. Reported together rather than fail-fast, then
        // failed as a group: an operator fixing a fresh host wants both the
        // KVM and the ublk verdict from one run, not one per restart.
        const std::vector<ae::setup::EnvironmentCheck> checks =
            ae::setup::CheckEnvironment(app_cfg.virtualization_mode);
        bool environment_ok = true;
        for (std::size_t i = 0; i < checks.size(); ++i) {
            if (checks[i].ok) continue;
            environment_ok = false;
            AGENTENV_ERROR("environment check failed step=" << checks[i].step << ": "
                                                            << checks[i].detail);
        }
        if (!environment_ok) return 1;

        // Step 4. Downloads firecracker, the kernel, regctl and the overlaybd
        // CLI tools, then writes the generated overlaybd global configs. The
        // configs come second because they are written per cache directory the
        // install just created.
        auto provisioned =
            ae::setup::deps::Ensure(app_cfg, app_cfg.deps_path, manifest.value());
        if (!provisioned.ok()) {
            AGENTENV_ERROR("ensure dependencies: " << provisioned.error());
            return 1;
        }
        // The p2p facade address is threaded in by the caller upstream; this
        // build has no iroh transport, so the configs are written without one
        // rather than with a placeholder that would point the daemon at a
        // peer-to-peer endpoint that does not exist.
        auto obd_configs = ae::setup::deps::WriteGeneratedOverlaybdGlobalConfigs(
            app_cfg, ae::core::Optional<std::string>());
        if (!obd_configs.ok()) {
            AGENTENV_ERROR("write generated overlaybd configs: " << obd_configs.error());
            return 1;
        }

        // Step 5. Advisory: a node that cannot raise its own limit still runs,
        // just with fewer concurrent sandboxes, so this must not be fatal.
        auto nofile = ae::setup::RaiseFileDescriptorLimit();
        if (nofile.ok()) {
            AGENTENV_INFO("file descriptor limit soft=" << nofile.value());
        } else {
            AGENTENV_WARN("raise file descriptor limit: " << nofile.error());
        }
    }

    auto store = std::shared_ptr<ae::orchestrator::MetadataStore>(
                     new ae::orchestrator::InMemoryMetadataStore());

    // The persister owns the virtualization mode because it decides how a
    // record's snapshot artifacts are interpreted; a KVM-written record read
    // back on a PVM node is not resumable, and the mode is what detects that.
    auto persister = std::shared_ptr<ae::orchestrator::SandboxPersister>(
                         new ae::orchestrator::FileBackedSandboxPersister(
                             cfg.data_dir + "/sandboxes", app_cfg.virtualization_mode));

    ae::sandbox::firecracker::Config fc_cfg;
    fc_cfg.firecracker_bin = app_cfg.ResolvedFirecrackerBinaryPath();
    // The AppConfig goes in too: `Build` projects each request onto it (rootfs
    // image, tools drive, boot-arg allowlist), and a factory without one can
    // only refuse to build.
    auto fc_factory = std::make_shared<ae::sandbox::firecracker::FirecrackerSandboxFactory>(
        fc_cfg, app_cfg);

    // Rust `UblkDeviceManager::init_global_from_config`: every fresh sandbox's
    // user rootfs and extra drives (volumes included) are ublk devices the
    // daemon exports. Rust spawns the daemon itself; this port connects to
    // one already listening on `ublk.daemon_socket_path` (run
    // `agentenv-ublk-daemon` beside the server). As in Rust, an unreachable
    // daemon is not fatal at startup -- the manager is left without a client
    // and `Build` refuses fresh sandboxes, naming the daemon.
    {
        const std::string& socket_path = app_cfg.ublk.daemon_socket_path;
        std::shared_ptr<ae::storage::ublk_daemon::DaemonClient> client(
            new ae::storage::ublk_daemon::DaemonClient(
                ae::storage::ublk_daemon::MakeUnixTransport(socket_path), socket_path));
        const auto features = client->GetFeatures();
        if (!features.ok()) {
            AGENTENV_ERROR("ublk daemon at " << socket_path
                                             << " is unreachable; fresh sandboxes are disabled: "
                                             << features.error().message);
            client.reset();
        } else {
            AGENTENV_INFO("connected to ublk daemon at " << socket_path);
        }
        fc_factory->SetUblkDeviceManager(std::make_shared<ae::sandbox::ublk::DeviceManager>(
            client, app_cfg.BlockPoolConfig().has_value()));
    }

    // Rust `crate::sandbox::resolve_tools_image` --
    // `deps::prepare_tools_image(config, version, |r| ImageResolver::new(config)
    //  .resolve_tools(&r))`. This binder is the whole reason the two sides stay
    // decoupled: `setup::deps` performs the arbitration and the install, the
    // image resolver performs the registry work, and the sandbox backend only
    // learns which of the two drive shapes to attach.
    //
    // A *fresh* resolver per call, and deliberately without the snapshot
    // repository: upstream's `resolve_tools` consumes `self` and mutates the
    // conversion policy (it pins the v1 converter and clears the overlaybd
    // referrer prefixes), so reusing one instance would leak a system
    // dependency's policy into user-image resolution. System dependencies also
    // come from their configured release source, which has nothing to do with
    // the repository.
    {
        const ae::cfg::AppConfig tools_config = app_cfg;
        const ae::setup::deps::Manifest tools_manifest = deps_manifest;
        fc_factory->SetToolsDriveResolver(
            [tools_config, tools_manifest](const std::string& version)
                -> ae::core::Expected<ae::core::Optional<std::string>, std::string> {
                return ae::setup::deps::PrepareToolsImage(
                    tools_config, tools_manifest, version,
                    [&tools_config](const std::string& image_ref)
                        -> ae::core::Expected<ae::core::Optional<std::string>, std::string> {
                        ae::image::DefaultImageResolver resolver(tools_config);
                        const auto resolved = resolver.ResolveTools(image_ref);
                        if (!resolved.ok()) {
                            return ae::core::make_unexpected(resolved.error().ToString());
                        }
                        if (!resolved.value().has_value()) {
                            // The release ships no overlaybd tools image; the
                            // legacy ext4 fallback handles it.
                            return ae::core::Optional<std::string>();
                        }
                        return ae::core::Optional<std::string>(
                            resolved.value()->overlaybd_config_path);
                    });
            });
    }
    // Rust `NetworkManager::global()`: every sandbox gets a slot -- netns,
    // veth pair, tap0 -- and its VMM is spawned into that namespace. Created
    // here rather than on first allocation so a missing CAP_NET_ADMIN is
    // reported at startup (construction only warns, as Rust does).
    fc_factory->SetNetworkManager(&ae::sandbox::network::SlotNetworkManager::Global());
    // Rust `FirecrackerPool::prime(10s)` (`server.rs:103`): pre-spawn VMMs in
    // their own slots so resumes skip the spawn and socket wait. Best-effort;
    // a disabled or failing pool leaves resumes on the cold path.
    {
        auto primed = ae::sandbox::firecracker::FirecrackerPool::PrimeGlobal(10 * 1000);
        if (!primed.ok()) {
            AGENTENV_WARN("firecracker pool prime failed; continuing startup: "
                          << primed.error());
        }
        fc_factory->SetFirecrackerPool(ae::sandbox::firecracker::FirecrackerPool::Global());
    }
    auto factory = std::shared_ptr<ae::sandbox::SandboxBackendFactory>(fc_factory);

    // 2b) Volume manager.
    //
    // Rust `server.rs:106-118`: the snapshot repository first, then
    // `VolumeManager::open_with_repository_and_limits(home/volumes/catalog,
    // repository, limits)`. The manager goes to both the orchestrator (which
    // releases leases on delete and publishes backings on capture) and the API
    // bundle (which serves `/volumes*`).
    //
    // The repository comes from `MakeSnapshotBackendFromConfig`, which selects
    // on `snapshot.repository_backend`. This used to branch on
    // `backend.posix_fs.has_value()` and build the posix-fs stores inline --
    // which silently sent an OSS-configured node to its local disk whenever a
    // `[backend.posix_fs]` section was also present, the ordinary state of a
    // migrated node. See that function for the whole argument.
    //
    // It returns the *pair* upstream's `build_snapshot_backend` does. The
    // second half is the runtime resolver, and it is why this call changed:
    // the repository-only form left `PosixFsRuntimeResolver` with no
    // production caller at all, so `POST /sandboxes` could not turn a
    // `templateID` into launchable paths and fell through to a fresh boot with
    // an empty rootfs config.
    std::shared_ptr<ae::volume::VolumeManager> volume_manager;
    std::shared_ptr<ae::image::ImageResolver> image_resolver;
    // Outlives the block below because the API bundle needs it too: the
    // snapshot route publishes through it. It used to be a local here, which
    // is why that route had nowhere to publish to.
    std::shared_ptr<ae::snapshot::repository::SnapshotRepository> repository;
    // The whole backend is held, not just the resolver: it owns the overlaybd
    // layer store the resolver borrows as a raw pointer, so letting it die
    // here would leave every resolve reading freed memory.
    ae::snapshot::repository::SnapshotBackend snapshot_backend;
    {
        auto made = ae::snapshot::repository::MakeSnapshotBackendFromConfig(app_cfg);
        if (!made.ok()) {
            // Fatal, as Rust's `?` in `build_snapshot_backend_from_config`
            // is. Carrying on would serve a node whose declared storage
            // backend does not exist: publishes would either fail one at a
            // time or, worse, land somewhere other than where the config
            // says.
            AGENTENV_ERROR("snapshot repository backend startup failed: "
                           << made.error().ToString());
            return 1;
        }
        snapshot_backend = made.value();
        repository = snapshot_backend.repository;
        if (!snapshot_backend.runtime_resolver) {
            // Not fatal, and said out loud because the consequence is a whole
            // route answering 501: an OSS node publishes and serves volumes
            // normally but cannot resume a snapshot, since this port has no
            // resolver for that backend.
            AGENTENV_WARN("snapshot runtime resolver unavailable for the configured"
                          << " repository backend; POST /sandboxes cannot launch from"
                          << " a snapshot on this node");
        }

        ae::volume::VolumeLimits limits;
        limits.max_size_mb = app_cfg.volume.max_size_mb;
        limits.max_mounts = app_cfg.volume.max_volume_count;

        auto opened = ae::volume::VolumeManager::OpenWithRepositoryAndLimits(
            ae::core::fs::Join(app_cfg.home_path, "volumes/catalog"), repository, limits);
        if (!opened.ok()) {
            // Fatal, as Rust's `?` is. A node that came up without a volume
            // manager would answer every `/volumes*` request with a 500 and
            // silently skip lease release on delete, leaking a reservation per
            // deleted sandbox.
            AGENTENV_ERROR("volume manager startup failed: " << opened.error().ToString());
            return 1;
        }
        volume_manager = opened.value();
        // Rust `ImageResolver::new(config).with_repository(repository)`.
        // `with_repository` mutates and returns a reference, so the shared_ptr
        // is built first and configured through it.
        auto resolver = std::make_shared<ae::image::DefaultImageResolver>(app_cfg);
        resolver->WithRepository(repository);
        image_resolver = resolver;
    }

    auto built = ae::orchestrator::Orchestrator::New(store, factory, persister, app_cfg,
                                                     volume_manager);
    if (!built.ok()) {
        // Startup restore failing is fatal on purpose. Carrying on with an
        // empty store would present a node with no sandboxes while the
        // previous run's artifacts are still on disk, and the first create
        // would allocate over them.
        AGENTENV_ERROR("orchestrator startup failed: " << built.error().Message());
        return 1;
    }
    auto svc = built.value();

    // 3) API key.
    //
    // Rust `server.rs:84` resolves this before anything else is built and
    // propagates failure with `?`. Failing closed is the point: a server that
    // came up without a key would serve every route unauthenticated, which is
    // exactly the state this wiring exists to end.
    auto api_key_resolved = ae::ApiKey::Resolve(app_cfg.home_path);
    if (!api_key_resolved.ok()) {
        AGENTENV_ERROR("api key resolution failed: " << api_key_resolved.error());
        return 1;
    }

    // 4) P2P transport.
    //
    // Rust `server.rs:87-88` resolves the node identity and then the transport,
    // with `?` on the transport: a node configured for p2p that cannot start it
    // fails to start. Keeping that is the whole point of wiring this here —
    // until now `[p2p].enabled = true` resolved to no transport at all, with
    // nothing in the logs to say so.
    ae::core::NodeIdentity identity =
        ae::core::NodeIdentity::FromConfig(ToCoreIdentityConfig(app_cfg.node_identity));
    ae::p2p::P2pFactoryInputs p2p_inputs = BuildP2pInputs(app_cfg, identity);

    if (app_cfg.p2p.enabled &&
        app_cfg.p2p.transport == ae::p2p::P2pTransportKind::Http &&
        app_cfg.p2p.shared_token.empty()) {
        // Warned about rather than refused: a single-tenant cluster whose blob
        // port is unreachable from outside may legitimately run without one.
        // Silence would be the problem — `core::http` has no TLS, so an
        // operator who did not mean this has no other hint.
        AGENTENV_WARN("p2p blob surface is unauthenticated;"
                      << " set AENV_P2P_SHARED_TOKEN or p2p.shared_token"
                      << " unless the port is unreachable from outside the cluster");
    }

    // The scheduler wire.
    //
    // Rust reaches the scheduler over gRPC; that path is gated on
    // AGENTENV_WITH_GRPC and needs a code generator this tree does not carry.
    // `MakeHttpSchedulerWire` speaks the same four calls over HTTP instead —
    // see the note at the top of `scheduler_http.h` for why that trade is the
    // right one for a cluster already running the `http` transport.
    //
    // Built only when an endpoint is configured. Without one
    // `PeerDiscoveryFromConfig` returns a Noop discovery and would never
    // consult these callbacks; constructing them anyway would log a
    // "wire ready" line for a scheduler nobody is going to call.
    ae::p2p::SchedulerWireTransport scheduler_wire;
    if (app_cfg.cluster.scheduler_endpoint.has_value()) {
        scheduler_wire = ae::p2p::MakeHttpSchedulerWire(
            *app_cfg.cluster.scheduler_endpoint, app_cfg.p2p.shared_token,
            ae::p2p::SchedulerPeerDiscovery::kRefreshRpcTimeoutMs,
            ae::p2p::SchedulerPeerDiscovery::kArtifactRpcTimeoutMs);
    }

    // The blob-server factory is only consulted by the `http` transport. It is
    // a factory rather than an instance so a `disabled` or `iroh` node never
    // constructs a listener it would not use.
    auto p2p_built = ae::p2p::TransportFromConfig(
        p2p_inputs, std::move(scheduler_wire),
        []() -> std::shared_ptr<ae::p2p::P2pBlobServer> {
            std::shared_ptr<ApiServerBlobServer> adapter(new ApiServerBlobServer());
            // Null when no server backend is compiled in. Returning it anyway
            // would defer the failure to the first request, by which point the
            // node has already advertised itself as a provider.
            if (!adapter->valid()) return std::shared_ptr<ae::p2p::P2pBlobServer>();
            return adapter;
        });
    if (!p2p_built.ok()) {
        AGENTENV_ERROR("p2p transport startup failed: " << p2p_built.error());
        return 1;
    }
    std::shared_ptr<ae::p2p::P2pTransport> p2p_transport = p2p_built.value();

    ae::p2p::P2pEndpoint p2p_local_endpoint;
    if (p2p_transport->LocalEndpoint(&p2p_local_endpoint)) {
        AGENTENV_INFO("p2p transport listening"
                      << " backend=" << p2p_local_endpoint.backend
                      << " address=" << p2p_local_endpoint.address);
    }

    // Rust `server.rs:90`, immediately after `local_endpoint()`. The image
    // cache resolves layers through this transport; until now it was built
    // here and never handed over, so every node silently fell back to pulling
    // from the registry while the p2p surface sat idle.
    //
    // Before `OverlaybdP2pRuntime` and the ublk manager, as upstream has it:
    // both can start fetching, and a cache that learns about the transport
    // afterwards would miss exactly the first round of lookups.
    ae::image::InitializeImageCacheP2pTransport(app_cfg, p2p_transport);

    // 5) HTTP server.
    auto server = ae::api::MakeServer();

    // The auth layer is installed before the routes so it covers every one of
    // them, including paths that do not exist: Rust wraps the whole router, so
    // an unauthenticated caller gets 401 rather than a 404 that confirms which
    // ids are absent.
    std::shared_ptr<ae::api::AuthChecks> auth_checks(
        new ae::api::OrchestratorAuthChecks(api_key_resolved.value(), svc.get(),
                                            app_cfg.tools.control_plane_port));
    server->Use(ae::api::MakeAuthMiddleware(auth_checks,
                                            app_cfg.sandbox_proxy.domains));
    // Rust `sandbox_proxy_classifier`, inside the auth layer: every request
    // addressing a sandbox is answered by the proxy, so one authorized by a
    // sandbox's data-plane policy can never reach a control-plane route.
    server->Use(ae::api::MakeSandboxProxyClassifier(
        app_cfg.sandbox_proxy.domains,
        ae::api::MakeOrchestratorProxyForwarder(
            svc.get(),
            static_cast<int64_t>(app_cfg.orchestrator.auto_resume_min_sandbox_timeout_secs) *
                1000)));

    ae::api::RegisterOrchestratorRoutes(server.get(), svc.get(), volume_manager,
                                        image_resolver, repository,
                                        snapshot_backend.runtime_resolver);

    InstallShutdownHandlers(server.get());

    // No "listening" line here: this used to log it before the bind, so a
    // server that never came up still claimed it had. `ListenAndServe` logs
    // once the socket is actually bound, with the port it really got.
    auto r = server->ListenAndServe(cfg.server_bind_addr);

    // 6) Graceful shutdown.
    //
    // Rust `server.rs:191-230` tears down in a fixed order. Of that sequence,
    // the orchestrator, the firecracker pool and the p2p transport exist in
    // this port; the startup-manifest drain, image-build cleanup,
    // observability reporter, ublk daemon and overlaybd p2p runtime are either
    // unported or have no global handle to stop. Their absence is a known gap,
    // not something this function can paper over.
    if (g_shutdown_signal != 0) {
        AGENTENV_INFO("received signal " << static_cast<int>(g_shutdown_signal)
                                         << ", starting graceful shutdown");
    }

    AGENTENV_INFO("stopping sandboxes before process exit");
    auto orch_stopped = svc->Shutdown();
    if (!orch_stopped.ok()) {
        AGENTENV_WARN("error occurred while shutting down orchestrator: "
                      << orch_stopped.error().Message());
    }

    if (ae::sandbox::firecracker::FirecrackerPool* pool =
            ae::sandbox::firecracker::FirecrackerPool::Global()) {
        AGENTENV_INFO("shutting down firecracker pool");
        auto pool_stopped = pool->Shutdown(false);
        if (!pool_stopped.ok()) {
            AGENTENV_WARN("error occurred while shutting down firecracker pool: "
                          << pool_stopped.error());
        }
    }

    AGENTENV_INFO("shutting down p2p transport");
    auto p2p_stopped = p2p_transport->Shutdown();
    if (!p2p_stopped.ok()) {
        AGENTENV_WARN("error occurred while shutting down p2p transport: "
                      << p2p_stopped.error().ToString());
    }

    if (!r.ok()) {
        AGENTENV_ERROR("server failed: " << r.error().chain());
        return 1;
    }
    return 0;
}
