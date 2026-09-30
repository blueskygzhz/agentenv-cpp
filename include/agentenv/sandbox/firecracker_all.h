// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/mod.rs — aggregate re-export.
//
// This header now just re-exports the per-file firecracker sub-headers, mirroring
// the Rust `mod.rs` that re-exports its child modules. Include it to pull in the
// whole Firecracker driver surface; include a specific sub-header for a narrower
// dependency.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_ALL_H_
#define AGENTENV_SANDBOX_FIRECRACKER_ALL_H_

#include "agentenv/sandbox/firecracker/config.h"     // Rust: config.rs
#include "agentenv/sandbox/firecracker/socket.h"     // Rust: socket.rs / connector.rs
#include "agentenv/sandbox/firecracker/mmds.h"       // Rust: mmds.rs
#include "agentenv/sandbox/firecracker/instance.h"   // Rust: instance.rs / process_vm_reader.rs
#include "agentenv/sandbox/firecracker/pool.h"       // Rust: pool.rs / factory.rs
#include "agentenv/sandbox/firecracker/sandbox.h"    // Rust: sandbox.rs (FirecrackerBackend)

#endif  // AGENTENV_SANDBOX_FIRECRACKER_ALL_H_
