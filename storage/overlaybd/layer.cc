// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/layer/layer_metadata.rs
#include "agentenv/storage/overlaybd/layer.h"

#include <cstdio>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/lsmt.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

const char* const kCommitFileName = "overlaybd.commit";
const char* const kSealedFileName = "overlaybd.sealed";

core::Optional<std::string> ResolveLocalLayerPath(const LayerConfig& layer) {
    if (!layer.file.empty()) {
        // A `file` carrying a scheme names a remote blob. Returning it would
        // hand a URL to a caller that is about to open(2) it, which fails far
        // from here with an error naming a path that was never a path.
        if (layer.file.find("://") != std::string::npos) {
            return core::Optional<std::string>();
        }
        return core::Optional<std::string>(layer.file);
    }
    if (layer.dir.empty()) return core::Optional<std::string>();

    // Commit before sealed, as Rust probes them. Both are tested for
    // existence rather than assumed: a directory with neither must yield
    // nothing, not a path that cannot be opened.
    const std::string commit = core::fs::Join(layer.dir, kCommitFileName);
    if (core::fs::Exists(commit)) return core::Optional<std::string>(commit);

    const std::string sealed = core::fs::Join(layer.dir, kSealedFileName);
    if (core::fs::Exists(sealed)) return core::Optional<std::string>(sealed);

    return core::Optional<std::string>();
}

namespace {

/// Reads `HeaderTrailer::kSpace` bytes from `offset` and deserializes them.
///
/// Shared by the header and trailer readers because the record is the same
/// shape at both ends of the file; only which end, and which predicates hold,
/// differ.
core::Expected<lsmt::HeaderTrailer, std::string>
ReadHeaderTrailerAt(const std::string& path, bool from_end) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == NULL) {
        return core::make_unexpected(std::string("open overlaybd layer ") + path);
    }

    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return core::make_unexpected(std::string("stat overlaybd layer ") + path);
    }
    const long size = std::ftell(file);
    if (size < 0) {
        std::fclose(file);
        return core::make_unexpected(std::string("stat overlaybd layer ") + path);
    }
    const uint64_t file_size = static_cast<uint64_t>(size);
    if (file_size < lsmt::HeaderTrailer::kSpace) {
        std::fclose(file);
        return core::make_unexpected(std::string("overlaybd layer ") + path +
                                     " is too small for its header");
    }

    const long offset =
        from_end ? static_cast<long>(file_size - lsmt::HeaderTrailer::kSpace) : 0L;
    if (std::fseek(file, offset, SEEK_SET) != 0) {
        std::fclose(file);
        return core::make_unexpected(std::string("seek overlaybd layer ") + path);
    }

    std::vector<uint8_t> raw(static_cast<size_t>(lsmt::HeaderTrailer::kSpace));
    const size_t read = std::fread(&raw[0], 1, raw.size(), file);
    std::fclose(file);
    if (read != raw.size()) {
        return core::make_unexpected(std::string("read overlaybd layer ") + path);
    }

    lsmt::HeaderTrailer record;
    if (!lsmt::HeaderTrailer::Deserialize(&raw[0], raw.size(), &record)) {
        return core::make_unexpected(std::string("invalid overlaybd record: ") + path);
    }
    return record;
}

/// Rust `read_overlaybd_layer_trailer` — the trailer occupies the final
/// `lsmt::HeaderTrailer::SPACE` bytes of a sealed layer.
core::Expected<lsmt::HeaderTrailer, std::string> ReadTrailer(const std::string& path) {
    core::Expected<lsmt::HeaderTrailer, std::string> record =
        ReadHeaderTrailerAt(path, /*from_end=*/true);
    if (!record.ok()) return record;
    // Rust asserts all four conditions together: a header, an index file or an
    // unsealed layer would all decode but carry meaningless metadata.
    if (!record.value().VerifyMagic() || !record.value().IsTrailer() ||
        !record.value().IsDataFile() || !record.value().IsSealed()) {
        return core::make_unexpected(std::string("overlaybd trailer mismatch for ") + path);
    }
    return record;
}

/// Rust `read_overlaybd_layer_header` — the record at offset 0.
core::Expected<lsmt::HeaderTrailer, std::string> ReadHeader(const std::string& path) {
    core::Expected<lsmt::HeaderTrailer, std::string> record =
        ReadHeaderTrailerAt(path, /*from_end=*/false);
    if (!record.ok()) return record;
    // Only magic and header-ness, as Rust checks here: sealedness is a
    // property of the trailer, and an RW layer has no trailer at all.
    if (!record.value().VerifyMagic() || !record.value().IsHeader()) {
        return core::make_unexpected(std::string("overlaybd header mismatch for ") + path);
    }
    return record;
}

}  // namespace

core::Expected<std::string, std::string>
ReadOverlaybdLayerUuid(const std::string& path) {
    core::Expected<lsmt::HeaderTrailer, std::string> trailer = ReadTrailer(path);
    if (!trailer.ok()) return core::make_unexpected(trailer.error());

    // The field is a fixed 37-byte NUL-padded buffer; Rust splits at the first
    // NUL and falls back to the nil uuid when the bytes are not a valid uuid.
    // An empty string is this port's nil.
    const uint8_t* uuid = trailer.value().uuid;
    size_t length = 0;
    while (length < sizeof(trailer.value().uuid) && uuid[length] != 0) ++length;

    const std::string text(reinterpret_cast<const char*>(uuid), length);
    // A uuid is 36 characters (8-4-4-4-12). Anything else is treated as nil,
    // matching Rust's `unwrap_or_else(|_| Uuid::nil())`.
    if (text.size() != 36) return std::string();
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return std::string();
            continue;
        }
        const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F');
        if (!is_hex) return std::string();
    }
    return text;
}

core::Expected<uint64_t, std::string>
ReadOverlaybdLayerVirtualSize(const std::string& path) {
    core::Expected<lsmt::HeaderTrailer, std::string> trailer = ReadTrailer(path);
    if (!trailer.ok()) return core::make_unexpected(trailer.error());
    return trailer.value().virtual_size;
}

core::Expected<bool, std::string>
ReadOverlaybdLayerIsSparseRw(const std::string& path) {
    core::Expected<lsmt::HeaderTrailer, std::string> header = ReadHeader(path);
    if (!header.ok()) return core::make_unexpected(header.error());
    return header.value().IsSparseRw();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
