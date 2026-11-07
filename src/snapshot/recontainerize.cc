// SPDX-License-Identifier: MIT
// Rust: src/snapshot/recontainerize.rs
#include "agentenv/snapshot/recontainerize.h"

#include <cstdlib>
#include <unistd.h>

#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/overlaybd/compact.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/zfile.h"

namespace agentenv {
namespace snapshot {

using repository::RepositoryError;
using repository::RepositoryResult;

namespace {

void UnlinkAndDelete(std::string* path) {
    if (path != NULL) {
        if (!path->empty()) ::unlink(path->c_str());
        delete path;
    }
}

const std::string& EmptyPath() {
    static const std::string empty;
    return empty;
}

}  // namespace

// ---- OwnedTempPath --------------------------------------------------------

OwnedTempPath::OwnedTempPath(const std::string& path) {
    if (path.empty()) return;
    holder_.reset(new std::string(path), &UnlinkAndDelete);
}

const std::string& OwnedTempPath::path() const {
    return holder_ ? *holder_ : EmptyPath();
}

core::Expected<OwnedTempPath, std::string> OwnedTempPath::Create(
    const std::string& prefix) {
    const char* env = ::getenv("TMPDIR");
    const std::string dir = (env != NULL && *env != '\0') ? std::string(env) : "/tmp";
    const core::Expected<core::fs::TempFile, std::string> temp =
        core::fs::CreateTempFileIn(dir, prefix);
    if (!temp.ok()) return core::make_unexpected(temp.error());
    // The descriptor is dropped here on purpose: callers reopen by path (the
    // compactor and the digest pass both do), and holding an extra fd open
    // across an upload would only pin the inode after the unlink.
    return OwnedTempPath(temp.value().path);
}

// ---- PreparedLayerUpload --------------------------------------------------

void PreparedLayerUpload::RetainSource(const OwnedTempPath& source) {
    if (temp_.valid()) return;
    temp_ = source;
}

PreparedLayerUpload PreparedLayerUpload::Make(const std::string& path,
                                                 const std::string& digest, uint64_t size,
                                                 const OwnedTempPath& temp) {
    PreparedLayerUpload out;
    out.path_ = path;
    out.digest_ = digest;
    out.size_ = size;
    out.temp_ = temp;
    return out;
}

namespace {

/// Rust `passthrough` — upload the source bytes unchanged, honouring the
/// trusted descriptor when one is available.
RepositoryResult<PreparedLayerUpload> Passthrough(
    const std::string& source, const core::Optional<std::string>& trusted_digest,
    uint64_t trusted_size) {
    if (trusted_digest.has_value()) {
        return PreparedLayerUpload::Make(source, *trusted_digest, trusted_size,
                                            OwnedTempPath());
    }
    const core::Expected<core::FileDigest, std::string> described =
        core::DescribeFile(source);
    if (!described.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "describe managed layer '" + source + "'", described.error()));
    }
    return PreparedLayerUpload::Make(source, described.value().sha256,
                                        described.value().size, OwnedTempPath());
}

}  // namespace

RepositoryResult<PreparedLayerUpload> PrepareLayerUpload(
    const std::string& source, const overlaybd::OverlaybdCompactOutput& mode,
    const core::Optional<std::string>& trusted_digest, uint64_t trusted_size) {
    if (mode.is_raw()) return Passthrough(source, trusted_digest, trusted_size);

    const core::Expected<std::shared_ptr<storage::overlaybd::LocalFile>, std::string> probe =
        storage::overlaybd::LocalFile::OpenRo(source);
    if (!probe.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "open layer '" + source + "' for zfile probe", probe.error()));
    }
    const core::Expected<int, std::string> zfile =
        storage::overlaybd::zfile::IsZFile(probe.value().get());
    if (!zfile.ok()) {
        // A file that claims to be a zfile but fails its header digest is an
        // error, not a 0. Returning it here is what keeps a corrupt layer from
        // being uploaded as though it were plain bytes.
        return core::make_unexpected(RepositoryError::Backend(
            "probe zfile header of layer '" + source + "'", zfile.error()));
    }
    if (zfile.value() == 1) {
        // Already compressed: uploaded unchanged, so the operation is
        // idempotent and a trusted descriptor still describes the bytes.
        return Passthrough(source, trusted_digest, trusted_size);
    }

    // A raw layer: compact it into a temporary zfile and describe *that*.
    const core::Expected<OwnedTempPath, std::string> temp =
        OwnedTempPath::Create("agentenv-zfile-layer-");
    if (!temp.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "create temp zfile layer for recontainerizing '" + source + "'", temp.error()));
    }

    std::vector<storage::overlaybd::LayerConfig> layers(1);
    layers[0].file = source;
    const core::Expected<core::Optional<std::string>, std::string> compacted =
        overlaybd::CompactLayers(layers, temp.value().path(), mode);
    if (!compacted.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "recontainerize layer '" + source + "' as zfile", compacted.error()));
    }
    if (!compacted.value().has_value()) {
        // A single input layer always produces output; fall back to the
        // original bytes if the compactor ever reports no data.
        return Passthrough(source, trusted_digest, trusted_size);
    }
    const std::string& output = *compacted.value();

    AGENTENV_DEBUG("recontainerized local layer as zfile for upload; layer uuid will be unset:"
                   " source=" << source << " output=" << output);

    // Re-hashed rather than carrying `trusted_digest` across: that descriptor
    // describes the *raw* source bytes, and these are the compressed ones. A
    // manifest built from the trusted digest here would name a blob whose
    // content does not hash to it, which no registry would accept and no
    // puller could verify.
    const core::Expected<core::FileDigest, std::string> described = core::DescribeFile(output);
    if (!described.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "describe recontainerized zfile layer '" + output + "'", described.error()));
    }
    // The temp path is handed over so the compacted file outlives this call
    // and is unlinked once the upload drops the last copy.
    return PreparedLayerUpload::Make(output, described.value().sha256, described.value().size,
                                        temp.value());
}

}  // namespace snapshot
}  // namespace agentenv
