// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{instance,process_vm_reader,sandbox}.rs.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_INSTANCE_H_
#define AGENTENV_SANDBOX_FIRECRACKER_INSTANCE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/identity.h"
#include "agentenv/sandbox/manifest.h"
#include "agentenv/sandbox/firecracker/dirty_memory.h"
#include "agentenv/sandbox/firecracker/mmds.h"
#include "agentenv/sandbox/firecracker/sandbox.h"
#include "agentenv/sandbox/firecracker/socket.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Abstract handle so the warm pool can hold heterogeneous instances.
/// Rust: the pool stores concrete FirecrackerInstance values; the ABC here keeps
/// pool.h decoupled from the (larger) concrete type.
class Instance {
 public:
    virtual ~Instance() {}
    virtual core::Expected<core::Unit, std::string> Boot() = 0;
    virtual core::Expected<core::Unit, std::string> Shutdown() = 0;
    /// Rust `FirecrackerInstance::set_mmds(&MmdsMetadata)` — takes the document
    /// itself, not pre-serialized bytes, so the size check below is applied to
    /// what actually goes on the wire.
    virtual core::Expected<core::Unit, std::string> SetMmds(const MmdsMetadata& metadata) = 0;
};

/// Rust: firecracker/instance.rs :: MMDS_SIZE_LIMIT (1 MiB).
static const size_t kMmdsSizeLimit = 1048576;

/// Rust: firecracker/instance.rs :: HTTP_API_MAX_PAYLOAD_SIZE.
///
/// Defined as equal to the MMDS limit upstream, and passed on the command line
/// because Firecracker's own default is far smaller. A snapshot load body
/// carries the memory backend path and every network override, so the default
/// is reachable in normal operation -- and the request is rejected by the VMM
/// before any of this code sees it.
static const size_t kHttpApiMaxPayloadSize = kMmdsSizeLimit;

/// Rust: firecracker/instance.rs :: parse_log_level — case-insensitive.
core::Expected<std::string, std::string> ParseLogLevel(const std::string& level);

// ---------------------------------------------------------------------------
// Firecracker API request bodies
// ---------------------------------------------------------------------------
//
// Built as `core::Json` values, never by pasting strings together. Every one of
// these carries host paths, and a path may legally contain a quote or a
// backslash; a concatenated body would then be invalid JSON, or worse, would
// let a crafted path inject a sibling field. Same failure mode as the MMDS
// extras, so the same fix.
//
// Free functions rather than private methods so the wire format can be tested
// without a socket: this format is what Firecracker validates, and a rejection
// mid-boot surfaces far from its cause.

/// Rust `MachineConfiguration`.
core::Json MachineConfigBody(uint32_t mem_size_mib, uint32_t vcpu_count, bool smt,
                             bool track_dirty_pages);

/// Rust `Logger` — `show_level` and `show_log_origin` are always on, as Rust
/// sets them; `module` stays absent.
core::Expected<core::Json, std::string> LoggerBody(const std::string& log_path,
                                                   const std::string& level);

/// Rust `BootSource`. `initrd_path` and `boot_args` are omitted when absent
/// (`skip_serializing_if`), not sent as null.
core::Json BootSourceBody(const std::string& kernel_image_path,
                          const core::Optional<std::string>& initrd_path,
                          const core::Optional<std::string>& boot_args);

/// Rust `Drive` as used by `add_drive` (pre-boot `PUT /drives/{id}`).
///
/// The limiter is attached here rather than PATCHed after start, so throttling
/// is in force from the guest's first I/O.
core::Json DriveBody(const std::string& drive_id, const std::string& path_on_host,
                     bool is_root_device, bool read_only, bool direct, IoEngine io_engine,
                     const core::Optional<RateLimiter>& rate_limiter);

/// Rust `PartialDrive` — only the fields being changed are sent, because
/// `PATCH /drives` reads an absent field as "leave unchanged".
core::Json PartialDriveBody(const std::string& drive_id,
                            const core::Optional<std::string>& path_on_host,
                            const core::Optional<RateLimiter>& rate_limiter);

/// Rust `NetworkInterface`.
core::Json NetworkInterfaceBody(const std::string& iface_id,
                                const core::Optional<std::string>& guest_mac,
                                const std::string& host_dev_name,
                                const core::Optional<RateLimiter>& rx_rate_limiter,
                                const core::Optional<RateLimiter>& tx_rate_limiter);

/// Rust `MmdsConfig` with `version = V2`.
core::Json MmdsConfigBody(const std::string& iface_id);

/// Rust `Balloon` — `amount_mib = 0`, `deflate_on_oom = true`,
/// `free_page_reporting = true`.
///
/// The point is free page reporting: the guest returns unused pages to the
/// host. `amount_mib = 0` means no forced inflation, and `deflate_on_oom` is a
/// safety net rather than the purpose.
core::Json BalloonBody();

/// Rust `SnapshotCreateParams` with `snapshot_type = Diff`.
///
/// State only: AgentENV packages guest memory itself from the dirty ranges, so
/// `mem_file_path` is deliberately absent.
core::Json SnapshotCreateBody(const std::string& snapshot_path);

/// Rust `MemoryBackend::backend_type`.
enum class MemoryBackendType {
    File,
    Uffd,
};
const char* MemoryBackendTypeToString(MemoryBackendType type);

/// Rust `SnapshotLoadParams`.
///
/// `track_dirty_pages` is only written when `track_dirty_pages_set`: Rust's
/// uffd path leaves the field absent while the file path always sets it, and
/// an absent field means "Firecracker's default" rather than false.
core::Json SnapshotLoadBody(
    const std::string& snapshot_path, MemoryBackendType backend_type,
    const std::string& backend_path, bool resume_vm, bool track_dirty_pages,
    bool track_dirty_pages_set,
    const std::vector<std::pair<std::string, std::string> >& network_overrides);

/// Rust: firecracker/instance.rs :: open_log_stdio — create parent dirs and
/// append-open the log file, returning the opened fd (-1 == /dev/null intent).
/// Exposed for testing. Returns fd (caller closes) or an error.
core::Expected<int, std::string> OpenLogStdio(const std::string& path);

/// Rust: firecracker/instance.rs :: read_stderr_tail — last <=4096 bytes.
core::Expected<std::string, std::string> ReadStderrTail(const std::string& path);

/// Rust: firecracker/instance.rs :: FirecrackerInstance.
///
/// Real process + API-socket management. The microVM configuration methods
/// (set_machine_config/set_boot_source/.../start/pause/resume) issue real
/// HTTP/1.1 requests over the API unix socket via the injected Connector; the
/// process lifecycle (spawn/wait_for_ready/stop) and file helpers are host-real
/// and testable exactly as the Rust unit tests exercise them (with /bin/true and
/// socket marker files).
class FirecrackerInstance : public Instance {
 public:
    /// `connector` may be null; API methods then fail with a clear error.
    FirecrackerInstance(std::string work_dir, std::shared_ptr<Connector> connector);
~FirecrackerInstance();

    FirecrackerInstance(const FirecrackerInstance&) = delete;
    FirecrackerInstance& operator=(const FirecrackerInstance&) = delete;

    const std::string& SocketPath() const { return socket_path_; }
    const std::string& WorkDir() const { return work_dir_; }

    /// Rust `pid`.
    core::Expected<int64_t, std::string> Pid() const;

    /// Rust `spawn_with_netns`. Spawns the given binary with the firecracker
    /// api-sock args; rejects a duplicate start.
    ///
    /// `netns_path` is the bind-mounted namespace file from a network `Slot`.
    /// It is opened in the parent and entered with `setns` in the child before
    /// `exec`, which is the only point where that is possible: Firecracker
    /// creates its tap device at configure time, and a VMM that started in the
    /// wrong namespace cannot be moved into another one afterwards.
    ///
    /// Empty `netns_path` keeps the parent's namespace.
    core::Expected<core::Unit, std::string>
        SpawnWithNetns(const std::string& firecracker_binary,
                       const std::string& stdout_path,
                       const std::string& stderr_path,
                       const std::string& netns_path);

    /// `SpawnWithNetns` with no namespace, for callers that do not allocate a
    /// network slot.
    core::Expected<core::Unit, std::string>
  Spawn(const std::string& firecracker_binary,
              const std::string& stdout_path,
          const std::string& stderr_path);

    /// Rust `wait_for_ready` — poll until the socket file exists, the child
    /// exits, or timeout elapses.
    core::Expected<core::Unit, std::string>
        WaitForReady(int64_t timeout_ms, int64_t poll_interval_ms);

    /// Rust `stop` — SIGTERM, wait up to timeout, then SIGKILL; remove socket.
    core::Expected<core::Unit, std::string> Stop(int64_t timeout_ms);

  /// Rust `resolve_host_path`.
    std::string ResolveHostPath(const std::string& path) const;

    // ---- microVM API (real HTTP over the api socket) ----

    /// Rust `set_cpu_config` — `PUT /cpu-config` with the template verbatim.
    ///
    /// The text is parsed first only to reject a malformed template here,
    /// where the error names `cpu_config_json`, instead of letting Firecracker
    /// refuse an unparseable body mid-boot.
    core::Expected<core::Unit, std::string> SetCpuConfig(const std::string& json);

    /// Rust `set_logger` — pre-boot only, and only once per VM.
    core::Expected<core::Unit, std::string> SetLogger(const std::string& log_path,
                                                      const std::string& level);

    core::Expected<core::Unit, std::string>
      SetMachineConfig(uint32_t mem_mib, uint32_t vcpus, bool smt, bool track_dirty);

    /// Rust `set_boot_source`.
    core::Expected<core::Unit, std::string>
        SetBootSource(const std::string& kernel_image,
                      const core::Optional<std::string>& initrd_path,
                      const core::Optional<std::string>& boot_args);

    /// Rust `add_drive`.
    core::Expected<core::Unit, std::string>
        AddDrive(const std::string& drive_id, const std::string& path_on_host, bool is_root,
                 bool read_only, bool direct, IoEngine io_engine,
                 const core::Optional<RateLimiter>& rate_limiter);

    /// Rust `patch_drive_rate_limiter` — on a running VM.
    core::Expected<core::Unit, std::string>
        PatchDriveRateLimiter(const std::string& drive_id, const RateLimiter& rate_limiter);

    /// Rust `patch_drive_path` — repoint a drive on a loaded snapshot.
    core::Expected<core::Unit, std::string>
        PatchDrivePath(const std::string& drive_id, const std::string& path_on_host);

    /// Rust `add_network_interface` — pre-boot only.
    core::Expected<core::Unit, std::string>
        AddNetworkInterface(const std::string& iface_id,
                            const core::Optional<std::string>& guest_mac,
                            const std::string& host_dev_name,
                            const core::Optional<RateLimiter>& rx_rate_limiter,
                            const core::Optional<RateLimiter>& tx_rate_limiter);

    /// Rust `set_mmds_config` — MMDS V2 on one interface, pre-boot only.
    core::Expected<core::Unit, std::string> SetMmdsConfig(const std::string& iface_id);

    /// Rust `set_balloon` — pre-boot only.
    core::Expected<core::Unit, std::string> SetBalloon();

    core::Expected<core::Unit, std::string> Start();
    core::Expected<core::Unit, std::string> Pause();
    core::Expected<core::Unit, std::string> Resume();

    /// Rust `create_state_only_snapshot`.
    core::Expected<core::Unit, std::string>
        CreateStateOnlySnapshot(const std::string& snapshot_path);

    /// Rust `get_dirty_memory_ranges` — `GET /vm/dirty-memory-ranges`.
    core::Expected<DirtyMemoryRanges, std::string> GetDirtyMemoryRanges();

    /// Rust `load_snapshot_uffd` — pre-boot only. Always resumes the VM and
    /// leaves `track_dirty_pages` unset, as Rust does.
    core::Expected<core::Unit, std::string>
        LoadSnapshotUffd(const std::string& snapshot_path, const std::string& uffd_uds_path,
                         const std::vector<std::pair<std::string, std::string> >&
                             network_overrides);

    /// Rust `load_snapshot_file` — pre-boot only.
    core::Expected<core::Unit, std::string>
        LoadSnapshotFile(const std::string& snapshot_path,
                         const std::string& mem_backend_path,
                         const std::vector<std::pair<std::string, std::string> >&
                             network_overrides,
                         bool resume_vm, bool track_dirty_pages);

    // ---- Instance ABC ----
    core::Expected<core::Unit, std::string> Boot() override;
    core::Expected<core::Unit, std::string> Shutdown() override;
    core::Expected<core::Unit, std::string> SetMmds(const MmdsMetadata& metadata) override;

 private:
    core::Expected<core::Unit, std::string> ApiPut(const std::string& path,
         const std::string& json_body);
    /// `PATCH` / `GET` share the connect-then-request path with `ApiPut`.
    core::Expected<std::string, std::string> ApiRequest(const std::string& method,
                                                        const std::string& path,
                                                        const std::string& json_body);
    std::string StderrSummary() const;

    std::string work_dir_;
    std::string socket_path_;
    std::string stderr_path_;
    std::shared_ptr<Connector> connector_;
    int64_t     child_pid_;   // -1 when no child
};

// ProcessVmReader lives in "firecracker/process_vm.h" — the port of
// process_vm_reader.rs. An abstract duplicate used to be declared here with no
// implementor other than its own factory and no caller at all; it collided by
// name with the real one.

/// Rust: firecracker/sandbox.rs :: FirecrackerCapturedSnapshot.
struct CapturedSnapshot {
    /// The real manifest type from `sandbox/manifest.h`. It is backend-agnostic
    /// by design (`backend` names the VMM), so there is no Firecracker-specific
    /// manifest to carry here.
    SandboxSnapshotManifest manifest;
    std::string             snapshot_dir;
};

/// Rust: firecracker/sandbox.rs :: FirecrackerPausedState.
class PausedState {
 public:
    virtual ~PausedState() {}
    virtual core::SandboxId SandboxId() const = 0;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_INSTANCE_H_
