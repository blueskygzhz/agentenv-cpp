// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/artifact.rs
//
// Canonical identity for an overlaybd layer blob, and the metadata published
// alongside it.
//
// The central problem this file solves: a registry blob URL is frequently
// *presigned*, so two nodes asking for the same layer see different URLs. A
// naive URL-keyed catalog would never share anything. So identity is derived
// from the digest embedded in the URL path whenever one is present, and only
// falls back to a query-stripped scheme/host/path key when it is not.
#ifndef AGENTENV_OVERLAYBD_P2P_ARTIFACT_H_
#define AGENTENV_OVERLAYBD_P2P_ARTIFACT_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/p2p/types.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

/// Rust `SHA256_HEX_LEN`.
extern const std::size_t kSha256HexLen;
/// Rust `PROTOCOL` — bumped when the published metadata shape changes.
extern const char* const kProtocol;
/// Rust `KEY_PREFIX`.
extern const char* const kKeyPrefix;

/// Rust struct `CanonicalBlobIdentity`.
struct CanonicalBlobIdentity {
    /// `digest/<digest>`, `uuid/<uuid>`, or `url/<scheme>://<host>[:port]<path>`.
    std::string key;
    /// Present only for the digest form.
    core::Optional<std::string> digest;

    /// Rust `from_digest` — lowercased, so casing cannot fork the key space.
    static CanonicalBlobIdentity FromDigest(const std::string& digest);
    /// Rust `from_uuid`.
    static CanonicalBlobIdentity FromUuid(const std::string& uuid);
    /// Rust `from_origin_url` — prefers an embedded `sha256:` digest, else a
    /// query-stripped URL key.
    static core::Expected<CanonicalBlobIdentity, std::string> FromOriginUrl(
        const std::string& url);

    bool operator==(const CanonicalBlobIdentity& o) const;
    bool operator!=(const CanonicalBlobIdentity& o) const { return !(*this == o); }
};

/// Rust struct `LayerMetadata` — what a publisher attaches to the artifact so
/// a consumer can reject a mismatched blob before reading any bytes.
struct LayerMetadata {
    std::string protocol;
    std::string canonical_key;
    core::Optional<std::string> digest;
    core::Optional<std::string> uuid;
    core::Optional<uint64_t> size;
    /// Hash of the origin URL, never the URL itself: it may be presigned and
    /// carry credentials.
    core::Optional<std::string> source_url_hash;

    /// Rust `from_digest`.
    static LayerMetadata FromDigest(const std::string& digest,
                                    const core::Optional<uint64_t>& size,
                                    const core::Optional<std::string>& source_url_hash);
    /// Rust `from_uuid`.
    static LayerMetadata FromUuid(const std::string& uuid, const core::Optional<uint64_t>& size);

    /// Rust `from_descriptor` — parses the descriptor's `metadata` value.
    static core::Expected<LayerMetadata, std::string> FromDescriptor(
        const agentenv::p2p::P2pArtifactDescriptor& descriptor);

    /// Rust `validate` — the static gate before trusting a peer's blob.
    core::Expected<core::Unit, std::string> Validate(
        const CanonicalBlobIdentity& canonical) const;

    /// Rust `to_value` — the JSON written into the published descriptor.
    std::string ToJson() const;
    static core::Expected<LayerMetadata, std::string> ParseJson(const std::string& text);

    bool operator==(const LayerMetadata& o) const;
    bool operator!=(const LayerMetadata& o) const { return !(*this == o); }
};

/// Rust `layer_key_from_digest`.
agentenv::p2p::P2pArtifactKey LayerKeyFromDigest(const std::string& digest);

/// Rust `layer_key_from_uuid`.
agentenv::p2p::P2pArtifactKey LayerKeyFromUuid(const std::string& uuid);

/// Rust `layer_artifact_key` — the catalog key. UUID keys are namespaced under
/// `uuid/` so they can never collide with a digest key.
agentenv::p2p::P2pArtifactKey LayerArtifactKey(const CanonicalBlobIdentity& canonical);

/// Rust `extract_sha256_digest`.
///
/// Requires a *boundary* after the 64 hex characters, so a longer hex run
/// (e.g. 66 chars) is not silently truncated into a valid-looking digest.
core::Optional<std::string> ExtractSha256Digest(const std::string& path);

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
#endif  // AGENTENV_OVERLAYBD_P2P_ARTIFACT_H_
