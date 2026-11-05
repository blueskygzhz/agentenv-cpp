// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/client.rs — the half of `OssClient` the
// shared document layer uses.
//
// Rust reaches object storage through `opendal`. This is the same set of
// operations issued directly as S3 REST calls, reusing the pieces the
// overlaybd OSS backend already has: SigV4 signing
// (`storage/overlaybd/s3_signer.h`), addressing-style detection and the
// libcurl transport.
//
// Deliberately *not* built on `storage/overlaybd/oss.h`'s `OssBackend`: that
// object is shaped for reading image layers (ranged GET, HEAD, multipart
// upload) and has no conditional write, no delete and no listing — which are
// exactly the three operations a shared catalog needs. Extending it would
// have mixed a read-optimised layer cache with a consistency-critical
// metadata path.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_OSS_CLIENT_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_OSS_CLIENT_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/repository/oss/object_store.h"
#include "agentenv/storage/overlaybd/http_transport.h"
#include "agentenv/storage/overlaybd/oss.h"
#include "agentenv/storage/overlaybd/s3_signer.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {

/// Rust `NormalizedOssConfig` — `backend.oss` validated once, so an
/// incomplete configuration fails at construction rather than on the first
/// request.
struct OssClientConfig {
    std::string bucket;
    std::string endpoint;
    std::string region;
    /// Trimmed of surrounding `/`, empty when unset. Every key is written
    /// under it.
    std::string prefix;
    storage::overlaybd::S3Credentials credentials;
    /// Unset means the style is detected from the endpoint.
    core::Optional<storage::overlaybd::AddressingStyle> addressing_style;

    long timeout_secs;
    /// Retries *per pass* on a transport failure or 5xx, as the overlaybd
    /// backend does.
    uint32_t retry_count;

    OssClientConfig() : timeout_secs(30), retry_count(2) {}

    /// Rust `NormalizedOssConfig::new`.
    ///
    /// `bucket`, `endpoint` and `region` are required and blank-is-absent: a
    /// field of spaces is a config that was never filled in, and sending it
    /// produces a 403 whose cause is invisible in the config file.
    ///
    /// A `credential_process` is refused rather than ignored: degrading to
    /// anonymous would fail later as a confusing 403 on a private bucket.
    static RepositoryResult<OssClientConfig> FromBackendConfig(const cfg::OssBackendConfig& cfg);
};

/// Rust `is_aliyun_host`.
bool IsAliyunHost(const std::string& host);

/// A response-body sink that can start over.
///
/// What makes a streamed download retryable: an attempt that failed halfway
/// has already written part of the file, and letting the next attempt append
/// to it would splice two responses into one object that is the right length
/// and the wrong bytes.
class RetryableSink : public storage::overlaybd::http::BodySink {
 public:
    /// Discards everything written so far. Called before each attempt, so a
    /// sink that was never written to must accept it too.
    virtual core::Expected<core::Unit, std::string> Restart() = 0;
};

/// An `ObjectStore` over the S3 REST API.
class OssClient : public ObjectStore {
 public:
    /// The transport call, injectable so the request construction and status
    /// handling can be tested without a network.
    ///
    /// This is the seam the tests use: what matters about this class is the
    /// requests it builds (a conditional header, a sorted canonical query)
    /// and how it reads the answers (412 is a lost race, 404 is absence) —
    /// neither of which needs a server to observe.
    typedef std::function<core::Expected<storage::overlaybd::http::Response, std::string>(
        const storage::overlaybd::http::Request&)>
        Transport;

    static RepositoryResult<OssClient*> New(const OssClientConfig& config);
    /// Same, with the transport replaced.
    static RepositoryResult<OssClient*> NewWithTransport(const OssClientConfig& config,
                                                         const Transport& transport);

    // ---- ObjectStore ----------------------------------------------------

    /// True for Alibaba's native OSS endpoints, which cannot write
    /// conditionally. The document layer refuses those rather than running a
    /// compare-and-swap whose precondition the store ignores.
    bool CanAppend() const override { return can_append_; }

    /// One `GET`, not Rust's `stat` + conditional `read`.
    ///
    /// A single response carries both the bytes and the `ETag` that describes
    /// them, so they cannot disagree — the two-step form exists upstream only
    /// because opendal's `read` does not return metadata, and it is precisely
    /// what makes `ObjectRead::Changed` reachable there. This client
    /// therefore never reports `Changed`; the state stays in the interface
    /// because the document layer must still handle a store that does.
    RepositoryResult<ObjectRead> GetWithEtag(const std::string& key) const override;

    RepositoryResult<bool> PutConditionally(const std::string& key, const std::string& bytes,
                                            const core::Optional<std::string>& etag) override;
    RepositoryResult<core::Unit> Put(const std::string& key, const std::string& bytes) override;
    RepositoryResult<core::Unit> Delete(const std::string& key) override;
    RepositoryResult<std::vector<std::string> > ListKeysPage(
        const std::string& prefix, const core::Optional<std::string>& start_after,
        std::size_t limit) const override;

    /// `prefix` applied to a document path. Exposed for assertions.
    std::string FullKey(const std::string& key) const;

    // ---- the blob half (Rust `exists` / `get_to_file` / `object_url`) ----
    //
    // Separate from `ObjectStore`, which is the document layer's seam: a
    // document is small and is read into memory, while these three exist
    // precisely because a layer is not.

    /// Rust `exists` — a `HEAD`, so probing a multi-gigabyte layer costs one
    /// round trip rather than its contents.
    RepositoryResult<bool> Exists(const std::string& key) const;

    /// Rust `get_to_file` — streams one object into `destination`, returning
    /// the byte count.
    ///
    /// Streamed rather than read into memory: `FetchBlob`'s callers fetch
    /// layers. Three failure modes are handled rather than left to surface as
    /// a corrupt file: the destination is not created at all unless bytes
    /// arrive (so a 404 leaves no empty file behind), a retried attempt starts
    /// the file over instead of appending to the previous one, and a response
    /// shorter than its own `Content-Length` is refused with the partial file
    /// removed.
    ///
    /// `ArtifactNotFound` when the object is absent. Upstream reports that as
    /// a backend error; this port follows `SnapshotRepository::FetchBlob`'s
    /// documented contract instead, which the POSIX backend already honours —
    /// a caller distinguishing "not published yet" from "transport broken"
    /// must get the same answer from both backends.
    RepositoryResult<uint64_t> GetToFile(const std::string& key,
                                         const std::string& destination) const;

    /// Rust `delete_prefix` — deletes every object under `prefix`.
    ///
    /// Lists recursively, then deletes one key at a time. Upstream runs 16
    /// deletes concurrently; these are serial, which costs latency on a
    /// snapshot with many files but removes the same set.
    ///
    /// Not a bulk `DeleteObjects` request: that returns per-key errors inside
    /// a 200 response, so treating the status as the outcome would report a
    /// partial delete as a complete one.
    RepositoryResult<core::Unit> DeletePrefix(const std::string& prefix);

    /// Rust `object_url` — `s3://<bucket>/<prefix>/<key>`.
    ///
    /// The same spelling the overlaybd OSS backend parses, so a located blob
    /// and an uploaded one are named identically.
    std::string ObjectUrl(const std::string& key) const;

 private:
    OssClient() : can_append_(false) {}

    /// Signs and sends one request, retrying a transport failure or 5xx and
    /// re-signing a 403 once (a stale timestamp or just-rotated token).
    ///
    /// `sink`, when given, receives the body of a successful response instead
    /// of `Response::body`, and is restarted before every attempt.
    core::Expected<storage::overlaybd::http::Response, std::string> Send(
        const std::string& method, const std::string& key, const std::string& canonical_query,
        const storage::overlaybd::http::HeaderMap& extra_headers, const std::string& body,
        RetryableSink* sink = 0) const;

    /// The location of `key` under this client's bucket and style.
    storage::overlaybd::ParsedOssUrl LocationOf(const std::string& key) const;

    OssClientConfig config_;
    storage::overlaybd::AddressingStyle style_;
    bool can_append_;
    Transport transport_;
};

/// Every `<Contents><Key>` in a ListObjectsV2 response, in document order.
///
/// Separate from the single-element helper the overlaybd backend has: a
/// listing is a repeated element, and taking only the first would silently
/// truncate every page to one entry.
std::vector<std::string> ParseListObjectsKeys(const std::string& xml);

/// Whether a ListObjectsV2 response says the page was truncated.
bool ParseListObjectsTruncated(const std::string& xml);

}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_OSS_CLIENT_H_
