// SPDX-License-Identifier: MIT
// Rust: src/p2p/iroh/transport.rs, over HTTP instead of iroh.
#include "agentenv/p2p/http_transport.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/http.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace p2p {

const char* const kCatalogLookupPath = "/p2p/v1/catalog/lookup";
const char* const kBlobPathPrefix = "/p2p/v1/blobs/";
const char* const kTokenHeader = "X-Agentenv-P2p-Token";
const char* const kDigestHeader = "X-Agentenv-P2p-Digest";

const uint64_t kFetchChunkBytes = 32ull * 1024 * 1024;
const uint64_t kMaxRangeBytes = 64ull * 1024 * 1024;
const std::size_t kMaxConcurrentCatalogLookups = 4;

core::Optional<std::string> FindP2pHeader(const std::map<std::string, std::string>& headers,
                                          const std::string& name) {
    for (std::map<std::string, std::string>::const_iterator it = headers.begin();
         it != headers.end(); ++it) {
        if (it->first.size() != name.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < name.size() && same; ++i) {
            same = std::tolower(static_cast<unsigned char>(it->first[i])) ==
                   std::tolower(static_cast<unsigned char>(name[i]));
        }
        if (same) return it->second;
    }
    return core::Optional<std::string>();
}

// ---- small helpers ----------------------------------------------------------

bool ParseByteRangeHeader(const std::string& value, uint64_t* start, uint64_t* end) {
    const std::string kPrefix = "bytes=";
    if (value.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    const std::string spec = value.substr(kPrefix.size());

    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= spec.size()) return false;
    // A multi-range request would need a multipart body, which neither the
    // server nor the only client can frame; rejecting it is honest.
    if (spec.find(',') != std::string::npos) return false;

    const std::string lo = spec.substr(0, dash);
    const std::string hi = spec.substr(dash + 1);
    for (std::size_t i = 0; i < lo.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(lo[i]))) return false;
    for (std::size_t i = 0; i < hi.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(hi[i]))) return false;

    const uint64_t s = std::strtoull(lo.c_str(), NULL, 10);
    const uint64_t e = std::strtoull(hi.c_str(), NULL, 10);
    if (e < s) return false;
    if (start) *start = s;
    if (end) *end = e;
    return true;
}

std::string FormatContentRange(uint64_t start, uint64_t end, uint64_t total) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "bytes %llu-%llu/%llu",
                  static_cast<unsigned long long>(start),
                  static_cast<unsigned long long>(end),
                  static_cast<unsigned long long>(total));
    return std::string(buf);
}

bool ParseContentRangeHeader(const std::string& value, bool* out_satisfied,
                             uint64_t* start, uint64_t* end, uint64_t* total) {
    const std::string kPrefix = "bytes ";
    if (value.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    const std::string spec = value.substr(kPrefix.size());

    const std::size_t slash = spec.find('/');
    if (slash == std::string::npos || slash + 1 >= spec.size()) return false;
    const std::string range = spec.substr(0, slash);
    const std::string size = spec.substr(slash + 1);

    // An unknown total (`*`) is refused rather than tolerated: every use here
    // needs the size, and accepting the header without one would mean
    // discovering that a field later, somewhere with less context.
    for (std::size_t i = 0; i < size.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(size[i]))) return false;
    }
    if (total) *total = std::strtoull(size.c_str(), NULL, 10);

    if (range == "*") {
        if (out_satisfied) *out_satisfied = false;
        if (start) *start = 0;
        if (end) *end = 0;
        return true;
    }

    const std::size_t dash = range.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= range.size()) return false;
    const std::string lo = range.substr(0, dash);
    const std::string hi = range.substr(dash + 1);
    for (std::size_t i = 0; i < lo.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(lo[i]))) return false;
    }
    for (std::size_t i = 0; i < hi.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(hi[i]))) return false;
    }
    const uint64_t s = std::strtoull(lo.c_str(), NULL, 10);
    const uint64_t e = std::strtoull(hi.c_str(), NULL, 10);
    if (e < s) return false;

    if (out_satisfied) *out_satisfied = true;
    if (start) *start = s;
    if (end) *end = e;
    return true;
}

bool ParseHostPort(const std::string& address, std::string* host, uint16_t* port) {
    const std::size_t colon = address.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= address.size()) return false;
    const std::string h = address.substr(0, colon);
    const std::string p = address.substr(colon + 1);
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(p[i]))) return false;
    }
    const unsigned long value = std::strtoul(p.c_str(), NULL, 10);
    if (value == 0 || value > 65535) return false;
    if (host) *host = h;
    if (port) *port = static_cast<uint16_t>(value);
    return true;
}

P2pResult<std::string> LocatorFromDescriptor(const P2pArtifactDescriptor& descriptor) {
    // Rust `blob_hash_from_descriptor` errors on a missing locator rather than
    // falling back to the key. The key names *what* the artifact is; the
    // locator names *which bytes*, and guessing one from the other would dial
    // a peer for content it never said it had.
    if (!descriptor.has_backend_locator || descriptor.backend_locator.empty()) {
        return core::make_unexpected(
            P2pError::Invalid("descriptor has no backend locator"));
    }
    return descriptor.backend_locator;
}

namespace {

/// Constant-time comparison. A token check that short-circuits on the first
/// differing byte leaks the token's prefix to anyone who can time it.
bool TokensMatch(const std::string& expected, const std::string& actual) {
    if (expected.empty()) return true;  // check disabled
    if (expected.size() != actual.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(expected[i]) ^
                    static_cast<unsigned char>(actual[i])));
    }
    return diff == 0;
}

core::http::Headers AuthHeaders(const std::string& token) {
    core::http::Headers h;
    if (!token.empty()) h[kTokenHeader] = token;
    return h;
}

/// Reads `[offset, offset+len)` of a local file into `out`.
core::Expected<core::Unit, std::string>
ReadFileRange(const std::string& path, uint64_t offset, uint64_t len, std::string* out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return core::make_unexpected(std::string("open failed"));
    f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!f) return core::make_unexpected(std::string("seek failed"));
    out->resize(static_cast<std::size_t>(len));
    if (len > 0) {
        f.read(&(*out)[0], static_cast<std::streamsize>(len));
        if (static_cast<uint64_t>(f.gcount()) != len) {
            return core::make_unexpected(std::string("short read"));
        }
    }
    return core::Unit{};
}

}  // namespace

// ---- Impl -------------------------------------------------------------------

class HttpP2pTransport::Impl {
 public:
    Options                                   options;
    std::shared_ptr<P2pPeerDiscovery>         discovery;
    std::shared_ptr<HttpBlobStore>            store;
    std::shared_ptr<PublishedArtifactCatalog> catalog;
    std::shared_ptr<P2pBlobServer>            server;
    std::thread                               serve_thread;
    bool                                      stopped = false;

    core::http::Timeouts LookupTimeouts() const {
        core::http::Timeouts t;
        t.read_ms = static_cast<int64_t>(options.config.lookup_timeout_ms);
        return t;
    }
    core::http::Timeouts FetchTimeouts() const {
        core::http::Timeouts t;
        t.read_ms = static_cast<int64_t>(options.config.fetch_timeout_ms);
        return t;
    }

    /// One `POST /p2p/v1/catalog/lookup` against a peer.
    core::Optional<P2pArtifactDescriptor> LookupPeer(const P2pPeer& peer,
                                                     const P2pArtifactKey& key) {
        const core::Expected<std::string, P2pError> address =
            EndpointAddressForBackend(peer.endpoint, kHttpBackendId);
        if (!address.ok()) return core::Optional<P2pArtifactDescriptor>();

        std::string host;
        uint16_t port = 0;
        if (!ParseHostPort(address.value(), &host, &port)) {
            AGENTENV_WARN("p2p: peer advertises an unparseable address peer=" <<
                          peer.node_id << " address=" << address.value());
            return core::Optional<P2pArtifactDescriptor>();
        }

        CatalogRequest request;
        request.key = key;

        core::http::Request req;
        req.method = "POST";
        req.path = kCatalogLookupPath;
        req.headers = AuthHeaders(options.shared_token);
        req.headers["Content-Type"] = "application/json";
        req.body = request.Encode();

        const core::http::HttpClient client(core::http::Target::Tcp(host, port),
                                            LookupTimeouts());
        const core::Expected<core::http::Response, std::string> response =
            client.Request(req);
        // A peer that is down is a miss, not a failure: the caller's job is to
        // find *a* provider, and one unreachable node does not make the lookup
        // unanswerable. Rust reaches the same conclusion by letting each
        // per-peer future resolve to None.
        if (!response.ok()) {
            AGENTENV_DEBUG("p2p: catalog lookup failed peer=" << peer.node_id <<
                           " error=" << response.error());
            return core::Optional<P2pArtifactDescriptor>();
        }
        if (!response.value().IsSuccess()) {
            AGENTENV_DEBUG("p2p: catalog lookup rejected peer=" << peer.node_id <<
                           " status=" << response.value().status);
            return core::Optional<P2pArtifactDescriptor>();
        }
        if (response.value().body.size() > kMaxCatalogResponseBytes) {
            AGENTENV_WARN("p2p: catalog response exceeds the protocol limit peer=" <<
                          peer.node_id);
            return core::Optional<P2pArtifactDescriptor>();
        }

        const core::Expected<CatalogResponse, std::string> decoded =
            CatalogResponse::Decode(response.value().body);
        if (!decoded.ok()) {
            AGENTENV_WARN("p2p: undecodable catalog response peer=" << peer.node_id <<
                          " error=" << decoded.error());
            return core::Optional<P2pArtifactDescriptor>();
        }
        return decoded.value().descriptor;
    }

    /// What one range request came back with.
    struct RangeReply {
        std::string bytes;
        /// Granted range, inclusive. May be narrower than asked for: the
        /// server clamps to the artifact's end and to its own per-request
        /// ceiling.
        uint64_t    start = 0;
        uint64_t    end = 0;
        /// The artifact's full size, from `Content-Range`. This is why the
        /// fetch loop needs no size probe.
        uint64_t    total = 0;
        /// False when the server answered 416, which it also uses to report
        /// the size of an artifact no range over it can satisfy -- an empty
        /// one. `bytes` is then empty and `total` is still meaningful.
        bool        satisfied = false;
    };

    /// `GET /p2p/v1/blobs/{locator}` for one range.
    ///
    /// Deliberately the only request the fetch path makes. There is a HEAD
    /// route in the protocol, but `core::http` cannot consume its reply: that
    /// client frames a response body by `Content-Length`, and a bodiless HEAD
    /// response carrying a non-zero one reads to it as truncated. Taking the
    /// total from `Content-Range` instead removes both the hazard and a round
    /// trip -- which matters here, because `HttpClient` opens a fresh
    /// connection per request.
    ///
    /// The returned body is verified to be exactly as long as the range the
    /// server says it granted. A short answer must not read as success: it is
    /// the one failure mode that would otherwise be indistinguishable from a
    /// complete transfer and would silently truncate the artifact.
    P2pResult<RangeReply> RemoteRange(const std::string& host, uint16_t port,
                                      const std::string& locator,
                                      uint64_t start, uint64_t end) {
        char range[96];
        std::snprintf(range, sizeof(range), "bytes=%llu-%llu",
                      static_cast<unsigned long long>(start),
                      static_cast<unsigned long long>(end));

        core::http::Request req;
        req.method = "GET";
        req.path = std::string(kBlobPathPrefix) + locator;
        req.headers = AuthHeaders(options.shared_token);
        req.headers["Range"] = range;

        const core::http::HttpClient client(core::http::Target::Tcp(host, port),
                                            FetchTimeouts());
        const core::Expected<core::http::Response, std::string> response =
            client.Request(req);
        if (!response.ok()) {
            return core::make_unexpected(
                P2pError::InternalMessage("p2p blob GET", response.error()));
        }

        const core::Optional<std::string> content_range =
            core::http::FindHeader(response.value().headers, "Content-Range");

        // 416 is not simply an error: it is also how the size of an empty
        // artifact is reported, since no range over it is satisfiable.
        if (response.value().status == 416) {
            RangeReply reply;
            reply.satisfied = false;
            bool parsed_satisfied = false;
            if (!content_range.has_value() ||
                !ParseContentRangeHeader(*content_range, &parsed_satisfied, NULL, NULL,
                                         &reply.total)) {
                return core::make_unexpected(P2pError::Invalid(
                    "peer refused the range without saying how large the artifact is"));
            }
            return reply;
        }

        if (!response.value().IsSuccess()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "peer answered %d", response.value().status);
            return core::make_unexpected(P2pError::InternalMessage("p2p blob GET", buf));
        }

        RangeReply reply;
        if (!content_range.has_value() ||
            !ParseContentRangeHeader(*content_range, &reply.satisfied, &reply.start,
                                     &reply.end, &reply.total)) {
            // Without it there is no way to know what was actually granted,
            // and assuming the request was honoured verbatim is how a clamped
            // reply becomes silent corruption.
            return core::make_unexpected(
                P2pError::Invalid("p2p blob GET carried no usable Content-Range"));
        }
        if (!reply.satisfied) {
            return core::make_unexpected(
                P2pError::Invalid("peer answered 2xx with an unsatisfied Content-Range"));
        }
        if (reply.start != start) {
            return core::make_unexpected(P2pError::Invalid(
                "peer served a different range start than was asked for"));
        }
        if (reply.end > end) {
            return core::make_unexpected(P2pError::Invalid(
                "peer served more than the range that was asked for"));
        }

        const uint64_t granted = reply.end - reply.start + 1;
        if (static_cast<uint64_t>(response.value().body.size()) != granted) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "peer granted %llu-%llu but sent %llu bytes, not %llu",
                          static_cast<unsigned long long>(reply.start),
                          static_cast<unsigned long long>(reply.end),
                          static_cast<unsigned long long>(response.value().body.size()),
                          static_cast<unsigned long long>(granted));
            return core::make_unexpected(P2pError::Invalid(buf));
        }
        reply.bytes = response.value().body;
        return reply;
    }

    /// Picks a provider for `descriptor`: this node if it holds the bytes,
    /// otherwise the first peer provider that parses.
    ///
    /// Rust `resolve_fetch_source`, with the same local-first ordering: a
    /// local copy makes the whole network hop unnecessary.
    P2pResult<core::Optional<P2pPeer> > ResolveSource(
        const P2pArtifactDescriptor& descriptor) {
        const core::Expected<std::string, P2pError> locator =
            LocatorFromDescriptor(descriptor);
        if (!locator.ok()) return core::make_unexpected(locator.error());

        for (std::size_t i = 0; i < descriptor.providers.size(); ++i) {
            if (descriptor.providers[i].IsLocal()) {
                if (store->Acquire(locator.value())) {
                    return core::Optional<P2pPeer>();  // local
                }
            }
        }
        for (std::size_t i = 0; i < descriptor.providers.size(); ++i) {
            if (descriptor.providers[i].IsLocal()) continue;
            const P2pPeer& peer = descriptor.providers[i].peer;
            std::string host;
            uint16_t port = 0;
            const core::Expected<std::string, P2pError> address =
                EndpointAddressForBackend(peer.endpoint, kHttpBackendId);
            if (!address.ok()) continue;
            if (!ParseHostPort(address.value(), &host, &port)) continue;
            return core::Optional<P2pPeer>(peer);
        }
        return core::make_unexpected(
            P2pError::Invalid("descriptor names no reachable provider"));
    }

    /// Rust `try_advertise_fetched_blob` — record what we just pulled so this
    /// node becomes a provider too.
    ///
    /// Failures are logged, not propagated: the caller asked for bytes and got
    /// them. Turning a successful fetch into an error because the follow-on
    /// bookkeeping failed would be strictly worse for them.
    void TryAdvertise(const P2pArtifactDescriptor& descriptor,
                      const std::string& path, bool advertise) {
        if (!advertise) return;

        const core::Expected<BlobEntry, P2pError> imported =
            store->ImportFetched(descriptor.key, path);
        if (!imported.ok()) {
            AGENTENV_WARN("p2p: could not adopt fetched artifact key=" << descriptor.key <<
                          " error=" << imported.error().ToString());
            return;
        }

        P2pArtifactDescriptor local;
        local.key = descriptor.key;
        local.providers.push_back(P2pArtifactProvider::MakeLocal());
        local.has_backend_locator = true;
        local.backend_locator = imported.value().digest;
        local.metadata_json = descriptor.metadata_json;

        const core::Expected<core::Unit, P2pError> upserted = catalog->Upsert(local);
        if (!upserted.ok()) {
            AGENTENV_WARN("p2p: could not advertise fetched artifact key=" << descriptor.key <<
                          " error=" << upserted.error().ToString());
            return;
        }
        discovery->RecordKey(descriptor.key);
    }
};

// ---- the range stream -------------------------------------------------------

namespace {

/// `FetchByteRange`'s result: issues one bounded request per `Next()`.
///
/// Holds a `BlobLease` for the local case, mirroring the `TempTag` Rust keeps
/// inside its lazy stream "until EOF or caller cancellation" -- without it an
/// unpublish arriving mid-stream could delete the bytes being read.
///
/// The contract from `transport.h` is that the stream yields exactly `len`
/// bytes or ends with an error. `Next()` therefore only reports exhaustion
/// when `remaining_ == 0`; a peer that stops early produces an error, because
/// a silent `false` is what would truncate the caller's data.
class RangeStream : public P2pByteStream {
 public:
    RangeStream(std::shared_ptr<HttpP2pTransport::Impl> impl,
                core::Optional<P2pPeer> peer, std::string locator,
                std::string local_path, std::shared_ptr<BlobLease> lease,
                uint64_t offset, uint64_t len)
        : impl_(std::move(impl)), peer_(std::move(peer)), locator_(std::move(locator)),
          local_path_(std::move(local_path)), lease_(std::move(lease)),
          next_offset_(offset), remaining_(len), failed_(false) {}

    P2pResult<bool> Next(std::vector<uint8_t>* chunk_out) override {
        if (failed_) {
            return core::make_unexpected(
                P2pError::Invalid("range stream already failed"));
        }
        if (remaining_ == 0) {
            if (chunk_out) chunk_out->clear();
            return false;
        }

        uint64_t want = remaining_;
        if (want > kMaxRangeBytes) want = kMaxRangeBytes;
        const uint64_t start = next_offset_;
        const uint64_t end = start + want - 1;
        std::string bytes;
        if (!peer_.has_value()) {
            const core::Expected<core::Unit, std::string> read =
                ReadFileRange(local_path_, start, want, &bytes);
            if (!read.ok()) {
                failed_ = true;
                return core::make_unexpected(
                    P2pError::InternalMessage("read local p2p blob range", read.error()));
            }
        } else {
            std::string host;
            uint16_t port = 0;
            const core::Expected<std::string, P2pError> address =
                EndpointAddressForBackend(peer_->endpoint, kHttpBackendId);
            if (!address.ok()) {
                failed_ = true;
                return core::make_unexpected(address.error());
            }
            if (!ParseHostPort(address.value(), &host, &port)) {
                failed_ = true;
                return core::make_unexpected(P2pError::Invalid("unparseable peer address"));
            }
            const P2pResult<HttpP2pTransport::Impl::RangeReply> fetched =
                impl_->RemoteRange(host, port, locator_, start, end);
            if (!fetched.ok()) {
                failed_ = true;
                return core::make_unexpected(fetched.error());
            }
            if (!fetched.value().satisfied) {
                // The window the caller asked for runs past the artifact. It
                // is an error rather than an early end of stream: the caller
                // was promised `len` bytes and must not be handed fewer
                // without being told.
                failed_ = true;
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "range %llu-%llu lies outside the artifact (%llu bytes)",
                              static_cast<unsigned long long>(start),
                              static_cast<unsigned long long>(end),
                              static_cast<unsigned long long>(fetched.value().total));
                return core::make_unexpected(P2pError::Invalid(buf));
            }
            bytes = fetched.value().bytes;
            // The server is allowed to grant less than was asked for, so the
            // cursor advances by what arrived rather than by what was wanted.
            // Tracking the request instead would skip the remainder and
            // deliver a window with a hole in it.
            want = fetched.value().end - fetched.value().start + 1;
        }

        next_offset_ += want;
        remaining_ -= want;
        if (chunk_out) {
            chunk_out->assign(bytes.begin(), bytes.end());
        }
        return true;
    }

 private:
    std::shared_ptr<HttpP2pTransport::Impl> impl_;
    core::Optional<P2pPeer>                 peer_;
    std::string                             locator_;
    std::string                             local_path_;
    std::shared_ptr<BlobLease>              lease_;
    uint64_t                                next_offset_;
    uint64_t                                remaining_;
    bool                                    failed_;
};

}  // namespace

// ---- start / stop -----------------------------------------------------------

core::Expected<std::shared_ptr<HttpP2pTransport>, P2pError>
HttpP2pTransport::Start(Options options, std::shared_ptr<P2pPeerDiscovery> discovery) {
    if (options.advertise_addr.empty()) {
        return core::make_unexpected(P2pError::Invalid(
            "the http P2P transport needs an advertise address peers can dial"));
    }
    std::string host;
    uint16_t port = 0;
    if (!ParseHostPort(options.advertise_addr, &host, &port)) {
        return core::make_unexpected(P2pError::Invalid(
            "p2p advertise address must be host:port, got " + options.advertise_addr));
    }

    const std::string root = core::fs::Join(options.config.store_dir, kHttpBackendId);
    const core::Expected<core::Unit, std::string> created = core::fs::CreateDirAll(root);
    if (!created.ok()) {
        return core::make_unexpected(
            P2pError::InternalMessage("create P2P store dir", created.error()));
    }

    std::shared_ptr<Impl> impl(new Impl());
    impl->options = options;
    impl->discovery = std::move(discovery);

    const core::Expected<std::shared_ptr<HttpBlobStore>, P2pError> store =
        HttpBlobStore::Open(root);
    if (!store.ok()) return core::make_unexpected(store.error());
    impl->store = store.value();

    P2pEndpoint local;
    local.backend = kHttpBackendId;
    local.address = options.advertise_addr;

    const core::Expected<std::shared_ptr<PublishedArtifactCatalog>, P2pError> catalog =
        PublishedArtifactCatalog::Load(core::fs::Join(root, "catalog.db"),
                                       options.node_id, local);
    if (!catalog.ok()) return core::make_unexpected(catalog.error());
    impl->catalog = catalog.value();

    impl->server = options.server;
    if (!impl->server) {
        // The caller owns the server because `agentenv-p2p` cannot link
        // `agentenv-api` (see the note on `P2pBlobServer`). Reporting it is
        // better than serving nothing: a transport that published its address
        // and then answered no requests would look to every peer like a
        // provider that lies.
        return core::make_unexpected(P2pError::Invalid(
            "the http P2P transport needs a blob server; the caller must inject one"));
    }
    RegisterHttpP2pRoutes(impl->server.get(), impl->catalog, impl->store,
                          options.shared_token);

    // `listen_addr` is what to bind; `advertise_addr` is what to tell peers.
    // Binding the advertise address would work on a node configured with a
    // concrete address and fail on one configured with a wildcard.
    const std::string bind = options.config.has_listen_addr
                                 ? options.config.listen_addr
                                 : options.advertise_addr;

    std::shared_ptr<P2pBlobServer> server = impl->server;
    impl->serve_thread = std::thread([server, bind]() {
        const core::Expected<core::Unit, std::string> served =
            server->ListenAndServe(bind);
        if (!served.ok()) {
            AGENTENV_ERROR("p2p: the blob server stopped serving bind=" << bind <<
                           " error=" << served.error());
        }
    });

    AGENTENV_INFO("p2p: http artifact transport started node=" << options.node_id <<
                  " serving=" << bind << " advertising=" << options.advertise_addr);

    return std::shared_ptr<HttpP2pTransport>(new HttpP2pTransport(impl));
}

HttpP2pTransport::~HttpP2pTransport() { Shutdown(); }

P2pResult<core::Unit> HttpP2pTransport::Shutdown() {
    if (!impl_ || impl_->stopped) return core::Unit{};
    impl_->stopped = true;
    if (impl_->server) impl_->server->Stop();
    if (impl_->serve_thread.joinable()) impl_->serve_thread.join();
    return core::Unit{};
}

std::shared_ptr<PublishedArtifactCatalog> HttpP2pTransport::catalog() const {
    return impl_->catalog;
}
std::shared_ptr<HttpBlobStore> HttpP2pTransport::store() const { return impl_->store; }

bool HttpP2pTransport::LocalEndpoint(P2pEndpoint* out) const {
    if (out) {
        out->backend = kHttpBackendId;
        out->address = impl_->options.advertise_addr;
    }
    return true;
}

// ---- lookup -----------------------------------------------------------------

P2pResult<bool>
HttpP2pTransport::LookupWithHints(const P2pArtifactKey& key,
                                  const std::vector<P2pArtifactProviderHint>& hints,
                                  P2pArtifactDescriptor* out) {
    // Local first. Rust `get_local` short-circuits before any peer traffic,
    // and it is not only a latency win: a node that holds the bytes is the
    // authoritative answer for itself.
    const core::Optional<P2pArtifactDescriptor> local = impl_->catalog->DescriptorFor(key);
    if (local.has_value()) {
        if (out) *out = *local;
        return true;
    }

    const P2pResult<std::vector<P2pPeer> > peers = impl_->discovery->PeersWithHints(hints);
    if (!peers.ok()) return core::make_unexpected(peers.error());

    // Rust `first_some_buffered_in_order`: run up to
    // MAX_CONCURRENT_CATALOG_LOOKUPS at a time but accept results in peer
    // order, so the answer does not depend on which peer happened to be
    // quickest. Without that, two nodes asking the same question could pick
    // different providers and the choice would be unreproducible.
    const std::vector<P2pPeer>& list = peers.value();
    for (std::size_t base = 0; base < list.size(); base += kMaxConcurrentCatalogLookups) {
        std::size_t count = list.size() - base;
        if (count > kMaxConcurrentCatalogLookups) count = kMaxConcurrentCatalogLookups;

        std::vector<core::Optional<P2pArtifactDescriptor> > results(count);
        std::vector<std::thread> workers;
        workers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const P2pPeer& peer = list[base + i];
            core::Optional<P2pArtifactDescriptor>* slot = &results[i];
            Impl* impl = impl_.get();
            workers.push_back(std::thread([impl, peer, key, slot]() {
                *slot = impl->LookupPeer(peer, key);
            }));
        }
        for (std::size_t i = 0; i < workers.size(); ++i) workers[i].join();

        for (std::size_t i = 0; i < count; ++i) {
            if (results[i].has_value()) {
                if (out) *out = *results[i];
                return true;
            }
        }
    }

    return false;  // Ok(None)
}

// ---- fetch ------------------------------------------------------------------

P2pResult<uint64_t>
HttpP2pTransport::FetchWithOptions(const P2pArtifactDescriptor& descriptor,
                                   const std::string& destination,
                                   P2pFetchOptions options) {
    const core::Expected<std::string, P2pError> locator =
        LocatorFromDescriptor(descriptor);
    if (!locator.ok()) return core::make_unexpected(locator.error());

    const P2pResult<core::Optional<P2pPeer> > source = impl_->ResolveSource(descriptor);
    if (!source.ok()) return core::make_unexpected(source.error());

    // Local hit: copy from the store rather than dialling ourselves.
    if (!source.value().has_value()) {
        std::shared_ptr<BlobLease> lease = impl_->store->Acquire(locator.value());
        if (!lease) {
            return core::make_unexpected(P2pError::Invalid("local artifact disappeared"));
        }
        const core::Expected<core::Unit, std::string> copied =
            core::fs::Copy(lease->entry().path, destination);
        if (!copied.ok()) {
            return core::make_unexpected(
                P2pError::InternalMessage("copy local p2p artifact", copied.error()));
        }
        return lease->entry().size;
    }

    std::string host;
    uint16_t port = 0;
    const core::Expected<std::string, P2pError> address =
        EndpointAddressForBackend(source.value()->endpoint, kHttpBackendId);
    if (!address.ok()) return core::make_unexpected(address.error());
    if (!ParseHostPort(address.value(), &host, &port)) {
        return core::make_unexpected(P2pError::Invalid("unparseable peer address"));
    }

    // Written under a temporary name and renamed on success, so a destination
    // that exists is always a complete artifact. A reader that raced a partial
    // download would otherwise see a valid-looking prefix.
    const std::string tmp = destination + ".p2p.tmp";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) {
            return core::make_unexpected(
                P2pError::InternalMessage("open p2p fetch destination", tmp));
        }

        // No size probe: the first range request reports the total in its
        // `Content-Range`, so the loop learns the size from the same response
        // that carries the first bytes. The alternative, a HEAD, costs an
        // extra connection and cannot be read back by `core::http` at all.
        uint64_t written = 0;
        uint64_t total = 0;
        bool total_known = false;
        for (;;) {
            uint64_t want = kFetchChunkBytes;
            if (total_known) {
                if (written >= total) break;
                if (total - written < want) want = total - written;
            }

            const P2pResult<Impl::RangeReply> chunk = impl_->RemoteRange(
                host, port, locator.value(), written, written + want - 1);
            if (!chunk.ok()) {
                f.close();
                core::fs::RemoveFile(tmp);
                return core::make_unexpected(chunk.error());
            }

            if (!total_known) {
                total = chunk.value().total;
                total_known = true;
                // An empty artifact: every range over it is unsatisfiable, so
                // 416 with a total of zero is the only way a peer can report
                // it. Nothing to write, and nothing wrong.
                if (total == 0) break;
            } else if (chunk.value().total != total) {
                // The artifact changed size mid-transfer, which means the
                // provider republished under the same locator. Splicing the
                // two halves would produce bytes neither version ever had;
                // the digest check below would catch it, but only after the
                // whole transfer, and with a far less specific message.
                f.close();
                core::fs::RemoveFile(tmp);
                return core::make_unexpected(P2pError::Invalid(
                    "the artifact changed size while it was being fetched"));
            }

            if (!chunk.value().satisfied) {
                // Only reachable if the total shrank between requests, since
                // the loop never asks past a known total.
                f.close();
                core::fs::RemoveFile(tmp);
                return core::make_unexpected(
                    P2pError::Invalid("peer stopped serving the artifact mid-transfer"));
            }

            f.write(chunk.value().bytes.data(),
                    static_cast<std::streamsize>(chunk.value().bytes.size()));
            if (!f) {
                f.close();
                core::fs::RemoveFile(tmp);
                return core::make_unexpected(
                    P2pError::MakeInternal("write p2p fetch destination failed"));
            }
            // Advanced by what arrived, not by what was asked for: the server
            // may grant a narrower range than requested.
            written += static_cast<uint64_t>(chunk.value().bytes.size());
            if (written >= total) break;
        }
    }

    // The transfer carried no per-chunk proof, unlike iroh's verified
    // streaming, so the whole-artifact digest is checked before the bytes are
    // allowed to take the destination's name. This is the one place that
    // catches a peer serving the wrong or corrupted content.
    const core::Expected<core::FileDigest, std::string> described =
        core::DescribeFile(tmp);
    if (!described.ok()) {
        core::fs::RemoveFile(tmp);
        return core::make_unexpected(
            P2pError::InternalMessage("digest p2p fetch", described.error()));
    }
    if (described.value().sha256 != locator.value()) {
        core::fs::RemoveFile(tmp);
        return core::make_unexpected(P2pError::Invalid(
            "fetched artifact digest " + described.value().sha256 +
            " does not match the descriptor locator " + locator.value()));
    }

    if (std::rename(tmp.c_str(), destination.c_str()) != 0) {
        core::fs::RemoveFile(tmp);
        return core::make_unexpected(
            P2pError::MakeInternal("rename p2p fetch destination failed"));
    }

    impl_->TryAdvertise(descriptor, destination, options.advertise);
    return described.value().size;
}

P2pResult<std::vector<uint8_t> >
HttpP2pTransport::FetchBytesWithOptions(const P2pArtifactDescriptor& descriptor,
                                        P2pFetchOptions options) {
    // Routed through the file path rather than accumulating in memory: this
    // overload has no length argument, so an artifact of any size would
    // otherwise be an unbounded allocation decided by the peer.
    const core::Expected<std::string, std::string> dir =
        core::fs::CreateTempDirIn(impl_->options.config.store_dir, "agentenv-p2p-");
    if (!dir.ok()) {
        return core::make_unexpected(
            P2pError::InternalMessage("stage p2p fetch", dir.error()));
    }
    const std::string path = core::fs::Join(dir.value(), "artifact");

    const P2pResult<uint64_t> fetched = FetchWithOptions(descriptor, path, options);
    if (!fetched.ok()) {
        core::fs::RemoveDirAll(dir.value());
        return core::make_unexpected(fetched.error());
    }

    const core::Expected<std::string, std::string> contents = core::fs::ReadToString(path);
    core::fs::RemoveDirAll(dir.value());
    if (!contents.ok()) {
        return core::make_unexpected(
            P2pError::InternalMessage("read staged p2p fetch", contents.error()));
    }
    return std::vector<uint8_t>(contents.value().begin(), contents.value().end());
}

P2pResult<std::shared_ptr<P2pByteStream> >
HttpP2pTransport::FetchByteRange(const P2pArtifactDescriptor& descriptor,
                                 uint64_t offset, size_t len) {
    const core::Expected<std::string, P2pError> locator =
        LocatorFromDescriptor(descriptor);
    if (!locator.ok()) return core::make_unexpected(locator.error());

    const P2pResult<core::Optional<P2pPeer> > source = impl_->ResolveSource(descriptor);
    if (!source.ok()) return core::make_unexpected(source.error());

    std::shared_ptr<BlobLease> lease;
    std::string local_path;
    if (!source.value().has_value()) {
        lease = impl_->store->Acquire(locator.value());
        if (!lease) {
            return core::make_unexpected(P2pError::Invalid("local artifact disappeared"));
        }
        // Checked here rather than per chunk: the range is a property of the
        // request, so an impossible one should fail before any bytes move.
        const uint64_t end = offset + static_cast<uint64_t>(len);
        if (end < offset || end > lease->entry().size) {
            return core::make_unexpected(P2pError::Invalid("range outside artifact"));
        }
        local_path = lease->entry().path;
    }

    std::shared_ptr<P2pByteStream> stream(
        new RangeStream(impl_, source.value(), locator.value(), local_path,
                        lease, offset, static_cast<uint64_t>(len)));
    return stream;
}

// ---- publish ----------------------------------------------------------------

P2pResult<core::Unit> HttpP2pTransport::Publish(const P2pPublishRequest& request) {
    const core::Expected<BlobEntry, P2pError> entry =
        impl_->store->Import(request.key, request.source, request.publish_mode);
    if (!entry.ok()) return core::make_unexpected(entry.error());

    P2pArtifactDescriptor descriptor;
    descriptor.key = request.key;
    descriptor.providers.push_back(P2pArtifactProvider::MakeLocal());
    descriptor.has_backend_locator = true;
    descriptor.backend_locator = entry.value().digest;
    descriptor.metadata_json = request.metadata_json;

    // The catalog persists before the index updates, so a failed write never
    // leaves this node claiming a publication the store lost.
    const core::Expected<core::Unit, P2pError> upserted = catalog()->Upsert(descriptor);
    if (!upserted.ok()) {
        // Roll the bytes back: a publication that is not in the catalog is
        // unreachable, and leaving the file behind would accumulate content no
        // key names.
        impl_->store->Remove(request.key);
        return core::make_unexpected(upserted.error());
    }

    // Best effort, like Rust: the scheduler is an accelerator for discovery,
    // not the record of what this node serves. That record is the catalog,
    // which has already been written.
    const P2pResult<core::Unit> recorded = impl_->discovery->RecordKey(request.key);
    if (!recorded.ok()) {
        AGENTENV_WARN("p2p: could not record key with the scheduler key=" << request.key <<
                      " error=" << recorded.error().ToString());
    }
    return core::Unit{};
}

P2pResult<bool> HttpP2pTransport::Unpublish(const P2pArtifactKey& key) {
    const core::Expected<core::Optional<P2pArtifactDescriptor>, P2pError> removed =
        impl_->catalog->Remove(key);
    if (!removed.ok()) return core::make_unexpected(removed.error());

    // The store is cleaned up even when the catalog had no entry: the two can
    // disagree after a crash between the two writes, and an unpublish is the
    // natural moment to reconcile them.
    const core::Expected<bool, P2pError> dropped = impl_->store->Remove(key);
    if (!dropped.ok()) return core::make_unexpected(dropped.error());

    const P2pResult<core::Unit> forgotten = impl_->discovery->ForgetKey(key);
    if (!forgotten.ok()) {
        AGENTENV_WARN("p2p: could not forget key at the scheduler key=" << key <<
                      " error=" << forgotten.error().ToString());
    }
    return removed.value().has_value();
}

}  // namespace p2p
}  // namespace agentenv
