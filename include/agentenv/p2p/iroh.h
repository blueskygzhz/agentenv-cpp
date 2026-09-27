// SPDX-License-Identifier: MIT
// Rust: src/p2p/iroh/{mod,transport,catalog,endpoint}.rs — the Iroh-blobs
// backed P2pTransport. The real implementation needs the iroh QUIC + blobs
// stack, so this is gated on AGENTENV_WITH_IROH. Without it, every operation
// returns a Disabled error (matching a transport that is not compiled in).
#ifndef AGENTENV_P2P_IROH_H_
#define AGENTENV_P2P_IROH_H_

#include <memory>
#include <string>

#include "agentenv/p2p/transport.h"

namespace agentenv {
namespace p2p {

/// Rust: iroh::IROH_BACKEND_ID.
extern const char* const kIrohBackendIdValue;

/// Rust: iroh::IrohBlobsP2pTransport.
class IrohBlobsP2pTransport : public P2pTransport {
 public:
    /// Rust `IrohBlobsP2pTransport::start(store_dir, listen_addr)`.
    /// Returns null + fills `err` when the iroh backend is not compiled in.
    static std::shared_ptr<IrohBlobsP2pTransport>
        Start(const std::string& store_dir, const std::string& listen_addr,
     std::string* err);

    P2pResult<bool> LookupWithHints(const P2pArtifactKey& key,
     const std::vector<P2pArtifactProviderHint>& hints,
      P2pArtifactDescriptor* out) override;
    P2pResult<uint64_t> Fetch(const P2pArtifactDescriptor& descriptor,
      const std::string& destination) override;
    P2pResult<std::vector<uint8_t> > FetchBytes(const P2pArtifactDescriptor& descriptor) override;
    P2pResult<std::shared_ptr<P2pByteStream> >
        FetchByteRange(const P2pArtifactDescriptor& descriptor,
     uint64_t offset, size_t len) override;
    P2pResult<core::Unit> Publish(const P2pPublishRequest& request) override;
    P2pResult<bool> Unpublish(const P2pArtifactKey& key) override;
    bool LocalEndpoint(P2pEndpoint* out) const override;
};

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_IROH_H_
