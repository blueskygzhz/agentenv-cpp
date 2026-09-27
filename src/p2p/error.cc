// SPDX-License-Identifier: MIT
// Rust: src/p2p/error.rs
#include "agentenv/p2p/error.h"

namespace agentenv {
namespace p2p {

std::string P2pError::ToString() const {
    switch (kind) {
        case Disabled:   return "p2p transport disabled";
        case InvalidDescriptor: return "invalid descriptor: " + message;
   case NotFound:          return "artifact not found: " + message;
  case Timeout:        return "p2p timeout: " + message;
        case Transport:         return "transport error: " + message;
     case Internal:
        default:        return "internal error: " + message;
    }
}

}  // namespace p2p
}  // namespace agentenv
