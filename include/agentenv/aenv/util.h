// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/{auth,grpc,output,progress,pty}.rs — CLI-side utils.
#ifndef AGENTENV_AENV_UTIL_H_
#define AGENTENV_AENV_UTIL_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace aenv {

// Rust: crates/aenv/src/auth.rs
namespace auth {
core::Expected<std::string, std::string> LoadToken(const std::string& profile);
core::Expected<core::Unit, std::string>  SaveToken(const std::string& profile,
                                                    const std::string& token);
}  // namespace auth

// Rust: crates/aenv/src/output.rs
namespace output {
void PrintTable(const std::string& csv_like);
void PrintJson(const std::string& json_text);
void PrintError(const std::string& msg);
}  // namespace output

// Rust: crates/aenv/src/progress.rs — indicatif-style progress bars.
namespace progress {
class Bar {
 public:
    virtual ~Bar() {}
    virtual void Update(uint64_t done, uint64_t total) = 0;
    virtual void Finish() = 0;
};
}  // namespace progress

// Rust: crates/aenv/src/pty.rs — TTY / pty helpers for `aenv connect`.
namespace pty {
core::Expected<core::Unit, std::string> AttachTty(int fd);
}  // namespace pty

// Rust: crates/aenv/src/grpc/mod.rs — optional gRPC transport for scheduler.
namespace grpc {
core::Expected<core::Unit, std::string> Ping(const std::string& endpoint);
}  // namespace grpc

}  // namespace aenv
}  // namespace agentenv
#endif  // AGENTENV_AENV_UTIL_H_
