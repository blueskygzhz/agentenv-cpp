// SPDX-License-Identifier: MIT
// Rust: src/p2p/transport.rs — the P2pTransport trait + byte stream.
#ifndef AGENTENV_P2P_TRANSPORT_H_
#define AGENTENV_P2P_TRANSPORT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/p2p/error.h"
#include "agentenv/p2p/types.h"

namespace agentenv {
namespace p2p {

/// Rust: `P2pByteStream` — a stream of Bytes chunks (each Next() yields a chunk).
class P2pByteStream {
 public:
    virtual ~P2pByteStream() {}
    /// Returns the next chunk. On success with `has_chunk==false`, the stream is
    /// exhausted. On error the stream is aborted.
    virtual P2pResult<bool> Next(std::vector<uint8_t>* chunk_out) = 0;
};

/// Rust: `P2pTransport` trait.
class P2pTransport {
 public:
    virtual ~P2pTransport() {}

    /// Rust `lookup_with_hints`.
    virtual P2pResult<bool>
     LookupWithHints(const P2pArtifactKey& key,
   const std::vector<P2pArtifactProviderHint>& hints,
   P2pArtifactDescriptor* out) = 0;

    /// Rust `fetch` — write the whole artifact to `destination`, return byte count.
    virtual P2pResult<uint64_t>
        Fetch(const P2pArtifactDescriptor& descriptor, const std::string& destination) = 0;

    /// Rust `fetch_bytes`.
    virtual P2pResult<std::vector<uint8_t> >
        FetchBytes(const P2pArtifactDescriptor& descriptor) = 0;

    /// Rust `fetch_byte_range`.
    virtual P2pResult<std::shared_ptr<P2pByteStream> >
        FetchByteRange(const P2pArtifactDescriptor& descriptor,
        uint64_t offset, size_t len) = 0;

    /// Rust `publish`.
    virtual P2pResult<core::Unit> Publish(const P2pPublishRequest& request) = 0;

    /// Rust `unpublish` — returns whether an entry was removed.
    virtual P2pResult<bool> Unpublish(const P2pArtifactKey& key) = 0;

    /// Rust `local_endpoint` — None represented by has==false.
    virtual bool LocalEndpoint(P2pEndpoint* out) const = 0;
};

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_TRANSPORT_H_
