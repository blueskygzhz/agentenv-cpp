// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/protocol.rs
#include "agentenv/storage/ublk_daemon/protocol.h"

#include <cstring>

#include "agentenv/core/serde.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

namespace sd = core::serde;

const char* AccessModeToString(AccessMode mode) {
    return mode == AccessMode::Shared ? "shared" : "exclusive";
}

core::Expected<AccessMode, std::string> AccessModeParse(const std::string& raw) {
    if (raw == "exclusive") return AccessMode::Exclusive;
    if (raw == "shared") return AccessMode::Shared;
    return core::make_unexpected(std::string("unknown access_mode '") + raw + "'");
}

namespace {

// `#[serde(tag = "kind", rename_all = "snake_case")]`. Requests and responses
// use *different* tag fields, so they cannot be decoded with each other's
// reader; a response read as a request would match no variant at all.
const char* const kRequestTag = "kind";
const char* const kResponseTag = "status";

struct NameKind {
    const char* name;
    int kind;
};

const NameKind kRequestKinds[] = {
    {"create_overlaybd", 0},
    {"create_overlaybd_runtime_device", 1},
    {"delete", 2},
    {"restack_snapshot", 3},
    {"get_features", 4},
    {"notify_sandbox_ready", 5},
    {"start_pack_recording", 6},
    {"pack_recording_status", 7},
    {"abort_pack_recording", 8},
    {"prefetch_startup_pack", 9},
    {"acquire_overlaybd", 10},
    {"release_overlaybd", 11},
    {"update_size", 12},
    {"shutdown", 13},
};

const NameKind kResponseKinds[] = {
    {"device_created", 0},
    {"overlaybd_runtime_device_created", 1},
    {"device_acquired", 2},
    {"released", 3},
    {"size_updated", 4},
    {"features", 5},
    {"deleted", 6},
    {"restack_snapshot_created", 7},
    {"pack_recording", 8},
    {"ok", 9},
    {"terminal_error", 10},
    {"invalid_request", 11},
    {"error", 12},
};

const char* KindName(const NameKind* table, std::size_t n, int kind) {
    for (std::size_t i = 0; i < n; ++i) {
        if (table[i].kind == kind) return table[i].name;
    }
    return "";
}

core::Expected<int, std::string> KindOf(const NameKind* table, std::size_t n,
                                        const std::string& name, const char* what) {
    for (std::size_t i = 0; i < n; ++i) {
        if (name == table[i].name) return table[i].kind;
    }
    return core::make_unexpected(std::string("unknown ") + what + " '" + name + "'");
}

void PutOptUint(core::JsonObject& o, const std::string& key,
                const core::Optional<uint64_t>& v) {
    // `Option<u64>` with no `skip_serializing_if`: serde writes an explicit
    // null, which the reader treats the same as absent.
    o[key] = v.has_value() ? core::Json(static_cast<int64_t>(*v)) : core::Json();
}

}  // namespace

// ── DaemonRequest ──────────────────────────────────────────────────────────

core::Json DaemonRequest::ToJson() const {
    core::JsonObject o;
    o[kRequestTag] = core::Json(std::string(KindName(
        kRequestKinds, sizeof(kRequestKinds) / sizeof(kRequestKinds[0]),
        static_cast<int>(kind))));

    switch (kind) {
    case Kind::CreateOverlaybd:
        o["image_config"] = core::Json(image_config);
        o["global_config"] = core::Json(global_config);
        break;
    case Kind::CreateOverlaybdRuntimeDevice:
        o["source_image_config"] = core::Json(source_image_config);
        o["global_config"] = core::Json(global_config);
        o["runtime_dir"] = core::Json(runtime_dir);
        o["read_only"] = core::Json(read_only);
        o["runtime_upper_mode"] =
            core::Json(std::string(overlaybd::UpperModeToString(runtime_upper_mode)));
        PutOptUint(o, "requested_virtual_size", requested_virtual_size);
        PutOptUint(o, "known_source_virtual_size", known_source_virtual_size);
        o["allow_shrink"] = core::Json(allow_shrink);
        break;
    case Kind::Delete:
    case Kind::PackRecordingStatus:
    case Kind::AbortPackRecording:
    case Kind::ReleaseOverlaybd:
        o["dev_id"] = core::Json(static_cast<int64_t>(dev_id));
        break;
    case Kind::RestackSnapshot:
        o["dev_id"] = core::Json(static_cast<int64_t>(dev_id));
        o["output_layer_path"] = core::Json(output_layer_path);
        break;
    case Kind::NotifySandboxReady:
        o["device_key"] = core::Json(device_key);
        break;
    case Kind::StartPackRecording:
        o["dev_id"] = core::Json(static_cast<int64_t>(dev_id));
        o["output"] = core::Json(output);
        o["max_pages"] = core::Json(static_cast<int64_t>(max_pages));
        o["min_window_ms"] = core::Json(static_cast<int64_t>(min_window_ms));
        o["quiet_ms"] = core::Json(static_cast<int64_t>(quiet_ms));
        o["max_window_ms"] = core::Json(static_cast<int64_t>(max_window_ms));
        break;
    case Kind::PrefetchStartupPack:
        o["image_config"] = core::Json(image_config);
        o["global_config"] = core::Json(global_config);
        o["url"] = core::Json(url);
        o["pack_size"] = core::Json(static_cast<int64_t>(pack_size));
        o["index_sha256"] = core::Json(index_sha256);
        o["mem_virtual_size"] = core::Json(static_cast<int64_t>(mem_virtual_size));
        o["timeout_secs"] = core::Json(static_cast<int64_t>(timeout_secs));
        break;
    case Kind::AcquireOverlaybd:
        o["image_config"] = core::Json(image_config);
        o["global_config"] = core::Json(global_config);
        PutOptUint(o, "virtual_size", virtual_size);
        o["access_mode"] = core::Json(std::string(AccessModeToString(access_mode)));
        break;
    case Kind::UpdateSize:
        o["dev_id"] = core::Json(static_cast<int64_t>(dev_id));
        o["new_sectors"] = core::Json(static_cast<int64_t>(new_sectors));
        break;
    case Kind::GetFeatures:
    case Kind::Shutdown:
        // Unit variants: serde writes only the tag.
        break;
    }
    return core::Json(o);
}

core::Expected<DaemonRequest, std::string> DaemonRequest::FromJson(const core::Json& value) {
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("daemon request must be an object"));
    }
    const core::JsonObject& f = value.as_object();
    const core::Expected<std::string, std::string> tag = sd::RequireString(f, kRequestTag);
    if (!tag.ok()) return core::make_unexpected(tag.error());
    const core::Expected<int, std::string> k =
        KindOf(kRequestKinds, sizeof(kRequestKinds) / sizeof(kRequestKinds[0]), tag.value(),
               "daemon request kind");
    if (!k.ok()) return core::make_unexpected(k.error());

    DaemonRequest out;
    out.kind = static_cast<Kind>(k.value());
    const uint64_t kU32Max = 0xffffffffull;
    const uint64_t kU64Max = 0x7fffffffffffffffull;

    switch (out.kind) {
    case Kind::CreateOverlaybd: {
        const core::Expected<std::string, std::string> a = sd::RequireString(f, "image_config");
        if (!a.ok()) return core::make_unexpected(a.error());
        const core::Expected<std::string, std::string> b = sd::RequireString(f, "global_config");
        if (!b.ok()) return core::make_unexpected(b.error());
        out.image_config = a.value();
        out.global_config = b.value();
        break;
    }
    case Kind::CreateOverlaybdRuntimeDevice: {
        const core::Expected<std::string, std::string> a =
            sd::RequireString(f, "source_image_config");
        if (!a.ok()) return core::make_unexpected(a.error());
        const core::Expected<std::string, std::string> b = sd::RequireString(f, "global_config");
        if (!b.ok()) return core::make_unexpected(b.error());
        const core::Expected<std::string, std::string> c = sd::RequireString(f, "runtime_dir");
        if (!c.ok()) return core::make_unexpected(c.error());
        const core::Expected<bool, std::string> ro = sd::RequireBool(f, "read_only");
        if (!ro.ok()) return core::make_unexpected(ro.error());
        out.source_image_config = a.value();
        out.global_config = b.value();
        out.runtime_dir = c.value();
        out.read_only = ro.value();
        // `#[serde(default = "default_runtime_upper_mode")]` — absent means
        // LogStructured, which is not the enum's first value, so this cannot
        // fall through to a zero-initialized default.
        const core::Json* mode = sd::OptValue(f, "runtime_upper_mode");
        if (mode != NULL && mode->kind() == core::Json::Kind::String) {
            const core::Expected<overlaybd::UpperMode, std::string> parsed =
                overlaybd::UpperModeParse(mode->as_string());
            if (!parsed.ok()) return core::make_unexpected(parsed.error());
            out.runtime_upper_mode = parsed.value();
        } else {
            out.runtime_upper_mode = overlaybd::UpperMode::LogStructured;
        }
        const core::Expected<core::Optional<uint64_t>, std::string> rv =
            sd::OptUint(f, "requested_virtual_size", kU64Max);
        if (!rv.ok()) return core::make_unexpected(rv.error());
        out.requested_virtual_size = rv.value();
        const core::Expected<core::Optional<uint64_t>, std::string> kv =
            sd::OptUint(f, "known_source_virtual_size", kU64Max);
        if (!kv.ok()) return core::make_unexpected(kv.error());
        out.known_source_virtual_size = kv.value();
        const core::Expected<bool, std::string> shrink =
            sd::DefaultBool(f, "allow_shrink", false);
        if (!shrink.ok()) return core::make_unexpected(shrink.error());
        out.allow_shrink = shrink.value();
        break;
    }
    case Kind::Delete:
    case Kind::PackRecordingStatus:
    case Kind::AbortPackRecording:
    case Kind::ReleaseOverlaybd: {
        const core::Expected<uint64_t, std::string> id = sd::RequireUint(f, "dev_id", kU32Max);
        if (!id.ok()) return core::make_unexpected(id.error());
        out.dev_id = static_cast<uint32_t>(id.value());
        break;
    }
    case Kind::RestackSnapshot: {
        const core::Expected<uint64_t, std::string> id = sd::RequireUint(f, "dev_id", kU32Max);
        if (!id.ok()) return core::make_unexpected(id.error());
        const core::Expected<std::string, std::string> p =
            sd::RequireString(f, "output_layer_path");
        if (!p.ok()) return core::make_unexpected(p.error());
        out.dev_id = static_cast<uint32_t>(id.value());
        out.output_layer_path = p.value();
        break;
    }
    case Kind::NotifySandboxReady: {
        const core::Expected<std::string, std::string> key = sd::RequireString(f, "device_key");
        if (!key.ok()) return core::make_unexpected(key.error());
        out.device_key = key.value();
        break;
    }
    case Kind::StartPackRecording: {
        const core::Expected<uint64_t, std::string> id = sd::RequireUint(f, "dev_id", kU32Max);
        if (!id.ok()) return core::make_unexpected(id.error());
        const core::Expected<std::string, std::string> o2 = sd::RequireString(f, "output");
        if (!o2.ok()) return core::make_unexpected(o2.error());
        const core::Expected<uint64_t, std::string> mp =
            sd::RequireUint(f, "max_pages", kU32Max);
        if (!mp.ok()) return core::make_unexpected(mp.error());
        const core::Expected<uint64_t, std::string> minw =
            sd::RequireUint(f, "min_window_ms", kU64Max);
        if (!minw.ok()) return core::make_unexpected(minw.error());
        const core::Expected<uint64_t, std::string> q =
            sd::RequireUint(f, "quiet_ms", kU64Max);
        if (!q.ok()) return core::make_unexpected(q.error());
        const core::Expected<uint64_t, std::string> maxw =
            sd::RequireUint(f, "max_window_ms", kU64Max);
        if (!maxw.ok()) return core::make_unexpected(maxw.error());
        out.dev_id = static_cast<uint32_t>(id.value());
        out.output = o2.value();
        out.max_pages = static_cast<uint32_t>(mp.value());
        out.min_window_ms = minw.value();
        out.quiet_ms = q.value();
        out.max_window_ms = maxw.value();
        break;
    }
    case Kind::PrefetchStartupPack: {
        const char* const kNames[3] = {"image_config", "global_config", "url"};
        std::string* const kTargets[3] = {&out.image_config, &out.global_config, &out.url};
        for (int i = 0; i < 3; ++i) {
            const core::Expected<std::string, std::string> v = sd::RequireString(f, kNames[i]);
            if (!v.ok()) return core::make_unexpected(v.error());
            *kTargets[i] = v.value();
        }
        const core::Expected<uint64_t, std::string> ps =
            sd::RequireUint(f, "pack_size", kU64Max);
        if (!ps.ok()) return core::make_unexpected(ps.error());
        const core::Expected<std::string, std::string> sha =
            sd::RequireString(f, "index_sha256");
        if (!sha.ok()) return core::make_unexpected(sha.error());
        const core::Expected<uint64_t, std::string> mvs =
            sd::RequireUint(f, "mem_virtual_size", kU64Max);
        if (!mvs.ok()) return core::make_unexpected(mvs.error());
        const core::Expected<uint64_t, std::string> ts =
            sd::RequireUint(f, "timeout_secs", kU64Max);
        if (!ts.ok()) return core::make_unexpected(ts.error());
        out.pack_size = ps.value();
        out.index_sha256 = sha.value();
        out.mem_virtual_size = mvs.value();
        out.timeout_secs = ts.value();
        break;
    }
    case Kind::AcquireOverlaybd: {
        const core::Expected<std::string, std::string> a = sd::RequireString(f, "image_config");
        if (!a.ok()) return core::make_unexpected(a.error());
        const core::Expected<std::string, std::string> b = sd::RequireString(f, "global_config");
        if (!b.ok()) return core::make_unexpected(b.error());
        const core::Expected<core::Optional<uint64_t>, std::string> vs =
            sd::OptUint(f, "virtual_size", kU64Max);
        if (!vs.ok()) return core::make_unexpected(vs.error());
        const core::Expected<std::string, std::string> am =
            sd::RequireString(f, "access_mode");
        if (!am.ok()) return core::make_unexpected(am.error());
        const core::Expected<AccessMode, std::string> parsed = AccessModeParse(am.value());
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        out.image_config = a.value();
        out.global_config = b.value();
        out.virtual_size = vs.value();
        out.access_mode = parsed.value();
        break;
    }
    case Kind::UpdateSize: {
        const core::Expected<uint64_t, std::string> id = sd::RequireUint(f, "dev_id", kU32Max);
        if (!id.ok()) return core::make_unexpected(id.error());
        const core::Expected<uint64_t, std::string> ns =
            sd::RequireUint(f, "new_sectors", kU64Max);
        if (!ns.ok()) return core::make_unexpected(ns.error());
        out.dev_id = static_cast<uint32_t>(id.value());
        out.new_sectors = ns.value();
        break;
    }
    case Kind::GetFeatures:
    case Kind::Shutdown:
        break;
    }
    return out;
}

// ── PackRecordingState ─────────────────────────────────────────────────────

core::Json PackRecordingState::ToJson() const {
    core::JsonObject o;
    switch (kind) {
    case Kind::Recording:
        o["state"] = core::Json(std::string("recording"));
        break;
    case Kind::Done:
        o["state"] = core::Json(std::string("done"));
        o["pages"] = core::Json(static_cast<int64_t>(pages));
        o["bytes"] = core::Json(static_cast<int64_t>(bytes));
        o["remote_bytes"] = core::Json(static_cast<int64_t>(remote_bytes));
        o["path"] = core::Json(path);
        break;
    case Kind::Failed:
        o["state"] = core::Json(std::string("failed"));
        o["reason"] = core::Json(reason);
        break;
    }
    return core::Json(o);
}

core::Expected<PackRecordingState, std::string> PackRecordingState::FromJson(
    const core::Json& value) {
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("pack recording state must be an object"));
    }
    const core::JsonObject& f = value.as_object();
    const core::Expected<std::string, std::string> tag = sd::RequireString(f, "state");
    if (!tag.ok()) return core::make_unexpected(tag.error());

    PackRecordingState out;
    const uint64_t kU64Max = 0x7fffffffffffffffull;
    if (tag.value() == "recording") {
        out.kind = Kind::Recording;
        return out;
    }
    if (tag.value() == "done") {
        out.kind = Kind::Done;
        const core::Expected<uint64_t, std::string> p =
            sd::RequireUint(f, "pages", 0xffffffffull);
        if (!p.ok()) return core::make_unexpected(p.error());
        const core::Expected<uint64_t, std::string> b = sd::RequireUint(f, "bytes", kU64Max);
        if (!b.ok()) return core::make_unexpected(b.error());
        const core::Expected<uint64_t, std::string> rb =
            sd::RequireUint(f, "remote_bytes", kU64Max);
        if (!rb.ok()) return core::make_unexpected(rb.error());
        const core::Expected<std::string, std::string> path2 = sd::RequireString(f, "path");
        if (!path2.ok()) return core::make_unexpected(path2.error());
        out.pages = static_cast<uint32_t>(p.value());
        out.bytes = b.value();
        out.remote_bytes = rb.value();
        out.path = path2.value();
        return out;
    }
    if (tag.value() == "failed") {
        out.kind = Kind::Failed;
        const core::Expected<std::string, std::string> r = sd::RequireString(f, "reason");
        if (!r.ok()) return core::make_unexpected(r.error());
        out.reason = r.value();
        return out;
    }
    return core::make_unexpected(std::string("unknown pack recording state '") + tag.value() +
                                 "'");
}

// ── DaemonResponse ─────────────────────────────────────────────────────────

core::Json DaemonResponse::ToJson() const {
    core::JsonObject o;
    o[kResponseTag] = core::Json(std::string(KindName(
        kResponseKinds, sizeof(kResponseKinds) / sizeof(kResponseKinds[0]),
        static_cast<int>(status))));

    switch (status) {
    case Status::DeviceCreated:
    case Status::DeviceAcquired:
        o["dev_id"] = core::Json(static_cast<int64_t>(dev_id));
        o["device_path"] = core::Json(device_path);
        break;
    case Status::OverlaybdRuntimeDeviceCreated:
        o["dev_id"] = core::Json(static_cast<int64_t>(dev_id));
        o["device_path"] = core::Json(device_path);
        o["actual_virtual_size"] = core::Json(static_cast<int64_t>(actual_virtual_size));
        o["runtime_image_config_path"] = core::Json(runtime_image_config_path);
        break;
    case Status::Features:
        o["flags"] = core::Json(static_cast<int64_t>(flags));
        break;
    case Status::RestackSnapshotCreated:
        // All three are `Option` with `#[serde(default)]` on two of them.
        // Absent is meaningful: an older daemon omits them, which is not the
        // same as reporting zero.
        o["descriptor"] = has_descriptor ? descriptor : core::Json();
        o["data_stat"] = has_data_stat ? data_stat : core::Json();
        PutOptUint(o, "ext4_used_bytes", ext4_used_bytes);
        break;
    case Status::PackRecording:
        o["state"] = state.ToJson();
        break;
    case Status::TerminalError:
    case Status::InvalidRequest:
    case Status::Error:
        o["message"] = core::Json(message);
        break;
    case Status::Released:
    case Status::SizeUpdated:
    case Status::Deleted:
    case Status::Ok:
        break;
    }
    return core::Json(o);
}

core::Expected<DaemonResponse, std::string> DaemonResponse::FromJson(const core::Json& value) {
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("daemon response must be an object"));
    }
    const core::JsonObject& f = value.as_object();
    const core::Expected<std::string, std::string> tag = sd::RequireString(f, kResponseTag);
    if (!tag.ok()) return core::make_unexpected(tag.error());
    const core::Expected<int, std::string> k =
        KindOf(kResponseKinds, sizeof(kResponseKinds) / sizeof(kResponseKinds[0]), tag.value(),
               "daemon response status");
    if (!k.ok()) return core::make_unexpected(k.error());

    DaemonResponse out;
    out.status = static_cast<Status>(k.value());
    const uint64_t kU32Max = 0xffffffffull;
    const uint64_t kU64Max = 0x7fffffffffffffffull;

    switch (out.status) {
    case Status::DeviceCreated:
    case Status::DeviceAcquired: {
        const core::Expected<uint64_t, std::string> id = sd::RequireUint(f, "dev_id", kU32Max);
        if (!id.ok()) return core::make_unexpected(id.error());
        const core::Expected<std::string, std::string> p = sd::RequireString(f, "device_path");
        if (!p.ok()) return core::make_unexpected(p.error());
        out.dev_id = static_cast<uint32_t>(id.value());
        out.device_path = p.value();
        break;
    }
    case Status::OverlaybdRuntimeDeviceCreated: {
        const core::Expected<uint64_t, std::string> id = sd::RequireUint(f, "dev_id", kU32Max);
        if (!id.ok()) return core::make_unexpected(id.error());
        const core::Expected<std::string, std::string> p = sd::RequireString(f, "device_path");
        if (!p.ok()) return core::make_unexpected(p.error());
        const core::Expected<uint64_t, std::string> vs =
            sd::RequireUint(f, "actual_virtual_size", kU64Max);
        if (!vs.ok()) return core::make_unexpected(vs.error());
        const core::Expected<std::string, std::string> rp =
            sd::RequireString(f, "runtime_image_config_path");
        if (!rp.ok()) return core::make_unexpected(rp.error());
        out.dev_id = static_cast<uint32_t>(id.value());
        out.device_path = p.value();
        out.actual_virtual_size = vs.value();
        out.runtime_image_config_path = rp.value();
        break;
    }
    case Status::Features: {
        const core::Expected<uint64_t, std::string> fl = sd::RequireUint(f, "flags", kU64Max);
        if (!fl.ok()) return core::make_unexpected(fl.error());
        out.flags = fl.value();
        break;
    }
    case Status::RestackSnapshotCreated: {
        const core::Json* d = sd::OptValue(f, "descriptor");
        if (d != NULL) {
            out.has_descriptor = true;
            out.descriptor = *d;
        }
        const core::Json* ds = sd::OptValue(f, "data_stat");
        if (ds != NULL) {
            out.has_data_stat = true;
            out.data_stat = *ds;
        }
        const core::Expected<core::Optional<uint64_t>, std::string> e4 =
            sd::OptUint(f, "ext4_used_bytes", kU64Max);
        if (!e4.ok()) return core::make_unexpected(e4.error());
        out.ext4_used_bytes = e4.value();
        break;
    }
    case Status::PackRecording: {
        const core::Json* st = sd::OptValue(f, "state");
        if (st == NULL) {
            return core::make_unexpected(std::string("missing field `state`"));
        }
        const core::Expected<PackRecordingState, std::string> parsed =
            PackRecordingState::FromJson(*st);
        if (!parsed.ok()) return core::make_unexpected(parsed.error());
        out.state = parsed.value();
        break;
    }
    case Status::TerminalError:
    case Status::InvalidRequest:
    case Status::Error: {
        const core::Expected<std::string, std::string> m = sd::RequireString(f, "message");
        if (!m.ok()) return core::make_unexpected(m.error());
        out.message = m.value();
        break;
    }
    case Status::Released:
    case Status::SizeUpdated:
    case Status::Deleted:
    case Status::Ok:
        break;
    }
    return out;
}

// ── framing ────────────────────────────────────────────────────────────────

std::vector<uint8_t> EncodeMessage(const core::Json& message) {
    const std::string payload = message.ToString();
    const uint32_t len = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> out;
    out.reserve(4 + payload.size());
    // Big-endian, as Rust's `len.to_be_bytes()`. Getting this backwards does
    // not corrupt the body -- it makes the length absurd, so the peer's size
    // guard rejects the frame and the symptom names the wrong problem.
    out.push_back(static_cast<uint8_t>((len >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((len >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(len & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

core::Expected<uint32_t, std::string> DecodeMessageLength(const uint8_t prefix[4]) {
    const uint32_t len = (static_cast<uint32_t>(prefix[0]) << 24) |
                         (static_cast<uint32_t>(prefix[1]) << 16) |
                         (static_cast<uint32_t>(prefix[2]) << 8) |
                         static_cast<uint32_t>(prefix[3]);
    if (len > kMaxMessageSize) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "daemon message too large: %u bytes (max %u)", len, kMaxMessageSize);
        return core::make_unexpected(std::string(msg));
    }
    return len;
}

DecodeStatus DecodeMessage(const uint8_t* buf, std::size_t len, core::Json* out,
                           std::size_t* consumed, std::string* err) {
    if (consumed != NULL) *consumed = 0;
    // A short read is normal on a stream socket and must be retried; it is
    // reported apart from a malformed frame, which must not be.
    if (len < 4) return DecodeStatus::Incomplete;
    const core::Expected<uint32_t, std::string> payload_len = DecodeMessageLength(buf);
    if (!payload_len.ok()) {
        if (err != NULL) *err = payload_len.error();
        return DecodeStatus::Malformed;
    }
    const std::size_t total = 4 + static_cast<std::size_t>(payload_len.value());
    if (len < total) return DecodeStatus::Incomplete;

    const std::string body(reinterpret_cast<const char*>(buf + 4),
                           static_cast<std::size_t>(payload_len.value()));
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(body);
    if (!parsed.ok()) {
        if (err != NULL) *err = parsed.error().chain();
        return DecodeStatus::Malformed;
    }
    if (out != NULL) *out = parsed.value();
    if (consumed != NULL) *consumed = total;
    return DecodeStatus::Ok;
}

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
