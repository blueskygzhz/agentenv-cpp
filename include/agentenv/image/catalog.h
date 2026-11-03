// SPDX-License-Identifier: MIT
// Rust: src/image/catalog.rs — published image descriptions and image tags,
// stored as repository documents so every backend follows the same rules.
//
// Rust implements `ImageCatalog` for every `R: Repository`; C++ has no
// blanket impls, so these are free functions over the repository.
#ifndef AGENTENV_IMAGE_CATALOG_H_
#define AGENTENV_IMAGE_CATALOG_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/published_image.h"
#include "agentenv/snapshot/image_tag.h"
#include "agentenv/snapshot/repository/interfaces.h"

namespace agentenv {
namespace image {

/// Rust `ImageTagRecord` — where an image tag points, and since when.
/// `rename_all = "camelCase"`; unknown fields are ignored on read.
struct ImageTagRecord {
    std::string image_digest;
    int64_t created_at_unix_ms;

    ImageTagRecord() : created_at_unix_ms(0) {}

    /// Rust `serde_json::to_vec` — struct declaration order:
    /// `{"imageDigest":..,"createdAtUnixMs":..}`.
    std::string Encode() const;
    /// Rust `serde_json::from_slice::<ImageTagRecord>`.
    static core::Expected<ImageTagRecord, std::string> Decode(const std::string& bytes);

    bool operator==(const ImageTagRecord& o) const {
        return image_digest == o.image_digest && created_at_unix_ms == o.created_at_unix_ms;
    }
};

/// Rust `put_image` — publishes the description under its digest. Callers
/// publish only after every referenced layer is durable; repeating it is
/// idempotent since the digest binds the complete description.
snapshot::repository::RepositoryResult<std::string> PutImage(
    snapshot::repository::SnapshotRepository& repository, const PublishedImage& image);

/// Rust `get_image` — verifies the stored bytes against `digest`.
snapshot::repository::RepositoryResult<core::Optional<PublishedImage> > GetImage(
    snapshot::repository::SnapshotRepository& repository, const std::string& digest);

/// Rust `put_image_tag` — points `tag` at `digest`, creating or moving it.
/// Callers check that the image is published under the tag's name.
snapshot::repository::RepositoryResult<core::Unit> PutImageTag(
    snapshot::repository::SnapshotRepository& repository, const snapshot::ImageTag& tag,
    const std::string& digest);

/// Rust `get_image_tag` — the digest `tag` points to; `None` when the tag
/// does not exist.
snapshot::repository::RepositoryResult<core::Optional<std::string> > GetImageTag(
    snapshot::repository::SnapshotRepository& repository, const snapshot::ImageTag& tag);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_CATALOG_H_
