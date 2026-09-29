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
#include "agentenv/sandbox/backend.h"

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
    // Rust `Orchestrator::with_file_backed_store_and_factory` builds the
    // in-memory metadata store plus a file-backed persister; the C++ port
    // currently wires the store and the backend.
    auto backend = std::shared_ptr<ae::sandbox::Backend>(
                       ae::sandbox::MakeBackend(cfg.sandbox_backend).release());
    auto store   = std::shared_ptr<ae::orchestrator::MetadataStore>(
                       new ae::orchestrator::InMemoryMetadataStore());
    ae::orchestrator::Orchestrator svc(store, backend);

    // 3) HTTP server.
    auto server = ae::api::MakeServer();
    ae::api::RegisterOrchestratorRoutes(server.get(), &svc);

    AGENTENV_INFO("agentenv listening on " << cfg.server_bind_addr);
    auto r = server->ListenAndServe(cfg.server_bind_addr);
    if (!r.ok()) {
        AGENTENV_ERROR("server failed: " << r.error().chain());
        return 1;
    }
    return 0;
}
