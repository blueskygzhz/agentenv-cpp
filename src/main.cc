// SPDX-License-Identifier: MIT
// Top-level agentenv binary — wires the orchestrator + backend + api::Server.
// This is the C++11 analog of Rust `src/bin/agentenv.rs`.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "agentenv/api/auth_middleware.h"
#include "agentenv/api/server.h"
#include "agentenv/api_key.h"
#include "agentenv/core/config.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/logging.h"
#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/service.h"
#include "agentenv/cfg.h"
#include "agentenv/p2p/http_transport.h"
#include "agentenv/p2p/node.h"
#include "agentenv/p2p/scheduler_http.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/factory_impl.h"

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

int main(int argc, char** argv) {
    std::string cfg_path = (argc > 1) ? argv[1] : "/etc/agentenv/config.toml";

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
    ae::cfg::AppConfig app_cfg;
    {
        auto mgr = ae::cfg::ConfigManager::NewFromPath(cfg_path);
        if (mgr.ok()) {
            app_cfg = mgr.value().config();
        } else {
            AGENTENV_WARN("app config defaults: " << mgr.error());
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
    auto factory = std::shared_ptr<ae::sandbox::SandboxBackendFactory>(
                       new ae::sandbox::firecracker::FirecrackerSandboxFactory(
                           fc_cfg, app_cfg));

    auto built = ae::orchestrator::Orchestrator::New(store, factory, persister, app_cfg);
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

    ae::api::RegisterOrchestratorRoutes(server.get(), svc.get());

    InstallShutdownHandlers(server.get());

    // No "listening" line here: this used to log it before the bind, so a
    // server that never came up still claimed it had. `ListenAndServe` logs
    // once the socket is actually bound, with the port it really got.
    auto r = server->ListenAndServe(cfg.server_bind_addr);

    // 6) Graceful shutdown.
    //
    // Rust `server.rs:191-230` tears down in a fixed order. Of that sequence,
    // only the orchestrator and the p2p transport exist in this port; the
    // startup-manifest drain, image-build cleanup, observability reporter,
    // firecracker pool, ublk daemon and overlaybd p2p runtime are either
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
