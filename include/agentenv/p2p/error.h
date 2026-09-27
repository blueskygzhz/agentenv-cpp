// SPDX-License-Identifier: MIT
// Rust: src/p2p/error.rs — P2pError + P2pResult.
#ifndef AGENTENV_P2P_ERROR_H_
#define AGENTENV_P2P_ERROR_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace p2p {

/// Rust: `P2pError` enum variants.
struct P2pError {
    enum Kind {
        Disabled,
        InvalidDescriptor,
        NotFound,
        Timeout,
  Transport,
        Internal,
    };
    Kind kind;
    std::string message;

    P2pError() : kind(Internal) {}
    P2pError(Kind k, std::string m) : kind(k), message(std::move(m)) {}

    static P2pError MakeDisabled() { return P2pError(Disabled, "p2p transport disabled"); }
 static P2pError Invalid(std::string reason) { return P2pError(InvalidDescriptor, std::move(reason)); }
    static P2pError MakeNotFound(std::string k) { return P2pError(NotFound, std::move(k)); }
    static P2pError MakeTimeout(std::string m) { return P2pError(Timeout, std::move(m)); }
    static P2pError MakeInternal(std::string m) { return P2pError(Internal, std::move(m)); }

    std::string ToString() const;
};

/// Rust: `type P2pResult<T> = Result<T, P2pError>`.
template <typename T>
using P2pResult = core::Expected<T, P2pError>;

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_ERROR_H_
