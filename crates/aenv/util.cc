// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/{auth,grpc,output,progress,pty}.rs — thin skeleton.
#include "agentenv/aenv/util.h"

#include <cstdio>

namespace agentenv { namespace aenv {

namespace auth {
core::Expected<std::string, std::string> LoadToken(const std::string&) {
    return std::string();  // empty == no token
}
core::Expected<core::Unit, std::string> SaveToken(const std::string&, const std::string&) {
    return core::Unit{};
}
}  // namespace auth

namespace output {
void PrintTable(const std::string& s) { std::printf("%s\n", s.c_str()); }
void PrintJson(const std::string& s)  { std::printf("%s\n", s.c_str()); }
void PrintError(const std::string& s) { std::fprintf(stderr, "error: %s\n", s.c_str()); }
}  // namespace output

namespace pty {
core::Expected<core::Unit, std::string> AttachTty(int) {
    return core::Unit{};   // TODO: openpty(3) + raw mode toggle
}
}  // namespace pty

namespace grpc {
core::Expected<core::Unit, std::string> Ping(const std::string&) {
    return core::make_unexpected(std::string("grpc not compiled in"));
}
}  // namespace grpc

}}  // namespace agentenv::aenv
