// SPDX-License-Identifier: MIT
// Rust: src/p2p/error.rs — P2pError + P2pResult.
//
// Strictly mirrors the Rust `enum Error` variant set. There are exactly five
// variants; `InvalidCatalog` is the only one carrying a second payload (path).
#ifndef AGENTENV_P2P_ERROR_H_
#define AGENTENV_P2P_ERROR_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace p2p {

/// Rust: `P2pError` enum variants.
struct P2pError {
    enum Kind {
        /// Rust `TransportDisabled`.
        TransportDisabled,
        /// Rust `InvalidDescriptor { reason }`.
        InvalidDescriptor,
        /// Rust `InvalidCatalog { path, reason }`.
        InvalidCatalog,
        /// Rust `Timeout { operation }`.
        Timeout,
        /// Rust `Internal(anyhow::Error)` — `#[error(transparent)]`.
        Internal,
    };
    Kind kind;
    /// `reason` for Invalid*, `operation` for Timeout, the message for Internal.
    std::string message;
    /// Only meaningful for `InvalidCatalog`.
    std::string path;

    P2pError() : kind(Internal) {}
    P2pError(Kind k, std::string m) : kind(k), message(std::move(m)) {}

    /// Rust `Error::TransportDisabled`.
    static P2pError MakeDisabled() { return P2pError(TransportDisabled, std::string()); }
    /// Rust `Error::InvalidDescriptor { reason }`.
    static P2pError Invalid(std::string reason) {
        return P2pError(InvalidDescriptor, std::move(reason));
    }
    /// Rust `Error::InvalidCatalog { path, reason }`.
    static P2pError MakeInvalidCatalog(std::string path, std::string reason) {
        P2pError e(InvalidCatalog, std::move(reason));
        e.path = std::move(path);
        return e;
    }
    /// Rust `Error::Timeout { operation }`.
    static P2pError MakeTimeout(std::string operation) {
        return P2pError(Timeout, std::move(operation));
    }
    /// Rust `Error::Internal(anyhow!(..))`.
    static P2pError MakeInternal(std::string m) { return P2pError(Internal, std::move(m)); }
    /// Rust `Error::internal_message(operation, source)` — "{operation}: {source}".
    static P2pError InternalMessage(const std::string& operation, const std::string& source) {
        return P2pError(Internal, operation + ": " + source);
    }

    /// Rust `Display` (thiserror `#[error(..)]` strings), verbatim.
    std::string ToString() const;
};

/// Rust: `type P2pResult<T> = Result<T, P2pError>`.
template <typename T>
using P2pResult = core::Expected<T, P2pError>;

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_ERROR_H_
