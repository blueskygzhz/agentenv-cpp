// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/layer/
#include "agentenv/storage/overlaybd/layer.h"

#include <cstdio>
#include <sstream>

#include "agentenv/storage/overlaybd/lsmt.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

const char* const kCommitFileName = "overlaybd.commit";
const char* const kSealedFileName = "overlaybd.sealed";

std::unique_ptr<LayerStack> MakeLayerStack() {
    return nullptr;  // TODO: concrete stack once VirtualFile impls are ported.
}

namespace {

/// Rust `read_overlaybd_layer_trailer` — the trailer occupies the final
/// `lsmt::HeaderTrailer::SPACE` bytes of a sealed layer.
core::Expected<lsmt::HeaderTrailer, std::string> ReadTrailer(const std::string& path) {
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
                                     " is too small for trailer");
    }

    if (std::fseek(file, static_cast<long>(file_size - lsmt::HeaderTrailer::kSpace),
                   SEEK_SET) != 0) {
        std::fclose(file);
        return core::make_unexpected(std::string("seek overlaybd trailer ") + path);
    }

    std::vector<uint8_t> raw(static_cast<size_t>(lsmt::HeaderTrailer::kSpace));
    const size_t read = std::fread(&raw[0], 1, raw.size(), file);
    std::fclose(file);
    if (read != raw.size()) {
        return core::make_unexpected(std::string("read overlaybd trailer ") + path);
    }

    lsmt::HeaderTrailer trailer;
    if (!lsmt::HeaderTrailer::Deserialize(&raw[0], raw.size(), &trailer)) {
        return core::make_unexpected(std::string("invalid overlaybd trailer: ") + path);
    }
    // Rust asserts all four conditions together: a header, an index file or an
    // unsealed layer would all decode but carry meaningless metadata.
    if (!trailer.VerifyMagic() || !trailer.IsTrailer() || !trailer.IsDataFile() ||
        !trailer.IsSealed()) {
        return core::make_unexpected(std::string("overlaybd trailer mismatch for ") + path);
    }
    return trailer;
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

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
