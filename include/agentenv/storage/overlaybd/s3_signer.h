// SPDX-License-Identifier: MIT
// AWS Signature Version 4 for the OSS/S3 backend.
//
// Rust gets this from `opendal`, which gets it from the AWS SDK's signer. No
// equivalent library is available here, so this implements the published
// SigV4 specification directly. It is self-contained on purpose: the only
// primitives needed are SHA-256 and HMAC-SHA256, both already in
// `core/digest.h`, so no new dependency is required.
#ifndef AGENTENV_STORAGE_OVERLAYBD_S3_SIGNER_H_
#define AGENTENV_STORAGE_OVERLAYBD_S3_SIGNER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/http_transport.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Credentials for one signed request.
struct S3Credentials {
    std::string access_key_id;
    std::string secret_access_key;
    /// Optional STS session token. When present it is both sent as
    /// `x-amz-security-token` and covered by the signature -- a token outside
    /// the signature could be swapped by anyone on the path.
    std::string security_token;

    bool IsAnonymous() const { return access_key_id.empty() || secret_access_key.empty(); }
};

/// What SigV4 needs to know about the request being signed.
struct S3SigningContext {
    std::string method;
    /// Already percent-encoded, starting with `/`.
    std::string canonical_uri;
    /// Already encoded and sorted by key; empty when there is none.
    std::string canonical_query;
    std::string host;
    std::string region;
    /// `"s3"` for both AWS S3 and Aliyun OSS in S3-compatible mode.
    std::string service;
    /// Lowercase hex SHA-256 of the body. For a GET or HEAD this is the hash
    /// of the empty string, which is why it is supplied rather than derived:
    /// a streaming upload knows its digest without holding the body.
    std::string payload_sha256_hex;
    /// `YYYYMMDDTHHMMSSZ`. Supplied so a caller can sign deterministically
    /// in a test rather than reading the clock.
    std::string amz_date;

    S3SigningContext() : method("GET"), canonical_uri("/"), service("s3") {}
};

/// Lowercase hex SHA-256 of the empty string — the payload hash of every
/// bodyless request.
extern const char* const kS3EmptyPayloadSha256;

/// `YYYYMMDDTHHMMSSZ` for now, in UTC.
std::string S3AmzDateNow();

/// Sign `request` in place, adding `Authorization`, `x-amz-date`,
/// `x-amz-content-sha256` and, when present, `x-amz-security-token`.
///
/// Anonymous credentials are a no-op rather than an error: a public bucket is
/// read without signing, and refusing would make that case impossible.
///
/// The signed header set is exactly `host`, `x-amz-content-sha256`,
/// `x-amz-date` and the token when present. Headers libcurl adds later (such
/// as `Accept`) are deliberately *not* signed -- including a header this code
/// does not control would break the signature whenever libcurl changed it.
void S3SignRequest(const S3Credentials& credentials, const S3SigningContext& context,
                   http::Request* request);

/// The `Authorization` value alone, exposed for testing against the published
/// SigV4 test vectors.
std::string S3BuildAuthorization(const S3Credentials& credentials,
                                 const S3SigningContext& context,
                                 const http::HeaderMap& signed_headers);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_S3_SIGNER_H_
