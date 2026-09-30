// SPDX-License-Identifier: MIT
// Rust: src/api/dto/*.rs  (subset of E2B-compatible schema in openapi.yml)
#ifndef AGENTENV_API_DTO_H_
#define AGENTENV_API_DTO_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"

namespace agentenv {
namespace api {

/// POST /sandboxes  request body.
struct CreateSandboxReq {
    std::string template_id;
    std::vector<std::string> env_vars;
    int64_t     timeout_ms = 300000;

    core::Json ToJson() const;
    static core::Expected<CreateSandboxReq, core::AnyError> FromJson(const core::Json& j);
};

/// POST /sandboxes  response body.
struct CreateSandboxResp {
    std::string sandbox_id;
    std::string template_id;
    std::string state;
    int64_t     created_at_ms = 0;

    core::Json ToJson() const;
    static core::Expected<CreateSandboxResp, core::AnyError> FromJson(const core::Json& j);
};

/// GET /sandboxes/{id}  response body.
struct GetSandboxResp {
    std::string sandbox_id;
    std::string state;
    std::string template_id;
    int64_t     created_at_ms = 0;
    int64_t     started_at_ms = 0;

    core::Json ToJson() const;
    static core::Expected<GetSandboxResp, core::AnyError> FromJson(const core::Json& j);
};

/// GET /sandboxes  response body.
struct ListSandboxesResp {
    std::vector<GetSandboxResp> sandboxes;
    core::Json ToJson() const;
    static core::Expected<ListSandboxesResp, core::AnyError> FromJson(const core::Json& j);
};

/// POST /sandboxes/{id}/exec  request body.
struct ExecReq {
    std::vector<std::string> cmd;
    std::vector<std::string> env_vars;
    int32_t timeout_sec = 60;

    core::Json ToJson() const;
    static core::Expected<ExecReq, core::AnyError> FromJson(const core::Json& j);
};

/// POST /sandboxes/{id}/exec  response body.
struct ExecResp {
    int32_t     exit_code = 0;
    std::string stdout_output;
    std::string stderr_output;

    core::Json ToJson() const;
    static core::Expected<ExecResp, core::AnyError> FromJson(const core::Json& j);
};

/// Generic error body.
struct ErrorResp {
    std::string code;
    std::string message;
    core::Json ToJson() const;
};

/// Rust generated `models::Error` — the `{code, message}` body every handler
/// returns on failure. `code` is the HTTP status, mirroring
/// `models::Error::new(400, ...)` upstream.
struct ApiError {
    int         code = 500;
    std::string message;

    static ApiError Make(int code, const std::string& message);

    bool operator==(const ApiError& o) const {
        return code == o.code && message == o.message;
    }
    bool operator!=(const ApiError& o) const { return !(*this == o); }

    core::Json ToJson() const;
};

}  // namespace api
}  // namespace agentenv
#endif  // AGENTENV_API_DTO_H_
