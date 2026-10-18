// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/oss.rs — reading objects from S3/OSS.
#ifndef AGENTENV_STORAGE_OVERLAYBD_OSS_H_
#define AGENTENV_STORAGE_OVERLAYBD_OSS_H_

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
    core::Expected<uint64_t, std::string> Size() override;

 private:
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
    core::Expected<http::Response, std::string> Send(const ParsedOssUrl& location,
                                                        const std::string& method,
                                                        bool has_range, uint64_t range_begin,
                                                        uint64_t range_end,
                                                        const uint8_t* body, size_t body_len);

    OssConfig config_;
    S3Credentials credentials_;
    core::Optional<AddressingStyle> addressing_override_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_OSS_H_
