// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/dev.rs
#include "agentenv/storage/ublk/dev.h"

namespace agentenv {
namespace storage {
namespace ublk {

core::Expected<std::unique_ptr<UblkDev>, std::string>
UblkDevBuilder::Build(std::shared_ptr<UblkTarget> /*target*/) {
    return core::make_unexpected(std::string("ublk dev requires AGENTENV_WITH_LIBURING"));
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
