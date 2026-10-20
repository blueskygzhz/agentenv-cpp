// SPDX-License-Identifier: MIT
// HTTP transport for the remote storage backends — see http_transport.h.
#include "agentenv/storage/overlaybd/http_transport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef AGENTENV_WITH_CURL
#include <curl/curl.h>
#endif

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace http {

namespace {

char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsUnreserved(char c) {
    // RFC 3986 unreserved set. Everything else is escaped, which is the safe
    // direction: over-escaping a legal character still addresses the same
    // resource, under-escaping one does not.
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

std::string Encode(const std::string& value, bool keep_slash) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (IsUnreserved(c) || (keep_slash && c == '/')) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
            out.push_back(kHex[static_cast<unsigned char>(c) & 0xF]);
        }
    }
    return out;
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

#ifdef AGENTENV_WITH_CURL

/// Routes each chunk either to the caller's sink or to `Response.body`.
///
/// The decision is per chunk rather than made up front because the status is
/// not known until the headers are in: an error body has to land in
/// `Response.body` so the caller can read the message, even when a sink was
/// supplied for the payload.
struct BodyTarget {
    std::vector<uint8_t>* buffer;
    BodySink* sink;
    const long* status;
    /// Set when the sink refused a chunk, so `Perform` can tell an aborted
    /// transfer apart from a network failure.
    bool sink_refused;
};

size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    BodyTarget* target = static_cast<BodyTarget*>(userdata);
    const size_t total = size * nmemb;
    const uint8_t* bytes = reinterpret_cast<uint8_t*>(ptr);

    const bool to_sink = target->sink != 0 && *target->status >= 200 && *target->status < 300;
    if (to_sink) {
        if (!target->sink->Write(bytes, total)) {
            target->sink_refused = true;
            // Returning anything but `total` makes libcurl abort with
            // CURLE_WRITE_ERROR, which is the documented way to stop a
            // transfer early -- so an over-long response is cut off instead
            // of being downloaded in full and then rejected.
            return 0;
        }
        return total;
    }

    target->buffer->insert(target->buffer->end(), bytes, bytes + total);
    return total;
}

struct HeaderTarget {
    HeaderMap* headers;
    /// Updated as soon as the status line arrives, because the body callback
    /// has to know whether this is a 2xx before the first chunk -- and
    /// `CURLINFO_RESPONSE_CODE` is only readable after the transfer.
    long* status;
};

size_t WriteHeader(char* ptr, size_t size, size_t nmemb, void* userdata) {
    HeaderTarget* target = static_cast<HeaderTarget*>(userdata);
    const size_t total = size * nmemb;
    const std::string line(ptr, total);

    const std::string::size_type colon = line.find(':');
    if (colon == std::string::npos) {
        // No colon: either the status line or the blank terminator. A status
        // line starts with the version, and a redirect chain produces several
        // -- the last one wins, which is the status the body belongs to.
        if (line.compare(0, 5, "HTTP/") == 0) {
            const std::string::size_type sp = line.find(' ');
            if (sp != std::string::npos) {
                *target->status = ::strtol(line.c_str() + sp + 1, 0, 10);
                // A new response means the previous one's headers do not
                // apply; keeping them would let a redirect's Content-Length
                // describe the final body.
                target->headers->clear();
            }
        }
        return total;
    }

    const std::string name = Trim(line.substr(0, colon));
    const std::string value = Trim(line.substr(colon + 1));
    if (!name.empty()) (*target->headers)[name] = value;
    return total;
}

struct ReadState {
    const uint8_t* data;
    size_t len;
    size_t pos;
};

size_t ReadBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    ReadState* st = static_cast<ReadState*>(userdata);
    const size_t want = size * nmemb;
    const size_t remain = st->len - st->pos;
    const size_t n = remain < want ? remain : want;
    if (n > 0) std::memcpy(ptr, st->data + st->pos, n);
    st->pos += n;
    return n;
}

/// `curl_global_init` once per process. libcurl is not safe to initialise
/// concurrently, and leaving it to the first `curl_easy_init` is explicitly
/// documented as unreliable.
void EnsureGlobalInit() {
    static bool done = false;
    if (!done) {
        ::curl_global_init(CURL_GLOBAL_DEFAULT);
        done = true;
    }
}

#endif  // AGENTENV_WITH_CURL

}  // namespace

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const char ca = LowerAscii(a[i]);
        const char cb = LowerAscii(b[i]);
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

bool Response::ContentLength(uint64_t* out) const {
    const HeaderMap::const_iterator found = headers.find("Content-Length");
    if (found == headers.end()) return false;
    const std::string trimmed = Trim(found->second);
    if (trimmed.empty()) return false;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        if (trimmed[i] < '0' || trimmed[i] > '9') return false;
    }
    if (out) *out = ::strtoull(trimmed.c_str(), 0, 10);
    return true;
}

bool Response::ContentRangeTotal(uint64_t* out) const {
    const HeaderMap::const_iterator found = headers.find("Content-Range");
    if (found == headers.end()) return false;
    // `bytes <start>-<end>/<total>`; the total is what a ranged read needs,
    // since Content-Length only describes the slice returned.
    const std::string::size_type slash = found->second.rfind('/');
    if (slash == std::string::npos) return false;
    const std::string total = Trim(found->second.substr(slash + 1));
    // `*` means the server will not say, which is not a parse failure.
    if (total.empty() || total == "*") return false;
    for (size_t i = 0; i < total.size(); ++i) {
        if (total[i] < '0' || total[i] > '9') return false;
    }
    if (out) *out = ::strtoull(total.c_str(), 0, 10);
    return true;
}

bool BufferSink::Write(const uint8_t* data, size_t len) {
    // Refused, not truncated. More bytes than the window means the server
    // ignored the range, so the surplus is a different region of the object
    // rather than the tail of what was asked for -- keeping the first `cap`
    // bytes would hand back the wrong data.
    if (filled_ + len > cap_) {
        overflow_ = true;
        return false;
    }
    if (len > 0) std::memcpy(dst_ + filled_, data, len);
    filled_ += len;
    return true;
}

bool Available() {
#ifdef AGENTENV_WITH_CURL
    return true;
#else
    return false;
#endif
}

std::string EncodePath(const std::string& path) { return Encode(path, true); }

std::string EncodeQueryValue(const std::string& value) { return Encode(value, false); }

core::Expected<Response, std::string> Perform(const Request& request) {
#ifndef AGENTENV_WITH_CURL
    (void)request;
    return core::make_unexpected(
        std::string("HTTP support not compiled in (AGENTENV_WITH_CURL)"));
#else
    EnsureGlobalInit();

    CURL* handle = ::curl_easy_init();
    if (handle == 0) return core::make_unexpected(std::string("curl_easy_init failed"));

    Response response;
    ReadState read_state;
    read_state.data = request.body.empty() ? 0 : &request.body[0];
    read_state.len = request.body.size();
    read_state.pos = 0;

    char error_buffer[CURL_ERROR_SIZE];
    error_buffer[0] = '\0';

    ::curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    ::curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer);
    ::curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(handle, CURLOPT_TIMEOUT, request.timeout_secs);
    ::curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, request.connect_timeout_secs);
    ::curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION,
                       request.follow_redirects ? 1L : 0L);
    // Mutual TLS. Both or neither: libcurl would otherwise send a certificate
    // it cannot prove ownership of, which fails the handshake outright rather
    // than degrading to anonymous TLS.
    if (!request.client_cert_file.empty() && !request.client_key_file.empty()) {
        ::curl_easy_setopt(handle, CURLOPT_SSLCERT, request.client_cert_file.c_str());
        ::curl_easy_setopt(handle, CURLOPT_SSLKEY, request.client_key_file.c_str());
        // Rust builds the identity from a concatenated cert+key PEM, so both
        // files are PEM here too.
        ::curl_easy_setopt(handle, CURLOPT_SSLCERTTYPE, "PEM");
        ::curl_easy_setopt(handle, CURLOPT_SSLKEYTYPE, "PEM");
    }
    ::curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteBody);
    BodyTarget body_target;
    body_target.buffer = &response.body;
    body_target.sink = request.body_sink;
    body_target.status = &response.status;
    body_target.sink_refused = false;
    ::curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body_target);
    ::curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, WriteHeader);
    HeaderTarget header_target;
    header_target.headers = &response.headers;
    header_target.status = &response.status;
    ::curl_easy_setopt(handle, CURLOPT_HEADERDATA, &header_target);

    if (request.method == "HEAD") {
        ::curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    } else if (request.method == "PUT") {
        ::curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
        ::curl_easy_setopt(handle, CURLOPT_READFUNCTION, ReadBody);
        ::curl_easy_setopt(handle, CURLOPT_READDATA, &read_state);
        ::curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE,
                           static_cast<curl_off_t>(request.body.size()));
    } else if (request.method != "GET") {
        ::curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        // A body on a non-PUT method. Previously only PUT could carry one, so
        // a POST's body was silently dropped -- which SigV4 turns into a 403
        // rather than a visible truncation, because the signature covers a
        // payload hash the server then computes over an empty body.
        //
        // POSTFIELDS rather than the upload machinery: it keeps
        // `CUSTOMREQUEST` as the method (CURLOPT_POST would force "POST" and
        // override a DELETE), and it sets Content-Length from the size given.
        if (!request.body.empty()) {
            ::curl_easy_setopt(handle, CURLOPT_POSTFIELDS,
                               reinterpret_cast<const char*>(&request.body[0]));
            ::curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                               static_cast<curl_off_t>(request.body.size()));
        }
    }

    if (request.has_range) {
        std::ostringstream range;
        // Inclusive on both ends, which is what HTTP means by `a-b`.
        range << request.range_begin << "-" << request.range_end;
        ::curl_easy_setopt(handle, CURLOPT_RANGE, range.str().c_str());
    }

    struct curl_slist* headers = 0;
    for (HeaderMap::const_iterator it = request.headers.begin();
         it != request.headers.end(); ++it) {
        const std::string line = it->first + ": " + it->second;
        headers = ::curl_slist_append(headers, line.c_str());
    }
    // libcurl would otherwise add `Expect: 100-continue` to larger PUTs and
    // wait out a timeout against servers that never answer it.
    headers = ::curl_slist_append(headers, "Expect:");
    if (headers != 0) ::curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = ::curl_easy_perform(handle);
    if (rc == CURLE_OK) {
        // Authoritative once the transfer finished; the header callback's
        // value is only needed mid-flight.
        ::curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    }
    const bool sink_refused = body_target.sink_refused;

    if (headers != 0) ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(handle);

    // Reported as success with its status intact. The abort was this layer's
    // own decision, so turning it into a transport error would hide the fact
    // that the server answered -- and the caller already knows why it
    // refused, from its own sink.
    if (rc == CURLE_WRITE_ERROR && sink_refused) return response;

    if (rc != CURLE_OK) {
        // A transport failure is an error; an HTTP status is not. The callers
        // branch on 404 / 416 / 5xx to choose between falling back, retrying
        // and failing, so those must survive as statuses.
        std::ostringstream o;
        o << "http " << request.method << " " << request.url << " failed: "
          << (error_buffer[0] != '\0' ? error_buffer : ::curl_easy_strerror(rc));
        return core::make_unexpected(o.str());
    }

    return response;
#endif
}

}  // namespace http
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
