// SPDX-License-Identifier: MIT
// Rust: src/p2p/mock.rs — faithful in-memory MockTransport.
#include "agentenv/p2p/mock.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

namespace agentenv {
namespace p2p {

namespace {
// A chunked byte stream over an owned buffer (mirrors the Rust 1024-byte chunks,
// with an optional forced failure after the first chunk).
class ChunkStream : public P2pByteStream {
 public:
    ChunkStream(std::vector<uint8_t> data, bool fail_after_first)
        : data_(std::move(data)), pos_(0), fail_after_first_(fail_after_first),
          chunks_yielded_(0) {}

    P2pResult<bool> Next(std::vector<uint8_t>* chunk_out) override {
        if (fail_after_first_ && chunks_yielded_ == 1) {
            return core::make_unexpected(P2pError::MakeInternal("forced range stream failure"));
        }
        if (pos_ >= data_.size()) {
            chunk_out->clear();
            return false;  // exhausted
        }
        const size_t kChunk = 1024;
        size_t end = pos_ + kChunk;
        if (end > data_.size()) end = data_.size();
        chunk_out->assign(data_.begin() + pos_, data_.begin() + end);
        pos_ = end;
        ++chunks_yielded_;
        return true;
    }

 private:
    std::vector<uint8_t> data_;
    size_t pos_;
    bool   fail_after_first_;
    int    chunks_yielded_;
};

// Rust `tokio::time::sleep(delay)` for the injected Option<Duration> knobs.
void SleepMs(int ms) {
    if (ms <= 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
}  // namespace

MockTransport::MockTransport()
    : lookup_count_(0), fetch_count_(0), fetch_bytes_count_(0),
      fetch_range_count_(0), publish_count_(0), unpublish_count_(0),
      fail_lookup_(false), fail_publish_(false), fail_range_after_first_(false),
      lookup_delay_ms_(0), fetch_range_delay_ms_(0) {}

P2pResult<bool>
MockTransport::LookupWithHints(const P2pArtifactKey& key,
                               const std::vector<P2pArtifactProviderHint>& /*hints*/,
                               P2pArtifactDescriptor* out) {
    // Rust bumps the counter, then sleeps *outside* the store lock.
    int delay_ms = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        ++lookup_count_;
        delay_ms = lookup_delay_ms_;
    }
    SleepMs(delay_ms);

    std::lock_guard<std::mutex> g(mu_);
    if (fail_lookup_) {
        return core::make_unexpected(P2pError::MakeInternal("forced lookup failure"));
    }
    std::map<P2pArtifactKey, P2pArtifactDescriptor>::iterator it = descriptors_.find(key);
    if (it == descriptors_.end()) {
        return false;  // Ok(None)
    }
    if (out) *out = it->second;
    return true;  // Ok(Some)
}

P2pResult<uint64_t>
MockTransport::FetchWithOptions(const P2pArtifactDescriptor& descriptor,
                                const std::string& destination,
                                P2pFetchOptions options) {
    std::lock_guard<std::mutex> g(mu_);
    ++fetch_count_;
    last_fetch_options_ = options;
    std::map<P2pArtifactKey, std::vector<uint8_t> >::iterator it = blobs_.find(descriptor.key);
    if (it == blobs_.end()) {
        return core::make_unexpected(P2pError::Invalid("missing test blob"));
    }
    std::ofstream f(destination.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) {
        return core::make_unexpected(P2pError::MakeInternal("write mock fetch failed"));
    }
    if (!it->second.empty()) {
        f.write(reinterpret_cast<const char*>(&it->second[0]),
                static_cast<std::streamsize>(it->second.size()));
    }
    return static_cast<uint64_t>(it->second.size());
}

P2pResult<std::vector<uint8_t> >
MockTransport::FetchBytesWithOptions(const P2pArtifactDescriptor& descriptor,
                                     P2pFetchOptions options) {
    std::lock_guard<std::mutex> g(mu_);
    ++fetch_bytes_count_;
    last_fetch_options_ = options;
    std::map<P2pArtifactKey, std::vector<uint8_t> >::iterator it = blobs_.find(descriptor.key);
    if (it == blobs_.end()) {
        return core::make_unexpected(P2pError::Invalid("missing test blob"));
    }
    return it->second;
}

P2pResult<std::shared_ptr<P2pByteStream> >
MockTransport::FetchByteRange(const P2pArtifactDescriptor& descriptor,
                              uint64_t offset, size_t len) {
    int delay_ms = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        ++fetch_range_count_;
        delay_ms = fetch_range_delay_ms_;
    }
    SleepMs(delay_ms);

    std::lock_guard<std::mutex> g(mu_);
    std::map<P2pArtifactKey, std::vector<uint8_t> >::iterator it = blobs_.find(descriptor.key);
    if (it == blobs_.end()) {
        return core::make_unexpected(P2pError::Invalid("missing test blob"));
    }
    const std::vector<uint8_t>& bytes = it->second;
    // Rust: usize::try_from(offset) — fails only on 32-bit overflow.
    if (offset > static_cast<uint64_t>(static_cast<size_t>(-1))) {
        return core::make_unexpected(P2pError::Invalid("invalid offset"));
    }
    size_t start = static_cast<size_t>(offset);
    // Rust: start.checked_add(len).
    size_t end = start + len;
    if (end < start) {
        return core::make_unexpected(P2pError::Invalid("range overflow"));
    }
    if (end > bytes.size()) {
        return core::make_unexpected(P2pError::Invalid("range outside blob"));
    }
    std::vector<uint8_t> slice(bytes.begin() + start, bytes.begin() + end);
    std::shared_ptr<P2pByteStream> stream(
        new ChunkStream(std::move(slice), fail_range_after_first_));
    return stream;
}

P2pResult<core::Unit>
MockTransport::Publish(const P2pPublishRequest& request) {
    std::lock_guard<std::mutex> g(mu_);
    ++publish_count_;
    if (fail_publish_) {
        return core::make_unexpected(P2pError::MakeInternal("forced publish failure"));
    }
    std::vector<uint8_t> bytes;
    if (request.source.kind == P2pPublishSource::Path) {
        std::ifstream f(request.source.path.c_str(), std::ios::binary);
        if (!f) {
            return core::make_unexpected(P2pError::MakeInternal("read mock publish failed"));
        }
        f.seekg(0, std::ios::end);
        std::streamoff sz = f.tellg();
        f.seekg(0, std::ios::beg);
        bytes.resize(static_cast<size_t>(sz < 0 ? 0 : sz));
        if (!bytes.empty()) f.read(reinterpret_cast<char*>(&bytes[0]),
                                   static_cast<std::streamsize>(bytes.size()));
    } else {
        bytes = request.source.bytes;
    }

    P2pArtifactDescriptor d;
    d.key = request.key;
    d.providers.push_back(P2pArtifactProvider::MakeLocal());
    d.has_backend_locator = true;
    d.backend_locator = "mock";
    d.metadata_json = request.metadata_json;

    descriptors_[request.key] = d;
    blobs_[request.key] = bytes;
    return core::Unit{};
}

P2pResult<bool>
MockTransport::Unpublish(const P2pArtifactKey& key) {
    std::lock_guard<std::mutex> g(mu_);
    // Rust records the key first, unconditionally, then reports removal.
    ++unpublish_count_;
    unpublished_keys_.push_back(key);
    bool removed = descriptors_.erase(key) > 0;
    blobs_.erase(key);
    return removed;
}

bool MockTransport::LocalEndpoint(P2pEndpoint* out) const {
    if (out) {
        out->backend = "mock";
        out->address = "mock";
    }
    return true;
}

int MockTransport::LookupCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return lookup_count_;
}
int MockTransport::FetchCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return fetch_count_;
}
int MockTransport::FetchBytesCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return fetch_bytes_count_;
}
int MockTransport::FetchRangeCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return fetch_range_count_;
}
int MockTransport::PublishCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return publish_count_;
}
int MockTransport::UnpublishCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return unpublish_count_;
}
std::vector<P2pArtifactKey> MockTransport::UnpublishedKeys() const {
    std::lock_guard<std::mutex> g(mu_);
    return unpublished_keys_;
}
void MockTransport::SetFailLookup(bool v) {
    std::lock_guard<std::mutex> g(mu_);
    fail_lookup_ = v;
}
void MockTransport::SetFailPublish(bool v) {
    std::lock_guard<std::mutex> g(mu_);
    fail_publish_ = v;
}
void MockTransport::SetFailRangeStreamAfterFirstChunk(bool v) {
    std::lock_guard<std::mutex> g(mu_);
    fail_range_after_first_ = v;
}
void MockTransport::SetLookupDelayMs(int ms) {
    std::lock_guard<std::mutex> g(mu_);
    lookup_delay_ms_ = ms;
}
void MockTransport::SetFetchRangeDelayMs(int ms) {
    std::lock_guard<std::mutex> g(mu_);
    fetch_range_delay_ms_ = ms;
}
P2pFetchOptions MockTransport::LastFetchOptions() const {
    std::lock_guard<std::mutex> g(mu_);
    return last_fetch_options_;
}

bool MockTransport::HasBlob(const P2pArtifactKey& key) const {
    std::lock_guard<std::mutex> g(mu_);
    return blobs_.find(key) != blobs_.end();
}

}  // namespace p2p
}  // namespace agentenv
