// SPDX-License-Identifier: MIT
// Top-level agentenv binary — wires the orchestrator + backend + api::Server.
// This is the C++11 analog of Rust `src/bin/agentenv.rs`.
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>

#include "agentenv/api/server.h"
#include "agentenv/core/config.h"
#include "agentenv/core/logging.h"
#include "agentenv/orchestrator/persistence.h"
#include "agentenv/orchestrator/service.h"
#include "agentenv/cfg.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/factory_impl.h"

namespace ae = agentenv;

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

    // 3) HTTP server.
    auto server = ae::api::MakeServer();
    ae::api::RegisterOrchestratorRoutes(server.get(), svc.get());

    // No "listening" line here: this used to log it before the bind, so a
    // server that never came up still claimed it had. `ListenAndServe` logs
    // once the socket is actually bound, with the port it really got.
    auto r = server->ListenAndServe(cfg.server_bind_addr);
    if (!r.ok()) {
        AGENTENV_ERROR("server failed: " << r.error().chain());
        return 1;
    }
    return 0;
}
