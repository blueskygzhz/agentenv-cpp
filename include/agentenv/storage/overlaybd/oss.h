// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/oss.rs — reading objects from S3/OSS.
#ifndef AGENTENV_STORAGE_OVERLAYBD_OSS_H_
#define AGENTENV_STORAGE_OVERLAYBD_OSS_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/s3_signer.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `DEFAULT_TIMEOUT` / `DEFAULT_RETRY_COUNT`.
///
/// Substituted when the config says zero. Rust treats zero as "unset" rather
/// than literal, and it has to: a zero timeout means "no limit" to the
/// transport, and zero attempts would make every transient failure fatal.
const uint64_t kOssDefaultTimeoutSecs = 30;
const uint32_t kOssDefaultRetryCount = 3;

/// Rust `AddressingStyle`.
enum AddressingStyle {
    /// `https://<bucket>.<endpoint-host>/<key>`.
    kAddressingVirtual,
    /// `https://<endpoint-host>/<bucket>/<key>`.
    kAddressingPath
};

/// Rust `parse_addressing_style` — `"virtual"`, `"path"`, or empty for
/// auto-detection.
///
/// Empty yields no override, which is distinct from either style: the choice
/// then comes from the endpoint, per URL.
core::Expected<core::Optional<AddressingStyle>, std::string> ParseAddressingStyle(
    const std::string& value);

/// Rust `detect_addressing_style`.
///
/// Virtual when the endpoint host already starts with the bucket name (a
/// CNAME that is only valid for that bucket), and virtual for Aliyun's own
/// endpoints, which require it. Everything else defaults to path style, which
/// any S3-compatible server understands.
core::Expected<AddressingStyle, std::string> DetectAddressingStyle(
    const std::string& endpoint, const std::string& bucket);

/// Rust `ParsedOssUrl` — an `oss://` or `s3://` URL resolved against config.
struct ParsedOssUrl {
    std::string bucket;
    std::string key;
    std::string region;
    std::string endpoint;
    AddressingStyle addressing_style;

    ParsedOssUrl() : addressing_style(kAddressingPath) {}

    /// Rust `ParsedOssUrl::parse`.
    ///
    /// The scheme must be `oss` or `s3`; the host is the bucket and the path
    /// is the key. `?endpoint=` and `?region=` in the URL win over the
    /// config, so one image can name a bucket in another region.
    static core::Expected<ParsedOssUrl, std::string> Parse(
        const std::string& raw, const std::string& default_endpoint,
        const std::string& default_region,
        const core::Optional<AddressingStyle>& addressing_override);

    /// The request URL for this object.
    std::string ObjectUrl() const;
    /// The `Host` header value, which the signature covers.
    std::string Host() const;
    /// The percent-encoded canonical path the signature covers.
    std::string CanonicalUri() const;
};

class OssBackend;

/// Rust `OssFile` — one object, read-only.
///
/// Reads become ranged GETs. The size is fetched once with a HEAD and cached:
/// an object is immutable for the lifetime of a handle, and re-probing it per
/// read would double the request count.
class OssFile : public VirtualFile {
 public:
    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                     size_t len) override;
    /// Always refused — Rust directs writes to `UploadBytes` instead.
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                  size_t len) override;
    /// Rust `size` / `read_at` — thin measuring wrappers over the
    /// matching `*Inner`, which hold the actual request logic.
    core::Expected<uint64_t, std::string> Size() override;

 private:
    core::Expected<uint64_t, std::string> SizeInner();
    core::Expected<ByteBuffer, std::string> ReadAtInner(uint64_t offset, size_t len);

    friend class OssBackend;

    OssFile() : backend_(0), size_known_(false), size_(0) {}
    OssFile(const OssFile&);
    OssFile& operator=(const OssFile&);

    OssBackend* backend_;
    ParsedOssUrl location_;
    bool size_known_;
    uint64_t size_;
};

/// Rust `OssBackend`.
class OssBackend {
 public:
    /// Rust `OssBackend::new`.
    ///
    /// Credentials may be absent, which yields unsigned requests for public
    /// buckets rather than an error.
    static core::Expected<std::shared_ptr<OssBackend>, std::string> New(
        const OssConfig& config);

    /// Rust `open_file_with_size_hint` / `open_with_size_hint`.
    ///
    /// A non-zero `size_hint` skips the HEAD. The caller usually knows the
    /// size from a manifest, and the object cannot change under it.
    core::Expected<std::shared_ptr<OssFile>, std::string> OpenWithSizeHint(
        const std::string& url, uint64_t size_hint);

    /// Rust `object_size` — `nothing` when the object does not exist.
    ///
    /// A 404 is an answer, not a failure: callers probe for an object they
    /// may still have to upload.
    core::Expected<core::Optional<uint64_t>, std::string> ObjectSize(const std::string& url);

    /// Rust `upload_bytes`.
    core::Expected<core::Unit, std::string> UploadBytes(const std::string& url,
                                                           const uint8_t* data, size_t len);

    /// Rust `upload_path` — upload a local file as one object.
    ///
    /// Multipart, because a sealed upper layer can exceed the 5 GiB
    /// single-PUT ceiling and because a single PUT would need the whole object
    /// in memory. The object appears whole or not at all: an abandoned upload
    /// is aborted rather than left to be billed as orphaned parts.
    ///
    /// `part_size` is raised to the 5 MiB protocol floor when smaller.
    /// `progress`, when given, is reset to zero and then advanced by each
    /// acknowledged part.
    core::Expected<core::Unit, std::string> UploadPath(const std::string& url,
                                                          const std::string& path,
                                                          size_t part_size, size_t concurrency,
                                                          std::atomic<uint64_t>* progress);

 private:
    friend class OssFile;

    OssBackend() {}
    OssBackend(const OssBackend&);
    OssBackend& operator=(const OssBackend&);

    core::Expected<ParsedOssUrl, std::string> ParseUrl(const std::string& url) const;

    /// One request, signed and retried.
    ///
    /// Retries only on a transport failure or 5xx: a 4xx is the server's
    /// final answer, and repeating it would turn a wrong key into a long
    /// stall. Each attempt is re-signed, because the signature covers a
    /// timestamp that the server only accepts within a window.
    /// `sink` null accumulates the body into the response; non-null streams
    /// it there instead, so a large ranged read is not held whole.
    /// Sign one request. Separated from `Send` because a 403 retry re-signs
    /// the same request: only the timestamp changes.
    void SignRequest(const S3Credentials& credentials, const ParsedOssUrl& location,
                     const std::string& method, const uint8_t* body, size_t body_len,
                     const std::string& canonical_query, http::Request* request) const;

    core::Expected<http::Response, std::string> Send(const ParsedOssUrl& location,
                                                        const std::string& method,
                                                        bool has_range, uint64_t range_begin,
                                                        uint64_t range_end,
                                                        const uint8_t* body, size_t body_len,
                                                        http::BodySink* sink);

    /// `Send` with a signed query string, for the multipart sub-resources
    /// (`?uploads`, `?partNumber&uploadId`, `?uploadId`).
    ///
    /// `canonical_query` must already be sorted and percent-encoded: SigV4
    /// hashes it verbatim, so building it here rather than in each caller is
    /// what keeps the sent URL and the signed string identical.
    /// The three multipart calls. Split out so `UploadPath` reads as the
    /// protocol sequence it is, and so each one's response validation lives
    /// next to the request that needs it.
    core::Expected<std::string, std::string> CreateMultipartUpload(
        const ParsedOssUrl& location);
    core::Expected<std::string, std::string> UploadPart(const ParsedOssUrl& location,
                                                           const std::string& upload_id,
                                                           uint32_t part_number,
                                                           const uint8_t* body,
                                                           size_t body_len);
    core::Expected<core::Unit, std::string> CompleteMultipartUpload(
        const ParsedOssUrl& location, const std::string& upload_id,
        const std::vector<std::string>& etags);
    /// Best-effort; failures are swallowed because the caller is already
    /// returning an error.
    void AbortMultipartUpload(const ParsedOssUrl& location, const std::string& upload_id);

    core::Expected<http::Response, std::string> SendWithQuery(
        const ParsedOssUrl& location, const std::string& method, bool has_range,
        uint64_t range_begin, uint64_t range_end, const uint8_t* body, size_t body_len,
        http::BodySink* sink, const std::string& canonical_query);

    OssConfig config_;
    S3Credentials credentials_;
    core::Optional<AddressingStyle> addressing_override_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_OSS_H_
