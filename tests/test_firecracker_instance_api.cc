// SPDX-License-Identifier: MIT
// Rust: the `firecracker_client::models` bodies that
// src/sandbox/firecracker/instance.rs sends, plus
// `parse_log_level` and the `GET /vm/dirty-memory-ranges` reply.
//
// These assertions are about the wire format Firecracker validates. A body it
// rejects fails mid-boot, far from whatever built it, so the shape is pinned
// here rather than discovered on a live VM.
#include "microtest.h"

#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/json.h"
#include "agentenv/sandbox/firecracker/instance.h"

using namespace agentenv;
using namespace agentenv::sandbox;
using namespace agentenv::sandbox::firecracker;

namespace {

const core::Json& Field(const core::Json& object, const std::string& key) {
    static const core::Json kNull;
    const core::JsonObject& fields = object.as_object();
    core::JsonObject::const_iterator it = fields.find(key);
    return it == fields.end() ? kNull : it->second;
}

bool Has(const core::Json& object, const std::string& key) {
    const core::JsonObject& fields = object.as_object();
    return fields.find(key) != fields.end();
}

core::Json Reparse(const core::Json& value) {
    core::Expected<core::Json, core::AnyError> v = core::Json::Parse(value.ToString());
    MT_EXPECT_TRUE(v.ok());
    return v.ok() ? v.value() : core::Json();
}

TokenBucket Bucket(int64_t refill, int64_t size) { return TokenBucket(refill, size); }

RateLimiter Limiter(const core::Optional<TokenBucket>& bw,
                    const core::Optional<TokenBucket>& ops) {
    RateLimiter l;
    l.bandwidth = bw;
    l.ops = ops;
    return l;
}

std::vector<std::pair<std::string, std::string> > Overrides() {
    std::vector<std::pair<std::string, std::string> > v;
    v.push_back(std::make_pair(std::string("eth0"), std::string("tap5")));
    return v;
}

}  // namespace

// ============================================================
// JSON escaping — the reason these are structured values
// ============================================================

// The previous port pasted paths straight into the body with no escaping, so a
// path containing a quote produced invalid JSON, and a crafted one could close
// the object and append a field of its own. Every body carries host paths, and
// boot args are partly API-supplied, so this is the same break that was fixed
// in the MMDS document.
MT_TEST(bodies_escape_paths_and_cannot_inject_fields) {
    const std::string nasty = "/img/a\",\"is_root_device\":true,\"x\":\"";
    const core::Json drive = DriveBody("d1", nasty, false, true, false, IoEngine::Sync,
                                       core::nullopt);
    const core::Json back = Reparse(drive);
    // The whole string stays one value, and the injected field does not exist.
    MT_EXPECT_EQ(Field(back, "path_on_host").as_string(), nasty);
    MT_EXPECT_TRUE(!Field(back, "is_root_device").as_bool());
    MT_EXPECT_TRUE(!Has(back, "x"));

    // Boot args reach the kernel command line, so the same rule applies.
    const core::Json boot = BootSourceBody(
        "/vm\\linux", core::nullopt,
        core::Optional<std::string>("console=ttyS0 bad=\"q\"\nnext=1\t"));
    const core::Json boot_back = Reparse(boot);
    MT_EXPECT_EQ(Field(boot_back, "kernel_image_path").as_string(), std::string("/vm\\linux"));
    MT_EXPECT_EQ(Field(boot_back, "boot_args").as_string(),
                 std::string("console=ttyS0 bad=\"q\"\nnext=1\t"));
}

// ============================================================
// machine / boot / logger
// ============================================================

MT_TEST(machine_config_body_matches_the_api_model) {
    const core::Json body = MachineConfigBody(2048, 4, true, false);
    MT_EXPECT_EQ(Field(body, "mem_size_mib").as_int(), static_cast<int64_t>(2048));
    MT_EXPECT_EQ(Field(body, "vcpu_count").as_int(), static_cast<int64_t>(4));
    MT_EXPECT_TRUE(Field(body, "smt").as_bool());
    MT_EXPECT_TRUE(!Field(body, "track_dirty_pages").as_bool());
}

MT_TEST(boot_source_body_omits_absent_optionals) {
    // `skip_serializing_if = "Option::is_none"`: omitted, not null.
    const core::Json bare = BootSourceBody("/vmlinux", core::nullopt, core::nullopt);
    MT_EXPECT_EQ(Field(bare, "kernel_image_path").as_string(), std::string("/vmlinux"));
    MT_EXPECT_TRUE(!Has(bare, "boot_args"));
    MT_EXPECT_TRUE(!Has(bare, "initrd_path"));

    const core::Json full = BootSourceBody("/vmlinux",
                                           core::Optional<std::string>("/initrd.img"),
                                           core::Optional<std::string>("console=ttyS0"));
    MT_EXPECT_EQ(Field(full, "initrd_path").as_string(), std::string("/initrd.img"));
    MT_EXPECT_EQ(Field(full, "boot_args").as_string(), std::string("console=ttyS0"));
}

MT_TEST(logger_body_normalizes_level_and_sets_both_show_flags) {
    const core::Expected<core::Json, std::string> body = LoggerBody("/var/log/fc.log", " WARN ");
    MT_EXPECT_TRUE(body.ok());
    if (!body.ok()) return;
    // Rust maps `warn` onto `Warning`; the level is case-insensitive and
    // trimmed, because it comes from a config file.
    MT_EXPECT_EQ(Field(body.value(), "level").as_string(), std::string("Warning"));
    MT_EXPECT_EQ(Field(body.value(), "log_path").as_string(), std::string("/var/log/fc.log"));
    MT_EXPECT_TRUE(Field(body.value(), "show_level").as_bool());
    MT_EXPECT_TRUE(Field(body.value(), "show_log_origin").as_bool());
    MT_EXPECT_TRUE(!Has(body.value(), "module"));
}

MT_TEST(logger_body_rejects_an_unknown_level) {
    // Firecracker accepts the logger only once per VM, so a bad level must be
    // caught before the request rather than burning that one chance.
    core::Expected<core::Json, std::string> body = LoggerBody("/l", "verbose");
    MT_EXPECT_TRUE(!body.ok());
    MT_EXPECT_TRUE(body.error().find("invalid firecracker log level") != std::string::npos);
    MT_EXPECT_TRUE(ParseLogLevel("Error").ok());
    MT_EXPECT_EQ(ParseLogLevel("trace").value(), std::string("Trace"));
}

// ============================================================
// drives
// ============================================================

MT_TEST(drive_body_carries_engine_direct_and_optional_limiter) {
    const core::Json bare =
        DriveBody("rootfs", "/dev/ublkb0", true, false, true, IoEngine::Async, core::nullopt);
    MT_EXPECT_EQ(Field(bare, "drive_id").as_string(), std::string("rootfs"));
    MT_EXPECT_TRUE(Field(bare, "is_root_device").as_bool());
    MT_EXPECT_TRUE(!Field(bare, "is_read_only").as_bool());
    MT_EXPECT_TRUE(Field(bare, "direct").as_bool());
    MT_EXPECT_EQ(Field(bare, "io_engine").as_string(), std::string("Async"));
    // No limiter at all, rather than an empty object: the drive is added
    // unthrottled when limiting is off.
    MT_EXPECT_TRUE(!Has(bare, "rate_limiter"));
    MT_EXPECT_EQ(std::string(IoEngineToString(IoEngine::Sync)), std::string("Sync"));

    const core::Json limited =
        DriveBody("rootfs", "/dev/ublkb0", true, false, true, IoEngine::Sync,
                  core::Optional<RateLimiter>(
                      Limiter(core::Optional<TokenBucket>(Bucket(1000, 5000)), core::nullopt)));
    const core::Json& limiter = Field(limited, "rate_limiter");
    MT_EXPECT_EQ(Field(Field(limiter, "bandwidth"), "size").as_int(),
                 static_cast<int64_t>(5000));
    MT_EXPECT_EQ(Field(Field(limiter, "bandwidth"), "refill_time").as_int(),
                 static_cast<int64_t>(1000));
    // An unconfigured dimension is left out.
    MT_EXPECT_TRUE(!Has(limiter, "ops"));
}

MT_TEST(token_bucket_distinguishes_absent_burst_from_zero) {
    // Firecracker only grants a one-time burst when the field is present, and
    // can reject a zero-token burst outright, so `0` and absent are different
    // requests.
    TokenBucket no_burst = Bucket(1000, 5000);
    core::Json body = DriveBody("d", "/p", false, false, false, IoEngine::Sync,
                               core::Optional<RateLimiter>(Limiter(
                                   core::Optional<TokenBucket>(no_burst), core::nullopt)));
    MT_EXPECT_TRUE(!Has(Field(Field(body, "rate_limiter"), "bandwidth"), "one_time_burst"));

    TokenBucket zero_burst = Bucket(1000, 5000);
    zero_burst.one_time_burst = core::Optional<int64_t>(0);
    body = DriveBody("d", "/p", false, false, false, IoEngine::Sync,
                     core::Optional<RateLimiter>(
                         Limiter(core::Optional<TokenBucket>(zero_burst), core::nullopt)));
    MT_EXPECT_TRUE(Has(Field(Field(body, "rate_limiter"), "bandwidth"), "one_time_burst"));
    MT_EXPECT_EQ(Field(Field(Field(body, "rate_limiter"), "bandwidth"), "one_time_burst").as_int(),
                 static_cast<int64_t>(0));
}

MT_TEST(partial_drive_body_sends_only_what_changes) {
    // `PATCH /drives` reads an absent field as "leave unchanged". Repointing a
    // path must therefore not also clear the limiter, and patching a limiter
    // must not move the backing file.
    const core::Json path_only = PartialDriveBody(
        "rootfs", core::Optional<std::string>("/dev/ublkb1"), core::nullopt);
    MT_EXPECT_EQ(Field(path_only, "drive_id").as_string(), std::string("rootfs"));
    MT_EXPECT_EQ(Field(path_only, "path_on_host").as_string(), std::string("/dev/ublkb1"));
    MT_EXPECT_TRUE(!Has(path_only, "rate_limiter"));

    const core::Json limiter_only = PartialDriveBody(
        "rootfs", core::nullopt,
        core::Optional<RateLimiter>(Limiter(core::Optional<TokenBucket>(Bucket(1000, 1)),
                                            core::Optional<TokenBucket>(Bucket(1000, 2)))));
    MT_EXPECT_TRUE(!Has(limiter_only, "path_on_host"));
    MT_EXPECT_TRUE(Has(limiter_only, "rate_limiter"));
    // Both dimensions present is how an inherited limit gets cleared on resume.
    MT_EXPECT_TRUE(Has(Field(limiter_only, "rate_limiter"), "bandwidth"));
    MT_EXPECT_TRUE(Has(Field(limiter_only, "rate_limiter"), "ops"));
}

// ============================================================
// network / mmds / balloon
// ============================================================

MT_TEST(network_interface_body_omits_absent_mac_and_limiters) {
    const core::Json bare =
        NetworkInterfaceBody("eth0", core::nullopt, "tap0", core::nullopt, core::nullopt);
    MT_EXPECT_EQ(Field(bare, "iface_id").as_string(), std::string("eth0"));
    MT_EXPECT_EQ(Field(bare, "host_dev_name").as_string(), std::string("tap0"));
    MT_EXPECT_TRUE(!Has(bare, "guest_mac"));
    MT_EXPECT_TRUE(!Has(bare, "rx_rate_limiter"));
    MT_EXPECT_TRUE(!Has(bare, "tx_rate_limiter"));

    const core::Json full = NetworkInterfaceBody(
        "eth0", core::Optional<std::string>("AA:BB:CC:DD:EE:FF"), "tap0",
        core::Optional<RateLimiter>(
            Limiter(core::Optional<TokenBucket>(Bucket(1000, 10)), core::nullopt)),
        core::Optional<RateLimiter>(
            Limiter(core::nullopt, core::Optional<TokenBucket>(Bucket(1000, 20)))));
    MT_EXPECT_EQ(Field(full, "guest_mac").as_string(), std::string("AA:BB:CC:DD:EE:FF"));
    MT_EXPECT_TRUE(Has(Field(full, "rx_rate_limiter"), "bandwidth"));
    MT_EXPECT_TRUE(Has(Field(full, "tx_rate_limiter"), "ops"));
}

MT_TEST(mmds_config_body_pins_version_v2) {
    const core::Json body = MmdsConfigBody("eth0");
    // Firecracker's default is V1, which serves metadata without the session
    // token handshake. V2 is therefore explicit, not decorative.
    MT_EXPECT_EQ(Field(body, "version").as_string(), std::string("V2"));
    MT_EXPECT_TRUE(Field(body, "network_interfaces").kind() == core::Json::Kind::Array);
    MT_EXPECT_EQ(Field(body, "network_interfaces").as_array().size(),
                 static_cast<std::size_t>(1));
    MT_EXPECT_EQ(Field(body, "network_interfaces").as_array()[0].as_string(),
                 std::string("eth0"));
    MT_EXPECT_TRUE(!Has(body, "ipv4_address"));
}

MT_TEST(balloon_body_enables_free_page_reporting_without_inflating) {
    const core::Json body = BalloonBody();
    // The purpose is free page reporting; inflation stays at zero.
    MT_EXPECT_EQ(Field(body, "amount_mib").as_int(), static_cast<int64_t>(0));
    MT_EXPECT_TRUE(Field(body, "free_page_reporting").as_bool());
    MT_EXPECT_TRUE(Field(body, "deflate_on_oom").as_bool());
    MT_EXPECT_TRUE(!Has(body, "stats_polling_interval_s"));
    MT_EXPECT_TRUE(!Has(body, "free_page_hinting"));
}

// ============================================================
// snapshot create / load
// ============================================================

MT_TEST(snapshot_create_body_is_diff_with_no_memory_file) {
    const core::Json body = SnapshotCreateBody("/snap/vm_state.bin");
    MT_EXPECT_EQ(Field(body, "snapshot_path").as_string(), std::string("/snap/vm_state.bin"));
    MT_EXPECT_EQ(Field(body, "snapshot_type").as_string(), std::string("Diff"));
    // AgentENV writes guest memory itself from the dirty ranges; asking
    // Firecracker for a memory file too would duplicate the whole image.
    MT_EXPECT_TRUE(!Has(body, "mem_file_path"));
}

MT_TEST(snapshot_load_body_file_backend_sets_track_dirty_pages) {
    const core::Json body =
        SnapshotLoadBody("/snap/vm_state.bin", MemoryBackendType::File, "/dev/ublkb1",
                         /*resume_vm=*/true, /*track_dirty_pages=*/true,
                         /*track_dirty_pages_set=*/true, Overrides());
    MT_EXPECT_EQ(Field(body, "snapshot_path").as_string(), std::string("/snap/vm_state.bin"));
    MT_EXPECT_EQ(Field(Field(body, "mem_backend"), "backend_type").as_string(),
                 std::string("File"));
    MT_EXPECT_EQ(Field(Field(body, "mem_backend"), "backend_path").as_string(),
                 std::string("/dev/ublkb1"));
    MT_EXPECT_TRUE(Field(body, "resume_vm").as_bool());
    MT_EXPECT_TRUE(Field(body, "track_dirty_pages").as_bool());
    MT_EXPECT_EQ(Field(body, "network_overrides").as_array().size(),
                 static_cast<std::size_t>(1));
    MT_EXPECT_EQ(Field(Field(body, "network_overrides").as_array()[0], "iface_id").as_string(),
                 std::string("eth0"));
    MT_EXPECT_EQ(
        Field(Field(body, "network_overrides").as_array()[0], "host_dev_name").as_string(),
        std::string("tap5"));
}

MT_TEST(snapshot_load_body_uffd_leaves_track_dirty_pages_absent) {
    // Rust's uffd path never sets the field, and absent means "Firecracker's
    // default" rather than false — sending false would be a different request.
    const core::Json body = SnapshotLoadBody(
        "/snap/vm_state.bin", MemoryBackendType::Uffd, "/run/uffd.sock",
        /*resume_vm=*/true, /*track_dirty_pages=*/false, /*track_dirty_pages_set=*/false,
        std::vector<std::pair<std::string, std::string> >());
    MT_EXPECT_TRUE(!Has(body, "track_dirty_pages"));
    MT_EXPECT_EQ(Field(Field(body, "mem_backend"), "backend_type").as_string(),
                 std::string("Uffd"));
    MT_EXPECT_EQ(std::string(MemoryBackendTypeToString(MemoryBackendType::File)),
                 std::string("File"));
    // An empty override list is omitted, not sent as `[]`.
    MT_EXPECT_TRUE(!Has(body, "network_overrides"));
}

// ============================================================
// GET /vm/dirty-memory-ranges
// ============================================================

MT_TEST(parse_dirty_memory_ranges_reads_the_api_reply) {
    core::Expected<core::Json, core::AnyError> json = core::Json::Parse(
        "{\"page_size\":4096,\"memory_size\":2147483648,\"ranges\":["
        "{\"base_host_virt_addr\":140737488355328,\"image_offset\":0,\"length\":8192},"
        "{\"base_host_virt_addr\":140737488363520,\"image_offset\":8192,\"length\":4096}]}");
    MT_EXPECT_TRUE(json.ok());
    if (!json.ok()) return;
    core::Expected<DirtyMemoryRanges, std::string> parsed =
        ParseDirtyMemoryRanges(json.value());
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;
    MT_EXPECT_EQ(parsed.value().page_size, 4096);
    MT_EXPECT_EQ(parsed.value().memory_size, static_cast<int64_t>(2147483648LL));
    MT_EXPECT_EQ(parsed.value().ranges.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(parsed.value().ranges[1].image_offset, static_cast<int64_t>(8192));
    MT_EXPECT_EQ(parsed.value().ranges[1].length, static_cast<int64_t>(4096));
    MT_EXPECT_EQ(parsed.value().ranges[0].base_host_virt_addr,
                 static_cast<int64_t>(140737488355328LL));
}

MT_TEST(parse_dirty_memory_ranges_requires_every_field) {
    // This reply decides which bytes of guest memory are captured. A missing
    // field read as 0 would silently produce a short memory image, so each one
    // must fail instead.
    const char* const kBad[6] = {
        "{\"memory_size\":1,\"ranges\":[]}",                  // no page_size
        "{\"page_size\":4096,\"ranges\":[]}",                 // no memory_size
        "{\"page_size\":4096,\"memory_size\":1}",             // no ranges
        "{\"page_size\":4096,\"memory_size\":1,\"ranges\":[{\"image_offset\":0,\"length\":1}]}",
        "{\"page_size\":-1,\"memory_size\":1,\"ranges\":[]}",  // negative
        "{\"page_size\":\"4096\",\"memory_size\":1,\"ranges\":[]}",  // wrong type
    };
    for (int i = 0; i < 6; ++i) {
        core::Expected<core::Json, core::AnyError> json = core::Json::Parse(kBad[i]);
        MT_EXPECT_TRUE(json.ok());
        if (!json.ok()) continue;
        MT_EXPECT_TRUE(!ParseDirtyMemoryRanges(json.value()).ok());
    }
    core::Expected<core::Json, core::AnyError> array = core::Json::Parse("[]");
    MT_EXPECT_TRUE(!ParseDirtyMemoryRanges(array.value()).ok());
}

MT_MAIN
