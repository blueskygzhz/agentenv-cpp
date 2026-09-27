// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/impls/{mod,cow,overlaybd_target}.rs — concrete Targets.
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

/// Rust struct `OverlaybdTarget` — serves reads from an overlaybd LSMT stack.
std::shared_ptr<Target> MakeOverlaybdTarget(const OverlaybdTargetConfig& cfg);

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_IMPLS_H_
