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

size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::vector<uint8_t>* out = static_cast<std::vector<uint8_t>*>(userdata);
    const size_t total = size * nmemb;
    out->insert(out->end(), reinterpret_cast<uint8_t*>(ptr),
                reinterpret_cast<uint8_t*>(ptr) + total);
    return total;
}

size_t WriteHeader(char* ptr, size_t size, size_t nmemb, void* userdata) {
    HeaderMap* out = static_cast<HeaderMap*>(userdata);
    const size_t total = size * nmemb;
    const std::string line(ptr, total);
    const std::string::size_type colon = line.find(':');
    // The status line and the blank terminator have no colon; skipping them
    // is why this is not an error.
    if (colon != std::string::npos) {
        const std::string name = Trim(line.substr(0, colon));
        const std::string value = Trim(line.substr(colon + 1));
        if (!name.empty()) (*out)[name] = value;
    }
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
    ::curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteBody);
    ::curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    ::curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, WriteHeader);
    ::curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response.headers);

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
        ::curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    }

    if (headers != 0) ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(handle);

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
