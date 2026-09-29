// SPDX-License-Identifier: MIT
// Rust: src/p2p/transport.rs — the P2pTransport trait + byte stream.
//
// The Rust trait has six required methods and four provided (defaulted) ones.
// That split is reproduced exactly: required => pure virtual, provided =>
// virtual with the Rust default body inline.
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

    // ---- required methods (no Rust default body) ----

    /// Rust `lookup_with_hints`. `*out` is only written when the result is true
    /// (i.e. Rust `Ok(Some(descriptor))`).
    virtual P2pResult<bool>
        LookupWithHints(const P2pArtifactKey& key,
                        const std::vector<P2pArtifactProviderHint>& hints,
                        P2pArtifactDescriptor* out) = 0;

    /// Rust `fetch_with_options` — write the artifact to `destination`, return
    /// the byte count.
    virtual P2pResult<uint64_t>
        FetchWithOptions(const P2pArtifactDescriptor& descriptor,
                         const std::string& destination,
                         P2pFetchOptions options) = 0;

    /// Rust `fetch_bytes_with_options`.
    virtual P2pResult<std::vector<uint8_t> >
        FetchBytesWithOptions(const P2pArtifactDescriptor& descriptor,
                              P2pFetchOptions options) = 0;

    /// Rust `fetch_byte_range` — must yield exactly `len` bytes or end the
    /// stream with an error; a short read must not complete successfully.
    virtual P2pResult<std::shared_ptr<P2pByteStream> >
        FetchByteRange(const P2pArtifactDescriptor& descriptor,
                       uint64_t offset, size_t len) = 0;

    /// Rust `publish`. Disabled transports return Ok(()) so callers can treat
    /// P2P publishing as an optional acceleration path.
    virtual P2pResult<core::Unit> Publish(const P2pPublishRequest& request) = 0;

    /// Rust `unpublish` — true when a local publication was removed.
    virtual P2pResult<bool> Unpublish(const P2pArtifactKey& key) = 0;

    // ---- provided methods (Rust default bodies) ----

    /// Rust `lookup` => `self.lookup_with_hints(key, &[]).await`.
    virtual P2pResult<bool> Lookup(const P2pArtifactKey& key,
                                   P2pArtifactDescriptor* out) {
        return LookupWithHints(key, std::vector<P2pArtifactProviderHint>(), out);
    }

    /// Rust `fetch` => `fetch_with_options(.., P2pFetchOptions::default())`.
    virtual P2pResult<uint64_t> Fetch(const P2pArtifactDescriptor& descriptor,
                                      const std::string& destination) {
        return FetchWithOptions(descriptor, destination, P2pFetchOptions::Default());
    }

    /// Rust `fetch_bytes` => `fetch_bytes_with_options(.., default())`.
    virtual P2pResult<std::vector<uint8_t> >
        FetchBytes(const P2pArtifactDescriptor& descriptor) {
        return FetchBytesWithOptions(descriptor, P2pFetchOptions::Default());
    }

    /// Rust `local_endpoint` — default `None`, i.e. returns false.
    virtual bool LocalEndpoint(P2pEndpoint* /*out*/) const { return false; }

    /// Rust `shutdown` — default `Ok(())`.
    virtual P2pResult<core::Unit> Shutdown() { return core::Unit{}; }
};

/// Rust: `DisabledP2pTransport` — lookup yields None, publish/unpublish are
/// no-ops, and every byte-moving operation fails with `TransportDisabled`.
class DisabledP2pTransport : public P2pTransport {
 public:
    P2pResult<bool> LookupWithHints(const P2pArtifactKey&,
                                    const std::vector<P2pArtifactProviderHint>&,
                                    P2pArtifactDescriptor*) override {
        return false;  // Ok(None)
    }
    P2pResult<uint64_t> FetchWithOptions(const P2pArtifactDescriptor&,
                                         const std::string&,
                                         P2pFetchOptions) override {
        return core::make_unexpected(P2pError::MakeDisabled());
    }
    P2pResult<std::vector<uint8_t> >
        FetchBytesWithOptions(const P2pArtifactDescriptor&, P2pFetchOptions) override {
        return core::make_unexpected(P2pError::MakeDisabled());
    }
    P2pResult<std::shared_ptr<P2pByteStream> >
        FetchByteRange(const P2pArtifactDescriptor&, uint64_t, size_t) override {
        return core::make_unexpected(P2pError::MakeDisabled());
    }
    P2pResult<core::Unit> Publish(const P2pPublishRequest&) override {
        return core::Unit{};  // Ok(())
    }
    P2pResult<bool> Unpublish(const P2pArtifactKey&) override {
        return false;  // Ok(false)
    }
};

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_TRANSPORT_H_
