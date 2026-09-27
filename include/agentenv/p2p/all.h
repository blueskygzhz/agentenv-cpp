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

#endif  // AGENTENV_P2P_ALL_H_
