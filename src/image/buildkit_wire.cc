// SPDX-License-Identifier: MIT
// Rust: the tonic channels in src/image/buildkit.rs and buildkit/history.rs.
//
// **This tree has no gRPC transport at all**, so these two factories always
// fail. Connecting fails explicitly rather than pretending to reach BuildKit,
// and every caller surfaces it as an error. Tests and embedders use the
// `BuildkitContent` / `BuildkitHistory` constructors that take a wire
// directly, which is how the wire protocol above is exercised.
//
// Note on `AGENTENV_WITH_GRPC`: turning that option on does **not** produce a
// working wire here. There is no `#ifdef`-guarded second implementation of
// these functions -- tonic's counterpart would be grpc++, which needs
// protobuf >= 3 and `grpc_cpp_plugin` to generate the BuildKit service stubs,
// and this tree carries neither. The option gates the scheduler/gateway
// surface, and even there it only prints
// "AGENTENV_WITH_GRPC enabled but the gRPC transport is not ported"
// (`services/scheduler/CMakeLists.txt`). An earlier version of this comment
// said the transport was "gated on AGENTENV_WITH_GRPC", which read as "flip
// the switch and it works" -- it does not, and nothing in the build says so
// at this file.
//
// Consequence worth stating plainly: `DefaultImageResolver::ResolveBuildkit`
// is therefore unreachable in production, and nothing calls it either -- see
// the note on its declaration in `image/resolver.h`.
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
