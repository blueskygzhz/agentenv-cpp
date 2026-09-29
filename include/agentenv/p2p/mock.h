// SPDX-License-Identifier: MIT
// Rust: src/p2p/mock.rs — MockTransport (in-memory transport for tests).
#ifndef AGENTENV_P2P_MOCK_H_
#define AGENTENV_P2P_MOCK_H_

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/p2p/transport.h"

namespace agentenv {
namespace p2p {

/// Rust: `MockTransport` — stores descriptors + blobs in memory, tracks call
/// counters, and supports forced failures, injected delays and chunked
/// byte-range streaming. Field-for-field with the Rust struct.
class MockTransport : public P2pTransport {
 public:
    MockTransport();

    P2pResult<bool> LookupWithHints(const P2pArtifactKey& key,
                                    const std::vector<P2pArtifactProviderHint>& hints,
                                    P2pArtifactDescriptor* out) override;
    P2pResult<uint64_t> FetchWithOptions(const P2pArtifactDescriptor& descriptor,
                                         const std::string& destination,
                                         P2pFetchOptions options) override;
    P2pResult<std::vector<uint8_t> >
        FetchBytesWithOptions(const P2pArtifactDescriptor& descriptor,
                              P2pFetchOptions options) override;
    P2pResult<std::shared_ptr<P2pByteStream> >
        FetchByteRange(const P2pArtifactDescriptor& descriptor,
                       uint64_t offset, size_t len) override;
    P2pResult<core::Unit> Publish(const P2pPublishRequest& request) override;
    P2pResult<bool> Unpublish(const P2pArtifactKey& key) override;
    bool LocalEndpoint(P2pEndpoint* out) const override;

    // ---- test knobs (mirror the Rust atomics/flags) ----
    int LookupCount() const;
    int FetchCount() const;
    int FetchBytesCount() const;
    int FetchRangeCount() const;
    int PublishCount() const;
    /// Rust `unpublish_count`.
    int UnpublishCount() const;
    /// Rust `unpublished_keys` — every key passed to unpublish(), in order,
    /// recorded even when nothing was removed.
    std::vector<P2pArtifactKey> UnpublishedKeys() const;

    void SetFailLookup(bool v);
    void SetFailPublish(bool v);
    void SetFailRangeStreamAfterFirstChunk(bool v);
    /// Rust `lookup_delay: Option<Duration>`.
    void SetLookupDelayMs(int ms);
    /// Rust `fetch_range_delay: Option<Duration>`.
    void SetFetchRangeDelayMs(int ms);

    /// Rust `options` captured by the last fetch call — lets tests assert that
    /// the defaulted `advertise: true` is threaded through.
    P2pFetchOptions LastFetchOptions() const;

    bool HasBlob(const P2pArtifactKey& key) const;

 private:
    mutable std::mutex mu_;
    std::map<P2pArtifactKey, P2pArtifactDescriptor> descriptors_;
    std::map<P2pArtifactKey, std::vector<uint8_t> > blobs_;
    int lookup_count_;
    int fetch_count_;
    int fetch_bytes_count_;
    int fetch_range_count_;
    int publish_count_;
    int unpublish_count_;
    std::vector<P2pArtifactKey> unpublished_keys_;
    bool fail_lookup_;
    bool fail_publish_;
    bool fail_range_after_first_;
    int  lookup_delay_ms_;
    int  fetch_range_delay_ms_;
    P2pFetchOptions last_fetch_options_;
};

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_MOCK_H_
