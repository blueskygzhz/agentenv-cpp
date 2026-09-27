// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/artifact.rs
#include "agentenv/overlaybd/p2p/artifact.h"

#include <cctype>
#include <sstream>

#include "agentenv/core/digest.h"
#include "agentenv/core/json.h"
#include "agentenv/core/url.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

const std::size_t kSha256HexLen = 64;
const char* const kProtocol = "agentenv-overlaybd-layer-v1";
const char* const kKeyPrefix = "overlaybd-layer/v1";

namespace {

using core::Optional;
using core::Unit;

std::string ToLower(const std::string& value) {
    std::string out = value;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// Reads an optional string member, treating JSON `null` as absent.
Optional<std::string> OptionalString(const core::JsonObject& members, const char* key) {
    const core::JsonObject::const_iterator found = members.find(key);
    if (found == members.end()) return Optional<std::string>();
    if (found->second.kind() != core::Json::Kind::String) return Optional<std::string>();
    return Optional<std::string>(found->second.as_string());
}

Optional<uint64_t> OptionalUint(const core::JsonObject& members, const char* key) {
    const core::JsonObject::const_iterator found = members.find(key);
    if (found == members.end()) return Optional<uint64_t>();

    // Accept both spellings: a size may arrive as an integer or, from a
    // producer that went through a float, as a whole-valued double.
    if (found->second.kind() == core::Json::Kind::Int) {
        const int64_t value = found->second.as_int();
        if (value < 0) return Optional<uint64_t>();
        return Optional<uint64_t>(static_cast<uint64_t>(value));
    }
    if (found->second.kind() == core::Json::Kind::Double) {
        const double value = found->second.as_double();
        if (value < 0) return Optional<uint64_t>();
        return Optional<uint64_t>(static_cast<uint64_t>(value));
    }
    return Optional<uint64_t>();
}

core::Json StringOrNull(const Optional<std::string>& value) {
    return value.has_value() ? core::Json(*value) : core::Json();
}

bool SameOptionalString(const Optional<std::string>& a, const Optional<std::string>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a.has_value() || *a == *b;
}

}  // namespace

CanonicalBlobIdentity CanonicalBlobIdentity::FromDigest(const std::string& digest) {
    CanonicalBlobIdentity identity;
    const std::string lowered = ToLower(digest);
    identity.key = "digest/" + lowered;
    identity.digest = lowered;
    return identity;
}

CanonicalBlobIdentity CanonicalBlobIdentity::FromUuid(const std::string& uuid) {
    CanonicalBlobIdentity identity;
    identity.key = "uuid/" + uuid;
    // Deliberately no digest: a UUID-addressed layer has no content identity
    // yet, so nothing may be verified against a digest.
    return identity;
}

core::Expected<CanonicalBlobIdentity, std::string> CanonicalBlobIdentity::FromOriginUrl(
    const std::string& url) {
    const core::Expected<core::Url, std::string> parsed = core::Url::Parse(url);
    if (!parsed.ok()) {
        return core::make_unexpected(std::string("invalid origin url ") + url + ": " +
                                     parsed.error());
    }

    // A digest in the path is the strongest identity available and is immune
    // to presigning, so it wins whenever present.
    const Optional<std::string> digest = ExtractSha256Digest(parsed.value().path);
    if (digest.has_value()) return FromDigest(*digest);

    if (parsed.value().host.empty()) {
        return core::make_unexpected(std::string("origin url missing host"));
    }

    // Query deliberately excluded: it carries the presigned signature, which
    // differs per request for the same blob.
    CanonicalBlobIdentity identity;
    identity.key = "url/" + parsed.value().scheme + "://" + parsed.value().Authority() +
                   parsed.value().path;
    return identity;
}

bool CanonicalBlobIdentity::operator==(const CanonicalBlobIdentity& o) const {
    return key == o.key && SameOptionalString(digest, o.digest);
}

LayerMetadata LayerMetadata::FromDigest(const std::string& digest,
                                        const Optional<uint64_t>& size,
                                        const Optional<std::string>& source_url_hash) {
    const std::string lowered = ToLower(digest);
    LayerMetadata metadata;
    metadata.protocol = kProtocol;
    metadata.canonical_key = CanonicalBlobIdentity::FromDigest(lowered).key;
    metadata.digest = lowered;
    metadata.size = size;
    metadata.source_url_hash = source_url_hash;
    return metadata;
}

LayerMetadata LayerMetadata::FromUuid(const std::string& uuid, const Optional<uint64_t>& size) {
    LayerMetadata metadata;
    metadata.protocol = kProtocol;
    metadata.canonical_key = CanonicalBlobIdentity::FromUuid(uuid).key;
    metadata.uuid = uuid;
    metadata.size = size;
    return metadata;
}

core::Expected<LayerMetadata, std::string> LayerMetadata::ParseJson(const std::string& text) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
    if (!parsed.ok()) {
        return core::make_unexpected(std::string("parse layer metadata: ") +
                                     parsed.error().chain());
    }
    if (parsed.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("parse layer metadata: not an object"));
    }
    const core::JsonObject& members = parsed.value().as_object();

    LayerMetadata metadata;
    const Optional<std::string> protocol = OptionalString(members, "protocol");
    const Optional<std::string> canonical_key = OptionalString(members, "canonical_key");
    if (!protocol.has_value() || !canonical_key.has_value()) {
        // These two are what `Validate` checks; without them nothing can be
        // verified, so a partial object must not parse.
        return core::make_unexpected(
            std::string("parse layer metadata: missing protocol or canonical_key"));
    }
    metadata.protocol = *protocol;
    metadata.canonical_key = *canonical_key;
    metadata.digest = OptionalString(members, "digest");
    metadata.uuid = OptionalString(members, "uuid");
    metadata.size = OptionalUint(members, "size");
    metadata.source_url_hash = OptionalString(members, "source_url_hash");
    return metadata;
}

core::Expected<LayerMetadata, std::string> LayerMetadata::FromDescriptor(
    const agentenv::p2p::P2pArtifactDescriptor& descriptor) {
    return ParseJson(descriptor.metadata_json);
}

core::Expected<Unit, std::string> LayerMetadata::Validate(
    const CanonicalBlobIdentity& canonical) const {
    if (protocol != kProtocol) {
        return core::make_unexpected(std::string("unexpected protocol ") + protocol);
    }
    if (canonical_key != canonical.key) {
        return core::make_unexpected(std::string("canonical key mismatch"));
    }
    // Only compared when both sides carry one: a UUID-addressed canonical has
    // no digest to check against.
    if (canonical.digest.has_value() && digest.has_value() && *canonical.digest != *digest) {
        return core::make_unexpected(std::string("digest mismatch"));
    }
    if (StartsWith(canonical.key, "uuid/")) {
        const std::string expected = canonical.key.substr(5);
        if (!uuid.has_value() || *uuid != expected) {
            return core::make_unexpected(std::string("uuid mismatch"));
        }
    }
    return Unit();
}

std::string LayerMetadata::ToJson() const {
    core::JsonObject out;
    out["protocol"] = core::Json(protocol);
    out["canonical_key"] = core::Json(canonical_key);
    out["digest"] = StringOrNull(digest);
    out["uuid"] = StringOrNull(uuid);
    out["size"] = size.has_value() ? core::Json(static_cast<int64_t>(*size)) : core::Json();
    out["source_url_hash"] = StringOrNull(source_url_hash);
    return core::Json(out).ToString();
}

bool LayerMetadata::operator==(const LayerMetadata& o) const {
    if (protocol != o.protocol || canonical_key != o.canonical_key) return false;
    if (!SameOptionalString(digest, o.digest)) return false;
    if (!SameOptionalString(uuid, o.uuid)) return false;
    if (!SameOptionalString(source_url_hash, o.source_url_hash)) return false;
    if (size.has_value() != o.size.has_value()) return false;
    return !size.has_value() || *size == *o.size;
}

agentenv::p2p::P2pArtifactKey LayerArtifactKey(const CanonicalBlobIdentity& canonical) {
    // UUID keys get their own namespace so they can never alias a digest key.
    if (StartsWith(canonical.key, "uuid/")) {
        return std::string(kKeyPrefix) + "/uuid/" + canonical.key.substr(5);
    }
    if (canonical.digest.has_value()) {
        return std::string(kKeyPrefix) + "/" + *canonical.digest;
    }
    // A URL key is hashed so the catalog key stays a fixed length and does not
    // leak the path.
    return std::string(kKeyPrefix) + "/url/" + core::Sha256Hex(canonical.key);
}

agentenv::p2p::P2pArtifactKey LayerKeyFromDigest(const std::string& digest) {
    return LayerArtifactKey(CanonicalBlobIdentity::FromDigest(digest));
}

agentenv::p2p::P2pArtifactKey LayerKeyFromUuid(const std::string& uuid) {
    return LayerArtifactKey(CanonicalBlobIdentity::FromUuid(uuid));
}

Optional<std::string> ExtractSha256Digest(const std::string& path) {
    const std::string marker = "sha256:";
    const std::size_t pos = path.find(marker);
    if (pos == std::string::npos) return Optional<std::string>();

    const std::string candidate = path.substr(pos + marker.size());
    std::string hex;
    for (std::size_t i = 0; i < candidate.size() && hex.size() < kSha256HexLen; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(candidate[i]))) break;
        hex.push_back(candidate[i]);
    }
    if (hex.size() != kSha256HexLen) return Optional<std::string>();

    // The character after the digest must end it. Without this, a 66-hex-digit
    // path segment would be truncated to a plausible-looking 64-digit digest
    // and two different blobs could collide onto one key.
    if (hex.size() < candidate.size()) {
        const char next = candidate[hex.size()];
        const bool boundary = next == '/' || next == '?' || next == '&' || next == '#' ||
                              next == ':' || next == '@' || next == ',' || next == ';';
        if (!boundary) return Optional<std::string>();
    }
    return Optional<std::string>("sha256:" + ToLower(hex));
}

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
