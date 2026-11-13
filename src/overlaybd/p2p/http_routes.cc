// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/facade.rs — the router half.
#include "agentenv/overlaybd/p2p/http_routes.h"

#include <sstream>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

namespace {

using core::Optional;
using core::Unit;

/// 32 MiB, matching the chunk ceiling `HttpP2pTransport` fetches with. Keeping
/// the two equal means a range the facade accepts is one the transport can
/// serve in one request.
const std::size_t kMaxFacadeRangeBytesValue = 32u * 1024 * 1024;

std::string DecimalString(uint64_t value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

void Fail(FacadeServerResponse* response, int status, const std::string& message) {
    response->status = status;
    response->headers["Content-Type"] = "text/plain; charset=utf-8";
    response->body = message;
}

/// The path of a raw request target, with any query removed.
std::string PathOf(const std::string& raw_target) {
    const std::size_t question = raw_target.find('?');
    return question == std::string::npos ? raw_target : raw_target.substr(0, question);
}

/// Writes a successful ranged response.
///
/// The headers are computed *before* the body is drained, which is the order
/// that matters: `BuildRangeResponseHeaders` is what decides the advertised
/// `Content-Length`, and the drain below is then held to it. Draining first
/// and describing afterwards would let a misbehaving source dictate the
/// framing.
void WriteRangeResponse(const RequestRange& request_range, const OriginRangeResult& result,
                        FacadeServerResponse* response) {
    const core::Expected<RangeResponseHeaders, HttpFacadeError> headers =
        BuildRangeResponseHeaders(request_range, result.response_len, result.total_size);
    if (!headers.ok()) {
        Fail(response, headers.error().status, headers.error().message);
        return;
    }
    if (result.response_len > kMaxFacadeRangeBytesValue) {
        // Refused rather than truncated: see `kMaxFacadeRangeBytes`.
        Fail(response, kStatusRangeNotSatisfiable, "requested range is too large to buffer");
        return;
    }
    if (!result.body) {
        Fail(response, kStatusBadGateway, "range source produced no body");
        return;
    }

    std::string body;
    const core::Expected<Unit, std::string> drained =
        DrainByteStream(result.body, static_cast<std::size_t>(result.response_len), &body);
    if (!drained.ok()) {
        AGENTENV_WARN("facade range body failed mid-stream: " << drained.error());
        Fail(response, kStatusBadGateway, "failed to read range body");
        return;
    }
    // A stream that ended early is a truncated layer read. Answering it with
    // the promised `Content-Length` would make the client wait forever, and
    // answering it with the short length would hand overlaybd a hole it would
    // treat as data.
    if (body.size() != result.response_len) {
        AGENTENV_WARN("facade range body was short: got " << body.size() << " of "
                                                          << result.response_len);
        Fail(response, kStatusBadGateway, "range body was shorter than promised");
        return;
    }

    response->status = headers.value().status;
    response->headers["Content-Length"] = DecimalString(headers.value().content_length);
    response->headers["Content-Range"] = headers.value().content_range;
    response->headers["Accept-Ranges"] = headers.value().accept_ranges;
    response->body.swap(body);
}

}  // namespace

const std::size_t kMaxFacadeRangeBytes = kMaxFacadeRangeBytesValue;

core::Expected<Unit, std::string> DrainByteStream(
    const std::shared_ptr<agentenv::p2p::P2pByteStream>& stream, std::size_t limit,
    std::string* out) {
    if (!stream) return core::make_unexpected(std::string("no byte stream"));
    out->clear();
    out->reserve(limit);
    for (;;) {
        std::vector<uint8_t> chunk;
        const agentenv::p2p::P2pResult<bool> next = stream->Next(&chunk);
        if (!next.ok()) return core::make_unexpected(next.error().message);
        if (!next.value()) break;  // exhausted
        if (out->size() + chunk.size() > limit) {
            // Surplus is an error, not something to trim: more bytes than the
            // window means the source answered a different region, so the
            // prefix cannot be assumed to be the right one either.
            return core::make_unexpected(std::string("range source produced more than ") +
                                         DecimalString(limit) + " bytes");
        }
        out->append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
    }
    return Unit();
}

void RegisterOverlaybdP2pFacadeRoutes(FacadeServer* server, std::shared_ptr<FacadeCore> core) {
    if (server == NULL || !core) return;

    // ---- origin-addressed reads ---------------------------------------------
    //
    // The catch-all is what makes this expressible: the captured remainder is
    // an origin URL, complete with `://` and its own path separators.
    server->Route("GET", std::string(kAddressPrefix) + "/{*origin}",
                  [core](const FacadeServerRequest& request, FacadeServerResponse* response) {
        // Parsed here as well as inside the core: the core needs it to decide
        // what to fetch, and the response framing needs it to describe what
        // came back. Both derive it from the same headers, so they cannot
        // disagree.
        const core::Expected<RequestRange, HttpFacadeError> request_range =
            ParseSingleRange(request.headers);
        if (!request_range.ok()) {
            Fail(response, request_range.error().status, request_range.error().message);
            return;
        }
        const core::Expected<OriginRangeResult, HttpFacadeError> result =
            core->HandleOriginRequest(request.raw_target, request.headers);
        if (!result.ok()) {
            Fail(response, result.error().status, result.error().message);
            return;
        }
        WriteRangeResponse(request_range.value(), result.value(), response);
    });

    // ---- uuid-addressed reads -----------------------------------------------

    server->Route("GET", std::string(kUuidAddressPrefix) + "/{uuid}",
                  [core](const FacadeServerRequest& request, FacadeServerResponse* response) {
        const core::Expected<RequestRange, HttpFacadeError> request_range =
            ParseSingleRange(request.headers);
        if (!request_range.ok()) {
            Fail(response, request_range.error().status, request_range.error().message);
            return;
        }
        // Taken off the path rather than from a captured parameter, matching
        // what `p2p/http_routes.cc` does with the blob locator: the one check
        // that it is a plausible uuid then lives in `HandleUuidRequest` only.
        const std::string path = PathOf(request.raw_target);
        const std::string prefix = std::string(kUuidAddressPrefix) + "/";
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
            Fail(response, kStatusBadRequest, "invalid uuid");
            return;
        }
        const std::string uuid = path.substr(prefix.size());
        const core::Expected<OriginRangeResult, HttpFacadeError> result =
            core->HandleUuidRequest(uuid, request.headers);
        if (!result.ok()) {
            Fail(response, result.error().status, result.error().message);
            return;
        }
        WriteRangeResponse(request_range.value(), result.value(), response);
    });

    // ---- publish ------------------------------------------------------------

    server->Route("POST", kPublishLayerPath,
                  [core](const FacadeServerRequest& request, FacadeServerResponse* response) {
        // The loopback gate runs first, before the body is even parsed: a
        // remote caller must not be able to spend this node's CPU on a hash it
        // was never going to be allowed to request.
        if (!request.peer_is_loopback) {
            Fail(response, kStatusForbidden,
                 "publish endpoint only accepts localhost clients");
            return;
        }
        const core::Expected<PublishLayerRequest, std::string> parsed =
            PublishLayerRequest::ParseJson(request.body);
        if (!parsed.ok()) {
            Fail(response, kStatusBadRequest, parsed.error());
            return;
        }
        const core::Expected<Unit, HttpFacadeError> published =
            core->HandlePublishLayer(request.peer_is_loopback, parsed.value());
        if (!published.ok()) {
            Fail(response, published.error().status, published.error().message);
            return;
        }
        response->status = kStatusOk;
        response->headers["Content-Type"] = "application/json";
        response->body = "{}";
    });
}

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
