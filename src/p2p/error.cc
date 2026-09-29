// SPDX-License-Identifier: MIT
// Rust: src/p2p/error.rs
#include "agentenv/p2p/error.h"

namespace agentenv {
namespace p2p {

std::string P2pError::ToString() const {
    switch (kind) {
        // #[error("P2P artifact transport is disabled")]
        case TransportDisabled:
            return "P2P artifact transport is disabled";
        // #[error("invalid P2P artifact descriptor: {reason}")]
        case InvalidDescriptor:
            return "invalid P2P artifact descriptor: " + message;
        // #[error("invalid P2P artifact catalog at {path:?}: {reason}")]
        // `{path:?}` on a PathBuf renders quoted, so reproduce the quotes.
        case InvalidCatalog:
            return "invalid P2P artifact catalog at \"" + path + "\": " + message;
        // #[error("P2P operation timed out: {operation}")]
        case Timeout:
            return "P2P operation timed out: " + message;
        // #[error(transparent)] — forwards the inner error's Display verbatim.
        case Internal:
        default:
            return message;
    }
}

}  // namespace p2p
}  // namespace agentenv
