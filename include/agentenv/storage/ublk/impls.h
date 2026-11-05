// SPDX-License-Identifier: MIT
// Placeholders against the superseded `Target` interface in `target.h`.
//
// Rust's `impls/` has no `cow.rs`; `BasicCowTarget` has no upstream
// counterpart at all. Its `overlaybd_target.rs` is ported in
// `overlaybd_target.h`, not here.
#ifndef AGENTENV_STORAGE_UBLK_IMPLS_H_
#define AGENTENV_STORAGE_UBLK_IMPLS_H_

#include <cstdint>
#include <memory>
#include <string>

#include "agentenv/storage/ublk/target.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust struct `BasicCowConfig`.
struct BasicCowConfig {
    std::string lower_path;   // read-only base
    std::string upper_path;   // COW upper layer
    int64_t     disk_bytes = 0;
};

/// Rust struct `BasicCowTarget` — copy-on-write over a base image.
std::shared_ptr<Target> MakeBasicCowTarget(const BasicCowConfig& cfg);

/// Rust struct `OverlaybdTargetConfig`.
struct OverlaybdTargetConfig {
    std::string overlaybd_config_path;
    int64_t   disk_bytes = 0;
};

/// A placeholder against the superseded `Target` interface. Serves zeroes and
/// refuses writes.
///
/// **Not** the port of Rust's `OverlaybdTarget` — that is
/// `ublk::OverlaybdTarget` in `overlaybd_target.h`, which implements
/// `UblkTarget` (Rust `UVMUblkTarget`) and reads and writes a real image.
/// Nothing constructs this; new code wants the other one.
std::shared_ptr<Target> MakeOverlaybdTarget(const OverlaybdTargetConfig& cfg);

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_IMPLS_H_
