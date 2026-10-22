// SPDX-License-Identifier: MIT
// Rust: src/p2p/mod.rs — aggregate re-export of the p2p module.
//
// Mirrors the Rust `mod.rs` that re-exports its child modules. Include this to
// pull in the whole p2p surface, or a specific sub-header for a narrower dep.
#ifndef AGENTENV_P2P_ALL_H_
#define AGENTENV_P2P_ALL_H_

#include "agentenv/p2p/types.h"       // Rust: types.rs
#include "agentenv/p2p/error.h"       // Rust: error.rs
#include "agentenv/p2p/config.h"  // Rust: config.rs
#include "agentenv/p2p/transport.h"   // Rust: transport.rs
#include "agentenv/p2p/discovery.h"   // Rust: discovery/mod.rs + scheduler.rs
#include "agentenv/p2p/mock.h"        // Rust: mock.rs
#include "agentenv/p2p/iroh.h"        // Rust: iroh/mod.rs (gated)
#include "agentenv/p2p/catalog.h"     // Rust: the portable half of iroh/catalog.rs
#include "agentenv/p2p/http_blob_store.h"  // no Rust counterpart: the HTTP byte store
#include "agentenv/p2p/http_transport.h"   // no Rust counterpart: the HTTP transport
#include "agentenv/p2p/node.h"        // Rust: mod.rs factories
#include "agentenv/p2p/scheduler_http.h"   // Rust: discovery/scheduler.rs, over HTTP

#endif  // AGENTENV_P2P_ALL_H_
