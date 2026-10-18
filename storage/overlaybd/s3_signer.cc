// SPDX-License-Identifier: MIT
// AWS Signature Version 4 — see s3_signer.h.
#include "agentenv/storage/overlaybd/s3_signer.h"

#include <ctime>
#include <sstream>
#include <vector>

#include "agentenv/core/digest.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

const char* const kS3EmptyPayloadSha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

namespace {

std::string ToHex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

std::string LowerAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

std::array<uint8_t, 32> Hmac(const std::vector<uint8_t>& key, const std::string& data) {
    return core::HmacSha256(key.empty() ? 0 : &key[0], key.size(), data.data(), data.size());
}

std::vector<uint8_t> HmacVec(const std::vector<uint8_t>& key, const std::string& data) {
    const std::array<uint8_t, 32> out = Hmac(key, data);
    return std::vector<uint8_t>(out.begin(), out.end());
}

}  // namespace

std::string S3AmzDateNow() {
    const std::time_t now = std::time(0);
    struct tm utc;
    // UTC, not local time: the scope string embeds the date, and a local
    // offset would put the request outside the server's accepted window near
    // midnight.
    ::gmtime_r(&now, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
    return std::string(buf);
}

std::string S3BuildAuthorization(const S3Credentials& credentials,
                                 const S3SigningContext& context,
                                 const http::HeaderMap& signed_headers) {
    // The scope date is the first 8 characters of the timestamp; the two must
    // agree or the server derives a different signing key.
    const std::string date_stamp = context.amz_date.substr(0, 8);

    // Canonical headers: lowercase name, trimmed value, sorted by name, each
    // line newline-terminated. The map is already case-insensitively ordered,
    // which is the order SigV4 requires.
    std::ostringstream canonical_headers;
    std::ostringstream signed_header_names;
    bool first = true;
    for (http::HeaderMap::const_iterator it = signed_headers.begin();
         it != signed_headers.end(); ++it) {
        const std::string name = LowerAscii(it->first);
        canonical_headers << name << ":" << Trim(it->second) << "\n";
        if (!first) signed_header_names << ";";
        signed_header_names << name;
        first = false;
    }

    std::ostringstream canonical_request;
    canonical_request << context.method << "\n"
                      << context.canonical_uri << "\n"
                      << context.canonical_query << "\n"
                      << canonical_headers.str() << "\n"
                      << signed_header_names.str() << "\n"
                      << context.payload_sha256_hex;

    const std::string canonical_hash = [&]() {
        const std::string body = canonical_request.str();
        uint8_t digest[32];
        core::Sha256Stream h;
        h.Update(body.data(), body.size());
        h.PeekFinal(digest);
        return ToHex(digest, 32);
    }();

    std::ostringstream scope;
    scope << date_stamp << "/" << context.region << "/" << context.service
          << "/aws4_request";

    std::ostringstream string_to_sign;
    string_to_sign << "AWS4-HMAC-SHA256\n"
                   << context.amz_date << "\n"
                   << scope.str() << "\n"
                   << canonical_hash;

    // The signing key is derived by chaining HMACs over date, region, service
    // and the terminator, so a key is only ever valid for one day, one region
    // and one service.
    const std::string initial = "AWS4" + credentials.secret_access_key;
    std::vector<uint8_t> key(initial.begin(), initial.end());
    key = HmacVec(key, date_stamp);
    key = HmacVec(key, context.region);
    key = HmacVec(key, context.service);
    key = HmacVec(key, "aws4_request");

    const std::array<uint8_t, 32> signature = Hmac(key, string_to_sign.str());

    std::ostringstream authorization;
    authorization << "AWS4-HMAC-SHA256 Credential=" << credentials.access_key_id << "/"
                  << scope.str() << ", SignedHeaders=" << signed_header_names.str()
                  << ", Signature=" << ToHex(&signature[0], 32);
    return authorization.str();
}

void S3SignRequest(const S3Credentials& credentials, const S3SigningContext& context,
                   http::Request* request) {
    if (request == 0) return;
    // A public bucket is read unsigned, so anonymous credentials are a no-op
    // rather than a failure.
    if (credentials.IsAnonymous()) return;

    // Exactly the headers this code controls. Signing something libcurl adds
    // later (`Accept`, `User-Agent`) would invalidate the signature whenever
    // libcurl changed it.
    http::HeaderMap signed_headers;
    signed_headers["host"] = context.host;
    signed_headers["x-amz-content-sha256"] = context.payload_sha256_hex;
    signed_headers["x-amz-date"] = context.amz_date;
    if (!credentials.security_token.empty()) {
        // Covered by the signature, not just sent: a token outside it could
        // be substituted by anyone on the path.
        signed_headers["x-amz-security-token"] = credentials.security_token;
    }

    request->headers["x-amz-content-sha256"] = context.payload_sha256_hex;
    request->headers["x-amz-date"] = context.amz_date;
    if (!credentials.security_token.empty()) {
        request->headers["x-amz-security-token"] = credentials.security_token;
    }
    request->headers["Authorization"] =
        S3BuildAuthorization(credentials, context, signed_headers);

    // `Host` is not set on the request: libcurl derives it from the URL, and
    // setting it twice would produce two headers and break the signature.
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
