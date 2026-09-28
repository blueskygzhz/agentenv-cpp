// SPDX-License-Identifier: MIT
// Rust: src/api/impls/pagination.rs
#include "agentenv/api/pagination.h"

namespace agentenv {
namespace api {

PaginationError MakePaginationError(PaginationError::Kind kind, const std::string& detail) {
    PaginationError error;
    error.kind = kind;
    error.detail = detail;
    return error;
}

std::string PaginationError::Message() const {
    switch (kind) {
        case Kind::DecodeCursor:
            return "error decoding cursor: " + detail;
        case Kind::InvalidCursorFormat:
            return "invalid cursor format";
        case Kind::InvalidCursorUtf8:
            return "invalid cursor format (not utf-8): " + detail;
        case Kind::InvalidCursorTimestamp:
            return "invalid timestamp format in cursor: " + detail;
        case Kind::InvalidCursorValue:
        default:
            return "invalid cursor value: " + detail;
    }
}

}  // namespace api
}  // namespace agentenv
