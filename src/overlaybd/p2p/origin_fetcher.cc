// SPDX-License-Identifier: MIT
// Rust: the `reqwest::Client` half of `src/overlaybd/p2p/facade.rs` —
// `fetch_origin_range_stream`'s actual HTTP call.
//
// `facade.h` left this behind an injectable `OriginRangeFetcher` because the
// protocol decisions are what needed testing. The concrete implementation was
// then never written, so `FacadeCore::HandleOriginRequest` had no origin to
// fall back to: with a null fetcher it answers 502 for every P2P miss, which
// is every read until some peer happens to hold the layer.
//
// Built on `storage::overlaybd::http` rather than `core::http`, and that is
// not a preference: registry and object-store URLs are HTTPS, `core::http`
// has no TLS, and this one is the libcurl-backed client the registry reader
// already uses for exactly these ranged GETs.
#include "agentenv/overlaybd/p2p/origin_fetcher.h"

#include <sstream>

#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/http_transport.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

namespace {

namespace shttp = storage::overlaybd::http;

/// A `P2pByteStream` over bytes already in memory.
///
/// **Deviation from upstream, forced by the server interfaces.** Rust hands
/// axum a streaming body; both server abstractions in this tree carry the body
/// as a `std::string`, so the facade buffers one bounded range either way (see
/// `kMaxFacadeRangeBytes`). Given that, buffering here and yielding once is
/// honest: a chunked stream over an already-complete buffer would only
/// simulate streaming the caller does not get.
class BufferedByteStream : public agentenv::p2p::P2pByteStream {
 public:
    explicit BufferedByteStream(std::vector<uint8_t> bytes)
        : bytes_(std::move(bytes)), yielded_(false) {}

    agentenv::p2p::P2pResult<bool> Next(std::vector<uint8_t>* chunk_out) override {
        if (yielded_) return false;
        yielded_ = true;
        chunk_out->swap(bytes_);
        return true;
    }

 private:
    std::vector<uint8_t> bytes_;
    bool yielded_;
};

/// Collects a response body, refusing more than the window asked for.
///
/// `shttp::BufferSink` needs a fixed caller-owned buffer and the response
/// length is not known until the headers arrive, so this grows instead --
/// bounded, because a server that ignores `Range` would otherwise stream the
/// whole object into memory.
class GrowingSink : public shttp::BodySink {
 public:
    explicit GrowingSink(std::size_t limit) : limit_(limit), overflow_(false) {}

    bool Write(const uint8_t* data, size_t len) override {
        if (bytes_.size() + len > limit_) {
            // Returning false aborts the transfer, which is the point: this is
            // discovered while the surplus is arriving rather than after.
            overflow_ = true;
            return false;
        }
        bytes_.insert(bytes_.end(), data, data + len);
        return true;
    }

    std::vector<uint8_t>& bytes() { return bytes_; }
    bool overflow() const { return overflow_; }

 private:
    std::vector<uint8_t> bytes_;
    std::size_t limit_;
    bool overflow_;
};

std::string DecimalString(uint64_t value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

}  // namespace

core::Expected<OriginRangeResult, std::string> CurlOriginRangeFetcher::FetchRange(
    const std::string& origin_url, const RequestRange& range,
    const std::vector<std::pair<std::string, std::string> >& forwarded) {
    if (!shttp::Available()) {
        return core::make_unexpected(
            std::string("this build has no HTTP client; rebuild with libcurl"));
    }

    const core::Expected<std::size_t, HttpFacadeError> len = range.Len();
    if (!len.ok()) return core::make_unexpected(len.error().message);
    if (len.value() > max_range_bytes_) {
        return core::make_unexpected(std::string("origin range of ") +
                                     DecimalString(len.value()) + " bytes exceeds the limit");
    }

    shttp::Request request;
    request.method = "GET";
    request.url = origin_url;
    request.has_range = true;
    request.range_begin = range.start;
    request.range_end = range.end;
    request.timeout_secs = timeout_secs_;
    // Registries redirect blob reads to a CDN, so this has to be on; the
    // registry reader sets it for the same reason.
    request.follow_redirects = true;
    // `ForwardedHeaders` has already reduced these to the `Authorization` /
    // `User-Agent` allowlist, so they are copied as given rather than filtered
    // again here -- one allowlist, in one place.
    for (std::size_t i = 0; i < forwarded.size(); ++i) {
        request.headers[forwarded[i].first] = forwarded[i].second;
    }

    GrowingSink sink(len.value());
    request.body_sink = &sink;

    const core::Expected<shttp::Response, std::string> response = shttp::Perform(request);
    if (!response.ok()) return core::make_unexpected(response.error());
    if (sink.overflow()) {
        return core::make_unexpected(
            std::string("origin returned more bytes than the requested range"));
    }

    // The status and header checks live in `ValidateOriginRangeResponse`,
    // which is shared with the tests and is where the "origin ignored Range"
    // case is caught. Calling it rather than re-deriving the rules here is
    // what keeps a 200-instead-of-206 from being framed as if it had complied.
    HeaderMap response_headers;
    for (shttp::HeaderMap::const_iterator it = response.value().headers.begin();
         it != response.value().headers.end(); ++it) {
        response_headers.Set(it->first, it->second);
    }
    ContentRange parsed;
    const core::Expected<uint64_t, std::string> validated = ValidateOriginRangeResponse(
        static_cast<int>(response.value().status), response_headers, range, &parsed);
    if (!validated.ok()) return core::make_unexpected(validated.error());

    if (sink.bytes().size() != validated.value()) {
        std::ostringstream oss;
        oss << "origin range body was " << sink.bytes().size() << " bytes, expected "
            << validated.value();
        return core::make_unexpected(oss.str());
    }

    OriginRangeResult result;
    result.response_len = validated.value();
    result.total_size = parsed.total;
    result.body.reset(new BufferedByteStream(std::move(sink.bytes())));
    return result;
}

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
