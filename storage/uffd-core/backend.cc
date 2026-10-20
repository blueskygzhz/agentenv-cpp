// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/backend.rs + the `Deserialize`/`Debug` impls of
// `GuestRegionUffdMapping` in handler.rs.
#include "agentenv/storage/uffd-core/backend.h"

#include <sstream>

namespace agentenv {
namespace storage {
namespace uffd {
namespace {

/// Reads one unsigned field, refusing anything that is not a non-negative
/// integer.
///
/// Firecracker sends sizes and addresses; a negative or fractional value means
/// the handshake is not what this code thinks it is, and accepting it would
/// turn into an out-of-range region that silently matches no fault.
core::Expected<uint64_t, std::string> RequireU64(const core::JsonObject& obj,
                                                 const char* key) {
    const core::JsonObject::const_iterator it = obj.find(key);
    if (it == obj.end()) {
        return core::make_unexpected(std::string("missing field `") + key + "`");
    }
    if (it->second.kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("field `") + key + "` is not an integer");
    }
    const int64_t v = it->second.as_int();
    if (v < 0) {
        return core::make_unexpected(std::string("field `") + key + "` is negative");
    }
    return static_cast<uint64_t>(v);
}

}  // namespace

core::Expected<std::vector<GuestRegionUffdMapping>, std::string>
GuestRegionUffdMapping::ParseList(const std::string& json) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(json);
    if (!parsed.ok()) {
        return core::make_unexpected(std::string("cannot deserialize memory mappings: ") +
                                     parsed.error().chain());
    }
    if (parsed.value().kind() != core::Json::Kind::Array) {
        return core::make_unexpected(
            std::string("memory mappings body is not a JSON array"));
    }

    const core::JsonArray& arr = parsed.value().as_array();
    std::vector<GuestRegionUffdMapping> out;
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].kind() != core::Json::Kind::Object) {
            std::ostringstream o;
            o << "memory mapping " << i << " is not a JSON object";
            return core::make_unexpected(o.str());
        }
        const core::JsonObject& obj = arr[i].as_object();

        GuestRegionUffdMapping m;
        const char* keys[4] = {"base_host_virt_addr", "size", "offset", "page_size"};
        uint64_t* dsts[4] = {&m.base_host_virt_addr, &m.size, &m.offset, &m.page_size};
        for (int k = 0; k < 4; ++k) {
            const core::Expected<uint64_t, std::string> v = RequireU64(obj, keys[k]);
            if (!v.ok()) {
                std::ostringstream o;
                o << "memory mapping " << i << ": " << v.error();
                return core::make_unexpected(o.str());
            }
            *dsts[k] = v.value();
        }
        // A zero page size would make the handler divide by zero when it
        // aligns a fault address, so it is refused where it arrives rather
        // than where it would crash.
        if (m.page_size == 0) {
            std::ostringstream o;
            o << "memory mapping " << i << ": page_size is zero";
            return core::make_unexpected(o.str());
        }
        out.push_back(m);
    }
    return out;
}

std::string GuestRegionUffdMapping::ToString() const {
    std::ostringstream o;
    o << "GuestRegionUffdMapping { base_host_virt_addr: 0x" << std::hex
      << base_host_virt_addr << ", size: 0x" << size << ", offset: 0x" << offset
      << std::dec << ", page_size: " << page_size << " }";
    return o.str();
}

core::Expected<uint64_t, std::string> HvaToImageOffset(
    const std::vector<GuestRegionUffdMapping>& regions, uint64_t hva) {
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const GuestRegionUffdMapping& r = regions[i];
        if (hva >= r.base_host_virt_addr && hva < r.base_host_virt_addr + r.size) {
            return r.offset + (hva - r.base_host_virt_addr);
        }
    }
    std::ostringstream o;
    o << "find region for hva 0x" << std::hex << hva;
    return core::make_unexpected(o.str());
}

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
