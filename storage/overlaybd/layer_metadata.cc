// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/layer/layer_metadata.rs
#include "agentenv/storage/overlaybd/layer_metadata.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include "agentenv/storage/overlaybd/lsmt.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

const char* const kCommitFileName = "overlaybd.commit";
const char* const kSealedFileName = "overlaybd.sealed";

namespace {

bool PathExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

std::string Join(const std::string& dir, const char* name) {
    if (dir.empty()) return std::string(name);
    if (dir[dir.size() - 1] == '/') return dir + name;
    return dir + "/" + name;
}

/// Read one `HeaderTrailer` from `offset`.
///
/// `kSpace` bytes are read but only `kOnDiskSize` are parsed: the record is
/// 390 bytes inside a 4096-byte slot, and the remainder is padding.
core::Expected<lsmt::HeaderTrailer, std::string> ReadRecordAt(const std::string& path,
                                                                 uint64_t offset,
                                                                 const char* what) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == 0) {
        return core::make_unexpected("open overlaybd layer " + path);
    }
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(f);
        return core::make_unexpected(std::string("seek overlaybd ") + what + " " + path);
    }

    std::vector<uint8_t> raw(static_cast<size_t>(lsmt::HeaderTrailer::kSpace), 0);
    const size_t got = std::fread(&raw[0], 1, raw.size(), f);
    std::fclose(f);
    if (got != raw.size()) {
        return core::make_unexpected(std::string("read overlaybd ") + what + " " + path);
    }

    lsmt::HeaderTrailer out;
    if (!lsmt::HeaderTrailer::Deserialize(&raw[0], lsmt::HeaderTrailer::kOnDiskSize, &out)) {
        return core::make_unexpected(std::string("invalid overlaybd ") + what + ": " + path);
    }
    return out;
}

core::Expected<lsmt::HeaderTrailer, std::string> ReadHeader(const std::string& path) {
    const core::Expected<lsmt::HeaderTrailer, std::string> h = ReadRecordAt(path, 0, "header");
    if (!h.ok()) return h;
    if (!h.value().VerifyMagic() || !h.value().IsHeader()) {
        return core::make_unexpected("overlaybd header mismatch for " + path);
    }
    return h;
}

core::Expected<lsmt::HeaderTrailer, std::string> ReadTrailer(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return core::make_unexpected("stat overlaybd layer " + path);
    }
    const uint64_t file_size = static_cast<uint64_t>(st.st_size);
    if (file_size < lsmt::HeaderTrailer::kSpace) {
        return core::make_unexpected("overlaybd layer " + path + " is too small for trailer");
    }

    const core::Expected<lsmt::HeaderTrailer, std::string> t =
        ReadRecordAt(path, file_size - lsmt::HeaderTrailer::kSpace, "trailer");
    if (!t.ok()) return t;

    // All four, not just the magic. A header-shaped record at the tail, or an
    // unsealed one, means the layer is still being written -- readable as a
    // file, but not as a finished layer.
    if (!t.value().VerifyMagic() || !t.value().IsTrailer() || !t.value().IsDataFile() ||
        !t.value().IsSealed()) {
        return core::make_unexpected("overlaybd trailer mismatch for " + path);
    }
    return t;
}

}  // namespace

core::Optional<std::string> ResolveLocalLayerPath(const LayerConfig& layer) {
    if (!layer.file.empty()) {
        // A URL is reported as absent rather than as a path: opening it
        // locally would fail with a misleading "no such file" instead of
        // routing the layer to the remote path.
        if (layer.file.find("://") != std::string::npos) {
            return core::Optional<std::string>();
        }
        return core::Optional<std::string>(layer.file);
    }
    if (layer.dir.empty()) return core::Optional<std::string>();

    // Probed in this order: `commit` is the current name, `sealed` is only
    // still read for layers written before it settled.
    const std::string commit = Join(layer.dir, kCommitFileName);
    if (PathExists(commit)) return core::Optional<std::string>(commit);

    const std::string sealed = Join(layer.dir, kSealedFileName);
    if (PathExists(sealed)) return core::Optional<std::string>(sealed);

    return core::Optional<std::string>();
}

core::Expected<uint64_t, std::string> ReadOverlaybdLayerVirtualSize(const std::string& path) {
    const core::Expected<lsmt::HeaderTrailer, std::string> t = ReadTrailer(path);
    if (!t.ok()) return core::make_unexpected(t.error());
    return t.value().virtual_size;
}

core::Expected<std::string, std::string> ReadOverlaybdLayerUuid(const std::string& path) {
    const core::Expected<lsmt::HeaderTrailer, std::string> t = ReadTrailer(path);
    if (!t.ok()) return core::make_unexpected(t.error());

    // The field is a NUL-terminated string in a fixed slot. Rust parses it and
    // falls back to the nil uuid on failure rather than erroring: the uuid is
    // informational, and a layer predating uuid stamping still reads fine.
    const char* raw = reinterpret_cast<const char*>(t.value().uuid);
    const size_t len = ::strnlen(raw, sizeof(t.value().uuid));
    if (len == 0) return std::string("00000000-0000-0000-0000-000000000000");
    return std::string(raw, len);
}

core::Expected<bool, std::string> ReadOverlaybdLayerIsSparseRw(const std::string& path) {
    const core::Expected<lsmt::HeaderTrailer, std::string> h = ReadHeader(path);
    if (!h.ok()) return core::make_unexpected(h.error());
    return h.value().IsSparseRw();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
