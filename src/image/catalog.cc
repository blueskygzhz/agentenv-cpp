// SPDX-License-Identifier: MIT
// Rust: src/image/catalog.rs
#include "agentenv/image/catalog.h"

#include <sstream>

#include "agentenv/core/digest.h"
#include "agentenv/core/json.h"
#include "agentenv/snapshot/record.h"

namespace agentenv {
namespace image {

namespace repo = snapshot::repository;

std::string ImageTagRecord::Encode() const {
    std::ostringstream os;
    os << "{\"imageDigest\":" << core::JsonQuote(image_digest)
       << ",\"createdAtUnixMs\":" << created_at_unix_ms << "}";
    return os.str();
}

core::Expected<ImageTagRecord, std::string> ImageTagRecord::Decode(const std::string& bytes) {
    const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(bytes);
    if (!value.ok()) return core::make_unexpected(value.error().chain());
    if (value.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("invalid type: expected struct ImageTagRecord"));
    }
    const core::JsonObject& fields = value.value().as_object();
    ImageTagRecord record;

    const core::JsonObject::const_iterator digest = fields.find("imageDigest");
    if (digest == fields.end()) {
        return core::make_unexpected(std::string("missing field `imageDigest`"));
    }
    if (digest->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("invalid type: field `imageDigest`"));
    }
    record.image_digest = digest->second.as_string();

    const core::JsonObject::const_iterator created = fields.find("createdAtUnixMs");
    if (created == fields.end()) {
        return core::make_unexpected(std::string("missing field `createdAtUnixMs`"));
    }
    if (created->second.kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("invalid type: field `createdAtUnixMs`"));
    }
    record.created_at_unix_ms = created->second.as_int();
    return record;
}

repo::RepositoryResult<std::string> PutImage(repo::SnapshotRepository& repository,
                                             const PublishedImage& image) {
    const core::Expected<std::pair<std::string, std::string>, std::string> encoded =
        image.Encode();
    if (!encoded.ok()) {
        return core::make_unexpected(repo::RepositoryError::Backend("encode image", encoded.error()));
    }
    const repo::RepositoryResult<repo::DocumentKey> key =
        repo::DocumentKey::Image(encoded.value().first);
    if (!key.ok()) return core::make_unexpected(key.error());
    const repo::RepositoryResult<core::Unit> put =
        repository.PutDocument(key.value(), encoded.value().second);
    if (!put.ok()) return core::make_unexpected(put.error());
    return encoded.value().first;
}

repo::RepositoryResult<core::Optional<PublishedImage> > GetImage(
    repo::SnapshotRepository& repository, const std::string& digest) {
    const repo::RepositoryResult<repo::DocumentKey> key = repo::DocumentKey::Image(digest);
    if (!key.ok()) return core::make_unexpected(key.error());
    const repo::RepositoryResult<core::Optional<std::string> > bytes =
        repository.GetDocument(key.value());
    if (!bytes.ok()) return core::make_unexpected(bytes.error());
    if (!bytes.value().has_value()) return core::Optional<PublishedImage>();
    const core::Expected<PublishedImage, std::string> image =
        PublishedImage::Decode(digest, *bytes.value());
    if (!image.ok()) {
        return core::make_unexpected(
            repo::RepositoryError::Backend("verify image description", image.error()));
    }
    return core::Optional<PublishedImage>(image.value());
}

repo::RepositoryResult<core::Unit> PutImageTag(repo::SnapshotRepository& repository,
                                               const snapshot::ImageTag& tag,
                                               const std::string& digest) {
    // Validates the digest before anything is written.
    const repo::RepositoryResult<repo::DocumentKey> image_key = repo::DocumentKey::Image(digest);
    if (!image_key.ok()) return core::make_unexpected(image_key.error());
    ImageTagRecord record;
    record.image_digest = digest;
    record.created_at_unix_ms = snapshot::NowUnixMs();
    return repository.PutDocument(repo::DocumentKey::ImageTag(tag), record.Encode());
}

repo::RepositoryResult<core::Optional<std::string> > GetImageTag(
    repo::SnapshotRepository& repository, const snapshot::ImageTag& tag) {
    const repo::RepositoryResult<core::Optional<std::string> > bytes =
        repository.GetDocument(repo::DocumentKey::ImageTag(tag));
    if (!bytes.ok()) return core::make_unexpected(bytes.error());
    if (!bytes.value().has_value()) return core::Optional<std::string>();

    // Rust `decode_tag`: a parse failure and a non-digest value are one error.
    const core::Expected<ImageTagRecord, std::string> record =
        ImageTagRecord::Decode(*bytes.value());
    if (!record.ok() || !core::IsSha256Digest(record.value().image_digest)) {
        return core::make_unexpected(repo::RepositoryError::Backend(
            "verify image tag",
            "image tag '" + tag.ToString() + "' does not hold an image digest"));
    }
    return core::Optional<std::string>(record.value().image_digest);
}

}  // namespace image
}  // namespace agentenv
