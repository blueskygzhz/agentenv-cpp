// SPDX-License-Identifier: MIT
// Rust: src/p2p/iroh/*.rs — gated behind AGENTENV_WITH_IROH.
#include "agentenv/p2p/iroh.h"

namespace agentenv {
namespace p2p {

const char* const kIrohBackendIdValue = "iroh";

#ifdef AGENTENV_WITH_IROH
#error "AGENTENV_WITH_IROH is set but the iroh transport is not yet ported"
#endif

// Without the iroh stack every op is a Disabled error, and start() fails.
std::shared_ptr<IrohBlobsP2pTransport>
IrohBlobsP2pTransport::Start(const std::string& /*store_dir*/,
         const std::string& /*listen_addr*/,
 std::string* err) {
    if (err) *err = "iroh transport not compiled in (build with AGENTENV_WITH_IROH)";
    return std::shared_ptr<IrohBlobsP2pTransport>();
}

P2pResult<bool>
IrohBlobsP2pTransport::LookupWithHints(const P2pArtifactKey&,
      const std::vector<P2pArtifactProviderHint>&,
      P2pArtifactDescriptor*) {
  return core::make_unexpected(P2pError::MakeDisabled());
}
P2pResult<uint64_t>
IrohBlobsP2pTransport::FetchWithOptions(const P2pArtifactDescriptor&, const std::string&,
                                       P2pFetchOptions) {
    return core::make_unexpected(P2pError::MakeDisabled());
}
P2pResult<std::vector<uint8_t> >
IrohBlobsP2pTransport::FetchBytesWithOptions(const P2pArtifactDescriptor&, P2pFetchOptions) {
    return core::make_unexpected(P2pError::MakeDisabled());
}
P2pResult<std::shared_ptr<P2pByteStream> >
IrohBlobsP2pTransport::FetchByteRange(const P2pArtifactDescriptor&, uint64_t, size_t) {
    return core::make_unexpected(P2pError::MakeDisabled());
}
P2pResult<core::Unit>
IrohBlobsP2pTransport::Publish(const P2pPublishRequest&) {
    return core::make_unexpected(P2pError::MakeDisabled());
}
P2pResult<bool>
IrohBlobsP2pTransport::Unpublish(const P2pArtifactKey&) {
return core::make_unexpected(P2pError::MakeDisabled());
}
bool IrohBlobsP2pTransport::LocalEndpoint(P2pEndpoint*) const {
    return false;  // None
}

}  // namespace p2p
}  // namespace agentenv
