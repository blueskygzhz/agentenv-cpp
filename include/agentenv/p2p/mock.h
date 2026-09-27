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
/// counters, and supports forced failures + chunked byte-range streaming.
class MockTransport : public P2pTransport {
 public:
    MockTransport();

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

    // ---- test knobs (mirror the Rust atomics/flags) ----
    int LookupCount() const { return lookup_count_; }
    int FetchCount() const { return fetch_count_; }
    int FetchBytesCount() const { return fetch_bytes_count_; }
    int FetchRangeCount() const { return fetch_range_count_; }
    int PublishCount() const { return publish_count_; }

    void SetFailLookup(bool v) { fail_lookup_ = v; }
    void SetFailPublish(bool v) { fail_publish_ = v; }
    void SetFailRangeStreamAfterFirstChunk(bool v) { fail_range_after_first_ = v; }

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
    bool fail_lookup_;
    bool fail_publish_;
    bool fail_range_after_first_;
};

}  // namespace p2p
}  // namespace agentenv
#endif  // AGENTENV_P2P_MOCK_H_
