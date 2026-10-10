// SPDX-License-Identifier: MIT
// Rust: the `firecracker_client::models` bodies that
// src/sandbox/firecracker/instance.rs sends.
//
// These are built as `core::Json` values rather than concatenated text. The
// bodies all carry host paths, and a path may legally contain a quote or a
// backslash: a concatenated body would then be invalid JSON, and a crafted one
// could close the object early and append a field of its own. That is the same
// break that was fixed in the MMDS document, and it matters more here because
// several of these fields decide what the guest boots from.
//
// Two shapes are reproduced exactly because Firecracker reads them as more
// than presence:
//   * `skip_serializing_if = "Option::is_none"` — an omitted field is "leave
//     alone" / "use the default", which `PATCH /drives` in particular depends
//     on; sending null instead is a different request.
//   * `TokenBucket::one_time_burst` — absent means no burst, while `0` is a
//     burst of zero tokens, which Firecracker can reject as invalid.
#include "agentenv/sandbox/firecracker/instance.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {
namespace {

/// Rust `TokenBucket`.
core::Json TokenBucketToJson(const TokenBucket& bucket) {
    core::JsonObject object;
    object["refill_time"] = core::Json(bucket.refill_time);
    object["size"] = core::Json(bucket.size);
    // `skip_serializing_if = "Option::is_none"`: a 0 burst must not be sent as
    // `one_time_burst: 0`, which is a request for a zero-token burst rather
    // than for no burst at all.
    if (bucket.one_time_burst.has_value()) {
        object["one_time_burst"] = core::Json(*bucket.one_time_burst);
    }
    return core::Json(object);
}

/// Rust `RateLimiter`. Either dimension may be absent.
core::Json RateLimiterToJson(const RateLimiter& limiter) {
    core::JsonObject object;
    if (limiter.bandwidth.has_value()) {
        object["bandwidth"] = TokenBucketToJson(*limiter.bandwidth);
    }
    if (limiter.ops.has_value()) {
        object["ops"] = TokenBucketToJson(*limiter.ops);
    }
    return core::Json(object);
}

void PutOptString(core::JsonObject& object, const std::string& key,
                  const core::Optional<std::string>& value) {
    if (value.has_value()) object[key] = core::Json(*value);
}

void PutOptLimiter(core::JsonObject& object, const std::string& key,
                   const core::Optional<RateLimiter>& limiter) {
    if (limiter.has_value()) object[key] = RateLimiterToJson(*limiter);
}

/// Rust `NetworkOverride`, used by both snapshot-load paths.
core::Json NetworkOverridesToJson(
    const std::vector<std::pair<std::string, std::string> >& overrides) {
    core::JsonArray array;
    for (std::size_t i = 0; i < overrides.size(); ++i) {
        core::JsonObject entry;
        entry["iface_id"] = core::Json(overrides[i].first);
        entry["host_dev_name"] = core::Json(overrides[i].second);
        array.push_back(core::Json(entry));
    }
    return core::Json(array);
}

}  // namespace

core::Json MachineConfigBody(uint32_t mem_size_mib, uint32_t vcpu_count, bool smt,
                             bool track_dirty_pages) {
    core::JsonObject object;
    object["mem_size_mib"] = core::Json(static_cast<int64_t>(mem_size_mib));
    object["vcpu_count"] = core::Json(static_cast<int64_t>(vcpu_count));
    object["smt"] = core::Json(smt);
    object["track_dirty_pages"] = core::Json(track_dirty_pages);
    return core::Json(object);
}

core::Expected<core::Json, std::string> LoggerBody(const std::string& log_path,
                                                   const std::string& level) {
    const core::Expected<std::string, std::string> parsed = ParseLogLevel(level);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    core::JsonObject object;
    object["level"] = core::Json(parsed.value());
    object["log_path"] = core::Json(log_path);
    object["show_level"] = core::Json(true);
    object["show_log_origin"] = core::Json(true);
    // `module` is left absent, as Rust sets it to `None`.
    return core::Json(object);
}

core::Json BootSourceBody(const std::string& kernel_image_path,
                          const core::Optional<std::string>& initrd_path,
                          const core::Optional<std::string>& boot_args) {
    core::JsonObject object;
    object["kernel_image_path"] = core::Json(kernel_image_path);
    PutOptString(object, "boot_args", boot_args);
    PutOptString(object, "initrd_path", initrd_path);
    return core::Json(object);
}

const char* IoEngineToString(IoEngine engine) {
    return engine == IoEngine::Async ? "Async" : "Sync";
}

core::Json DriveBody(const std::string& drive_id, const std::string& path_on_host,
                     bool is_root_device, bool read_only, bool direct, IoEngine io_engine,
                     const core::Optional<RateLimiter>& rate_limiter) {
    core::JsonObject object;
    object["drive_id"] = core::Json(drive_id);
    object["is_root_device"] = core::Json(is_root_device);
    object["path_on_host"] = core::Json(path_on_host);
    object["is_read_only"] = core::Json(read_only);
    object["direct"] = core::Json(direct);
    object["io_engine"] = core::Json(std::string(IoEngineToString(io_engine)));
    PutOptLimiter(object, "rate_limiter", rate_limiter);
    return core::Json(object);
}

core::Json PartialDriveBody(const std::string& drive_id,
                            const core::Optional<std::string>& path_on_host,
                            const core::Optional<RateLimiter>& rate_limiter) {
    core::JsonObject object;
    object["drive_id"] = core::Json(drive_id);
    // Both are `skip_serializing_if`. This is load-bearing: `PATCH /drives`
    // reads an absent field as "leave unchanged", so repointing a path must
    // not also reset the limiter, and vice versa.
    PutOptString(object, "path_on_host", path_on_host);
    PutOptLimiter(object, "rate_limiter", rate_limiter);
    return core::Json(object);
}

core::Json NetworkInterfaceBody(const std::string& iface_id,
                                const core::Optional<std::string>& guest_mac,
                                const std::string& host_dev_name,
                                const core::Optional<RateLimiter>& rx_rate_limiter,
                                const core::Optional<RateLimiter>& tx_rate_limiter) {
    core::JsonObject object;
    object["iface_id"] = core::Json(iface_id);
    object["host_dev_name"] = core::Json(host_dev_name);
    PutOptString(object, "guest_mac", guest_mac);
    PutOptLimiter(object, "rx_rate_limiter", rx_rate_limiter);
    PutOptLimiter(object, "tx_rate_limiter", tx_rate_limiter);
    return core::Json(object);
}

core::Json MmdsConfigBody(const std::string& iface_id) {
    core::JsonArray interfaces;
    interfaces.push_back(core::Json(iface_id));
    core::JsonObject object;
    object["network_interfaces"] = core::Json(interfaces);
    // V2 is explicit: Firecracker's default is V1, which hands the guest its
    // metadata without the session-token handshake.
    object["version"] = core::Json(std::string("V2"));
    return core::Json(object);
}

core::Json BalloonBody() {
    core::JsonObject object;
    object["amount_mib"] = core::Json(static_cast<int64_t>(0));
    object["deflate_on_oom"] = core::Json(true);
    object["free_page_reporting"] = core::Json(true);
    // `stats_polling_interval_s` and `free_page_hinting` stay absent, as Rust
    // leaves them `None`.
    return core::Json(object);
}

core::Json SnapshotCreateBody(const std::string& snapshot_path) {
    core::JsonObject object;
    object["snapshot_path"] = core::Json(snapshot_path);
    // Diff, and with no `mem_file_path`: AgentENV packages guest memory itself
    // from the dirty ranges, so asking Firecracker to also write a memory file
    // would duplicate the whole image.
    object["snapshot_type"] = core::Json(std::string("Diff"));
    return core::Json(object);
}

const char* MemoryBackendTypeToString(MemoryBackendType type) {
    return type == MemoryBackendType::Uffd ? "Uffd" : "File";
}

core::Json SnapshotLoadBody(
    const std::string& snapshot_path, MemoryBackendType backend_type,
    const std::string& backend_path, bool resume_vm, bool track_dirty_pages,
    bool track_dirty_pages_set,
    const std::vector<std::pair<std::string, std::string> >& network_overrides) {
    core::JsonObject backend;
    backend["backend_type"] = core::Json(std::string(MemoryBackendTypeToString(backend_type)));
    backend["backend_path"] = core::Json(backend_path);

    core::JsonObject object;
    object["snapshot_path"] = core::Json(snapshot_path);
    object["mem_backend"] = core::Json(backend);
    object["resume_vm"] = core::Json(resume_vm);
    if (track_dirty_pages_set) {
        object["track_dirty_pages"] = core::Json(track_dirty_pages);
    }
    // Rust only sets the field when the list is non-empty; an empty array and
    // an absent field are not the same request to Firecracker.
    if (!network_overrides.empty()) {
        object["network_overrides"] = NetworkOverridesToJson(network_overrides);
    }
    return core::Json(object);
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
