// SPDX-License-Identifier: MIT
// Rust: src/snapshot/startup_pack.rs — the two descriptor types only.
#include "agentenv/snapshot/startup_pack.h"

namespace agentenv {
namespace snapshot {
namespace {

/// Reads a `serde` `u64` field. Rejects a negative or non-integer value rather
/// than wrapping it: these are sizes, and a wrapped value would be enormous.
core::Expected<uint64_t, std::string> RequireUint(const core::Json& object,
                                                  const std::string& field) {
    const core::JsonObject& fields = object.as_object();
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }
    if (it->second.kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("field `") + field + "` must be an integer");
    }
    const int64_t value = it->second.as_int();
    if (value < 0) {
        return core::make_unexpected(std::string("field `") + field + "` must not be negative");
    }
    return static_cast<uint64_t>(value);
}

core::Expected<std::string, std::string> RequireString(const core::Json& object,
                                                       const std::string& field) {
    const core::JsonObject& fields = object.as_object();
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }
    if (it->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("field `") + field + "` must be a string");
    }
    return it->second.as_string();
}

}  // namespace

core::Json MemoryStartupPackInfo::ToJson() const {
    core::JsonObject object;
    object["packSize"] = core::Json(static_cast<int64_t>(pack_size));
    object["memVirtualSize"] = core::Json(static_cast<int64_t>(mem_virtual_size));
    object["indexSha256"] = core::Json(index_sha256);
    return core::Json(object);
}

core::Expected<MemoryStartupPackInfo, std::string> MemoryStartupPackInfo::FromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("memory startup pack info must be an object"));
    }
    MemoryStartupPackInfo info;

    const core::Expected<uint64_t, std::string> pack_size = RequireUint(json, "packSize");
    if (!pack_size.ok()) return core::make_unexpected(pack_size.error());
    info.pack_size = pack_size.value();

    const core::Expected<uint64_t, std::string> mem_virtual_size =
        RequireUint(json, "memVirtualSize");
    if (!mem_virtual_size.ok()) return core::make_unexpected(mem_virtual_size.error());
    info.mem_virtual_size = mem_virtual_size.value();

    const core::Expected<std::string, std::string> index_sha256 =
        RequireString(json, "indexSha256");
    if (!index_sha256.ok()) return core::make_unexpected(index_sha256.error());
    info.index_sha256 = index_sha256.value();

    return info;
}

core::Json ResolvedStartupPack::ToJson() const {
    core::JsonObject object;
    object["url"] = core::Json(url);
    object["packSize"] = core::Json(static_cast<int64_t>(pack_size));
    object["indexSha256"] = core::Json(index_sha256);
    object["memVirtualSize"] = core::Json(static_cast<int64_t>(mem_virtual_size));
    return core::Json(object);
}

core::Expected<ResolvedStartupPack, std::string> ResolvedStartupPack::FromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("resolved startup pack must be an object"));
    }
    ResolvedStartupPack pack;

    const core::Expected<std::string, std::string> url = RequireString(json, "url");
    if (!url.ok()) return core::make_unexpected(url.error());
    pack.url = url.value();

    const core::Expected<uint64_t, std::string> pack_size = RequireUint(json, "packSize");
    if (!pack_size.ok()) return core::make_unexpected(pack_size.error());
    pack.pack_size = pack_size.value();

    const core::Expected<std::string, std::string> index_sha256 =
        RequireString(json, "indexSha256");
    if (!index_sha256.ok()) return core::make_unexpected(index_sha256.error());
    pack.index_sha256 = index_sha256.value();

    const core::Expected<uint64_t, std::string> mem_virtual_size =
        RequireUint(json, "memVirtualSize");
    if (!mem_virtual_size.ok()) return core::make_unexpected(mem_virtual_size.error());
    pack.mem_virtual_size = mem_virtual_size.value();

    return pack;
}

core::Optional<ResolvedStartupPack> ResolveStartupPackRef(
    const core::Optional<MemoryStartupPackInfo>& info, bool consume_enabled,
    const std::string& url) {
    // Both gates fall back to a plain on-demand resume rather than an error:
    // a missing pack is the normal case, not a broken snapshot.
    if (!consume_enabled) return core::Optional<ResolvedStartupPack>();
    if (!info.has_value()) return core::Optional<ResolvedStartupPack>();

    ResolvedStartupPack pack;
    pack.url = url;
    pack.pack_size = info->pack_size;
    pack.index_sha256 = info->index_sha256;
    pack.mem_virtual_size = info->mem_virtual_size;
    return core::Optional<ResolvedStartupPack>(pack);
}

}  // namespace snapshot
}  // namespace agentenv
