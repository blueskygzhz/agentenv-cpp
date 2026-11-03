// SPDX-License-Identifier: MIT
// Rust: the tonic channels in src/image/buildkit.rs and buildkit/history.rs.
//
// This build has no gRPC transport: tonic's counterpart here is grpc++, which
// is gated on AGENTENV_WITH_GRPC. Connecting therefore fails explicitly instead
// of pretending to reach BuildKit, and every caller surfaces it as an error.
// Tests and embedders use the `BuildkitContent` / `BuildkitHistory` constructors
// that take a wire directly.
#include "agentenv/image/buildkit.h"

namespace agentenv {
namespace image {

namespace {

std::string NoTransport(const std::string& host, uint16_t port) {
    return "cannot reach BuildKit at " + host + ":" + std::to_string(port) +
           ": this build has no gRPC transport";
}

}  // namespace

core::Expected<std::shared_ptr<BuildkitContentWire>, std::string> MakeBuildkitContentWire(
    const std::string& host, uint16_t port) {
    return core::make_unexpected(NoTransport(host, port));
}

core::Expected<std::shared_ptr<BuildkitControlWire>, std::string> MakeBuildkitControlWire(
    const std::string& host, uint16_t port) {
    return core::make_unexpected(NoTransport(host, port));
}

}  // namespace image
}  // namespace agentenv
