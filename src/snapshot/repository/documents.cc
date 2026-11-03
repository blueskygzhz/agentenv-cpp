// SPDX-License-Identifier: MIT
// Rust: src/repository/layout.rs — `DocumentKey`.
#include "agentenv/snapshot/repository/documents.h"

#include "agentenv/core/digest.h"

namespace agentenv {
namespace snapshot {
namespace repository {

namespace {

const char* const kVolumeRecordsPrefix = "volumes/records/";
const char* const kImagesPrefix = "images/descriptions/";
const char* const kImageTagsPrefix = "images/tags/";
const char* const kImageBuildsPrefix = "images/builds/";
const char* const kSnapshotRecordsPrefix = "catalog/records/";

/// Rust `image_tags_prefix`.
std::string ImageTagsPrefix(const snapshot::ImageName& name) {
    return std::string(kImageTagsPrefix) + name.ToString() + "/";
}

}  // namespace

DocumentKey DocumentKey::BuildLog(const core::SnapshotId& id) {
    return DocumentKey(Kind::BuildLog, id.ToString());
}

DocumentKey DocumentKey::BuildCacheState() { return DocumentKey(Kind::BuildCacheState, ""); }

DocumentKey DocumentKey::VolumeRecord(const std::string& id) {
    return DocumentKey(Kind::VolumeRecord, id);
}

DocumentKey DocumentKey::VolumeAlias(const std::string& name) {
    return DocumentKey(Kind::VolumeAlias, name);
}

DocumentKey DocumentKey::SnapshotRecord(const core::SnapshotId& id) {
    return DocumentKey(Kind::SnapshotRecord, id.ToString());
}

DocumentKey DocumentKey::SnapshotAlias(const snapshot::SnapshotAlias& alias) {
    return DocumentKey(Kind::SnapshotAlias, alias.ToString());
}

RepositoryResult<DocumentKey> DocumentKey::Image(const std::string& digest) {
    if (!core::IsSha256Digest(digest)) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest("invalid image digest '" + digest + "'"));
    }
    return DocumentKey(Kind::Image, digest);
}

DocumentKey DocumentKey::ImageTag(const snapshot::ImageTag& tag) {
    DocumentKey key(Kind::ImageTag, tag.ToString());
    key.tag_ = core::Optional<snapshot::ImageTag>(tag);
    return key;
}

DocumentKey DocumentKey::ImageBuild(const core::SnapshotId& id) {
    return DocumentKey(Kind::ImageBuild, id.ToString());
}

std::string DocumentKey::Path() const {
    switch (kind_) {
        case Kind::BuildLog:
            return "snapshots/build-logs/" + value_ + ".json";
        case Kind::BuildCacheState:
            return "template-build/cache-head.json";
        case Kind::VolumeRecord:
            return kVolumeRecordsPrefix + value_ + ".json";
        case Kind::VolumeAlias:
            return "volumes/aliases/" + value_ + ".json";
        case Kind::SnapshotRecord:
            return kSnapshotRecordsPrefix + value_ + ".json";
        case Kind::SnapshotAlias:
            return "catalog/aliases/" + value_ + ".json";
        case Kind::Image:
            return kImagesPrefix + value_ + ".json";
        case Kind::ImageTag:
            return ImageTagsPrefix(tag_->name()) + tag_->tag() + ".json";
        case Kind::ImageBuild:
            return kImageBuildsPrefix + value_ + ".json";
    }
    return std::string();
}

std::string DocumentKey::PosixPath() const {
    switch (kind_) {
        case Kind::VolumeAlias:
            return "volumes/aliases/" + value_;
        case Kind::SnapshotAlias:
            return "catalog/aliases/" + value_;
        default:
            return Path();
    }
}

std::string DocumentKey::PosixLockPath() const {
    switch (kind_) {
        case Kind::BuildCacheState:
            return "volumes/aliases/aenv-buildkit-cache-head.lock";
        case Kind::VolumeRecord:
            return kVolumeRecordsPrefix + value_ + ".lock";
        case Kind::VolumeAlias:
            return "volumes/aliases/" + value_ + ".lock";
        case Kind::SnapshotRecord:
            return kSnapshotRecordsPrefix + value_ + ".lock";
        case Kind::SnapshotAlias:
            return "catalog/aliases/" + value_ + ".lock";
        case Kind::Image:
            return kImagesPrefix + value_ + ".lock";
        case Kind::ImageTag:
            return ImageTagsPrefix(tag_->name()) + tag_->tag() + ".lock";
        case Kind::ImageBuild:
            return kImageBuildsPrefix + value_ + ".lock";
        case Kind::BuildLog:
            return Path() + ".lock";
    }
    return std::string();
}

const char* DocumentCollectionPrefix(DocumentCollection collection) {
    switch (collection) {
        case DocumentCollection::VolumeRecords:
            return kVolumeRecordsPrefix;
        case DocumentCollection::SnapshotRecords:
            return kSnapshotRecordsPrefix;
    }
    return kVolumeRecordsPrefix;
}

core::Optional<DocumentKey> DocumentCollectionKey(DocumentCollection collection,
                                                  const std::string& id) {
    switch (collection) {
        case DocumentCollection::VolumeRecords:
            return DocumentKey::VolumeRecord(id);
        case DocumentCollection::SnapshotRecords: {
            const core::Expected<core::SnapshotId, std::string> parsed =
                core::SnapshotId::Parse(id);
            if (!parsed.ok()) return core::Optional<DocumentKey>();
            return DocumentKey::SnapshotRecord(parsed.value());
        }
    }
    return core::Optional<DocumentKey>();
}

core::Optional<std::string> DocumentCollectionIdFromFileName(DocumentCollection,
                                                             const std::string& file_name) {
    // Both collections: Rust `file_name.strip_suffix(".json")`.
    static const std::string kSuffix = ".json";
    if (file_name.size() < kSuffix.size() ||
        file_name.compare(file_name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return core::Optional<std::string>();
    }
    return file_name.substr(0, file_name.size() - kSuffix.size());
}

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
