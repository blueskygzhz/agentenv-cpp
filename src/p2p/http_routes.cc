// SPDX-License-Identifier: MIT
// Rust: `BlobsProtocol` + `CatalogProtocol` as registered on the iroh
// `Router` (src/p2p/iroh/transport.rs:142-148) — the serving half, over HTTP.
//
// Rust accepts two ALPNs on one QUIC endpoint: `iroh_blobs::ALPN` for data and
// `CATALOG_ALPN` for metadata. The split survives here as two paths on one
// listener, for the reason Rust states at that call site: "Serve both data and
// metadata from the same endpoint. The scheduler only passes endpoint
// addresses around; it does not proxy catalog requests or artifact bytes."
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/p2p/http_transport.h"

namespace agentenv {
namespace p2p {

namespace {

/// Constant-time token comparison. A check that short-circuits on the first
/// differing byte leaks the token's prefix to anyone who can time it.
bool TokenAccepted(const P2pServerRequest& request, const std::string& expected) {
    if (expected.empty()) return true;  // check disabled
    const core::Optional<std::string> presented =
        FindP2pHeader(request.headers, kTokenHeader);
    if (!presented.has_value()) return false;
    if (presented->size() != expected.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(expected[i]) ^
                    static_cast<unsigned char>((*presented)[i])));
    }
    return diff == 0;
}

/// The locator from `/p2p/v1/blobs/{locator}`.
///
/// Only the digest alphabet is accepted. The locator reaches the store, which
/// maps it onto a file name, so anything outside `[A-Za-z0-9:.-]` is rejected
/// here rather than relied upon to be harmless further in.
bool LocatorFromPath(const std::string& path, std::string* out) {
    const std::string prefix(kBlobPathPrefix);
    if (path.size() <= prefix.size()) return false;
    if (path.compare(0, prefix.size(), prefix) != 0) return false;

    const std::string locator = path.substr(prefix.size());
    if (locator.empty() || locator.size() > 160) return false;
    for (std::size_t i = 0; i < locator.size(); ++i) {
        const char c = locator[i];
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == ':' ||
                        c == '.' || c == '-';
        if (!ok) return false;
    }
    if (out) *out = locator;
    return true;
}

void Fail(P2pServerResponse* response, int status, const std::string& message) {
    response->status = status;
    response->headers["Content-Type"] = "text/plain";
    response->body = message;
}

std::string DecimalString(uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    return std::string(buf);
}

}  // namespace

void RegisterHttpP2pRoutes(P2pBlobServer* server,
                           std::shared_ptr<PublishedArtifactCatalog> catalog,
                           std::shared_ptr<HttpBlobStore> store,
                           const std::string& shared_token) {
    // ---- metadata -----------------------------------------------------------

    server->Route("POST", kCatalogLookupPath,
                  [catalog, shared_token](const P2pServerRequest& request,
                                          P2pServerResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "p2p token rejected");
            return;
        }
        // Checked before parsing, so a peer cannot force an unbounded
        // allocation by sending a large body — the same ordering
        // `CatalogRequest::Decode` already enforces internally.
        if (request.body.size() > kMaxCatalogRequestBytes) {
            Fail(response, 400, "catalog request too large");
            return;
        }
        const core::Expected<CatalogRequest, std::string> decoded =
            CatalogRequest::Decode(request.body);
        if (!decoded.ok()) {
            Fail(response, 400, "malformed catalog request: " + decoded.error());
            return;
        }

        // `DescriptorForResponse`, not `DescriptorFor`: the served copy
        // advertises this node as the sole provider, because a requesting peer
        // is asking what we can serve, not what we have heard about.
        CatalogResponse answer;
        answer.descriptor = catalog->DescriptorForResponse(decoded.value().key);

        const std::string encoded = answer.Encode();
        if (encoded.size() > kMaxCatalogResponseBytes) {
            // Refused rather than truncated: a clipped descriptor would still
            // decode into something the peer would act on.
            Fail(response, 500, "catalog response exceeds the protocol limit");
            return;
        }

        // A miss is a 200 with an absent descriptor, never a 404. The wire
        // type is explicit that "an absent descriptor is a miss, not an
        // error", and a 404 would be indistinguishable from a node that does
        // not speak this protocol at all.
        response->status = 200;
        response->headers["Content-Type"] = "application/json";
        response->body = encoded;
    });

    // ---- size ---------------------------------------------------------------
    //
    // Served but not used by this tree's own client, which is a deliberate
    // asymmetry rather than an oversight.
    //
    // `core::http` cannot consume a HEAD reply at all: it frames a response
    // body by `Content-Length`, so a bodiless answer carrying a non-zero one
    // reads to it as truncated. The fetch path therefore takes the total from
    // the first range response's `Content-Range` instead, which also saves a
    // connection -- `HttpClient` opens a fresh one per request.
    //
    // Kept anyway because the route is part of the protocol, not of this
    // client: `curl -I` is how an operator checks whether a node holds a
    // layer, and a peer built on a streaming HTTP client has no reason to be
    // denied the cheaper size probe. Removing it would narrow the protocol to
    // this implementation's current limitation.
    server->Route("HEAD", std::string(kBlobPathPrefix) + "{locator}",
                  [store, shared_token](const P2pServerRequest& request,
                                        P2pServerResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "p2p token rejected");
            return;
        }
        std::string locator;
        if (!LocatorFromPath(request.path, &locator)) {
            Fail(response, 400, "malformed blob locator");
            return;
        }
        std::shared_ptr<BlobLease> lease = store->Acquire(locator);
        if (!lease) {
            Fail(response, 404, "no such artifact");
            return;
        }

        response->status = 200;
        response->headers["Content-Length"] = DecimalString(lease->entry().size);
        response->headers["Accept-Ranges"] = "bytes";
        response->headers[kDigestHeader] = lease->entry().digest;
    });

    // ---- bytes --------------------------------------------------------------

    server->Route("GET", std::string(kBlobPathPrefix) + "{locator}",
                  [store, shared_token](const P2pServerRequest& request,
                                        P2pServerResponse* response) {
        if (!TokenAccepted(request, shared_token)) {
            Fail(response, 401, "p2p token rejected");
            return;
        }
        std::string locator;
        if (!LocatorFromPath(request.path, &locator)) {
            Fail(response, 400, "malformed blob locator");
            return;
        }

        const core::Optional<std::string> range = FindP2pHeader(request.headers, "Range");
        // A whole-artifact GET is refused, not served. The response body is
        // assembled in memory before it is written, so answering one would
        // charge this node an artifact-sized allocation chosen by the peer.
        // Requiring the header is what makes the memory ceiling a property of
        // the protocol rather than a hope about callers.
        if (!range.has_value()) {
            Fail(response, 400, "the p2p blob endpoint requires a Range header");
            return;
        }
        uint64_t start = 0;
        uint64_t end = 0;
        if (!ParseByteRangeHeader(*range, &start, &end)) {
            Fail(response, 400, "unsupported Range header");
            return;
        }

        // Taken before the size checks and held across the read, so an
        // unpublish arriving now cannot delete the file underneath it.
        std::shared_ptr<BlobLease> lease = store->Acquire(locator);
        if (!lease) {
            Fail(response, 404, "no such artifact");
            return;
        }
        const BlobEntry& entry = lease->entry();

        // Unsatisfiable only when the range starts past the end. A range whose
        // *end* runs past it is clamped, per RFC 7233, and that is load-bearing
        // rather than pedantry: it is what lets a client ask for the first
        // chunk of an artifact whose size it does not know yet and learn the
        // total from `Content-Range`. Rejecting those outright would force a
        // size probe first, and `core::http` cannot issue one -- it frames a
        // response by `Content-Length`, so a bodiless HEAD reply reads as a
        // truncated response.
        //
        // The empty-artifact case falls out here as a 416 carrying
        // `bytes */0`, which a client can read as "nothing to fetch" without
        // a separate code path.
        if (entry.size == 0 || start >= entry.size) {
            response->status = 416;
            response->headers["Content-Range"] = "bytes */" + DecimalString(entry.size);
            response->body = "range starts past the end of the artifact";
            return;
        }
        if (end > entry.size - 1) end = entry.size - 1;
        if (end - start + 1 > kMaxRangeBytes) {
            // Clamped to the ceiling rather than refused: the client learns
            // what it was granted from `Content-Range` and asks again from
            // there, so a conservative server never deadlocks a transfer.
            end = start + kMaxRangeBytes - 1;
        }
        const uint64_t length = end - start + 1;

        // A `Reference` publication names a file this store does not own, so
        // its size is re-checked on every serve. If it has changed, the bytes
        // no longer match the digest peers hold, and serving them would be
        // worse than refusing: the reader's own digest check would fail much
        // later with nothing pointing back here.
        if (entry.referenced) {
            const core::Expected<uint64_t, std::string> actual =
                core::fs::FileSize(entry.path);
            if (!actual.ok() || actual.value() != entry.size) {
                Fail(response, 409, "the referenced artifact changed on disk");
                return;
            }
        }

        std::string bytes;
        {
            std::ifstream file(entry.path.c_str(), std::ios::binary);
            if (!file) {
                Fail(response, 500, "cannot open the artifact");
                return;
            }
            file.seekg(static_cast<std::streamoff>(start), std::ios::beg);
            bytes.resize(static_cast<std::size_t>(length));
            file.read(&bytes[0], static_cast<std::streamsize>(length));
            if (static_cast<uint64_t>(file.gcount()) != length) {
                // The length was already validated against the recorded size,
                // so a short read means the file shrank. Reporting it as an
                // error preserves the invariant the client relies on: a 2xx
                // body is exactly the requested length.
                Fail(response, 500, "the artifact is shorter than its record");
                return;
            }
        }

        response->status = 206;
        response->headers["Content-Type"] = "application/octet-stream";
        response->headers["Content-Length"] = DecimalString(length);
        response->headers["Content-Range"] = FormatContentRange(start, end, entry.size);
        response->headers[kDigestHeader] = entry.digest;
        response->body = bytes;
    });
}

}  // namespace p2p
}  // namespace agentenv
