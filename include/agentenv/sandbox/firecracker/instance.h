// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{instance,process_vm_reader,sandbox}.rs.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_INSTANCE_H_
#define AGENTENV_SANDBOX_FIRECRACKER_INSTANCE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/sandbox/firecracker/manifest.h"
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
    virtual core::Expected<core::Unit, std::string> SetMmds(const MmdsData& d) = 0;
};

/// Rust: firecracker/instance.rs :: MMDS_SIZE_LIMIT (1 MiB).
static const size_t kMmdsSizeLimit = 1048576;

/// Rust: firecracker/instance.rs :: parse_log_level — case-insensitive.
core::Expected<std::string, std::string> ParseLogLevel(const std::string& level);

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

    /// Rust `spawn_with_netns` (netns omitted in this port). Spawns the given
    /// binary with the firecracker api-sock args; rejects a duplicate start.
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
    core::Expected<core::Unit, std::string>
      SetMachineConfig(uint32_t mem_mib, uint32_t vcpus, bool smt, bool track_dirty);
    core::Expected<core::Unit, std::string>
        SetBootSource(const std::string& kernel_image, const std::string& boot_args);
    core::Expected<core::Unit, std::string>
        AddDrive(const std::string& drive_id, const std::string& path_on_host,
      bool is_root, bool read_only);
    core::Expected<core::Unit, std::string> Start();
    core::Expected<core::Unit, std::string> Pause();
    core::Expected<core::Unit, std::string> Resume();

    // ---- Instance ABC ----
    core::Expected<core::Unit, std::string> Boot() override;
    core::Expected<core::Unit, std::string> Shutdown() override;
    core::Expected<core::Unit, std::string> SetMmds(const MmdsData& d) override;

 private:
    core::Expected<core::Unit, std::string> ApiPut(const std::string& path,
         const std::string& json_body);
    std::string StderrSummary() const;

    std::string work_dir_;
    std::string socket_path_;
    std::string stderr_path_;
    std::shared_ptr<Connector> connector_;
    int64_t     child_pid_;   // -1 when no child
};

/// Rust: firecracker/process_vm_reader.rs :: ProcessVmReader (process_vm_readv).
class ProcessVmReader {
 public:
    virtual ~ProcessVmReader() {}
    virtual core::Expected<std::vector<uint8_t>, std::string>
        Read(int64_t pid, uint64_t addr, size_t len) = 0;
};

/// A real process_vm_readv(2)-backed reader (works on any pid we may ptrace/read;
/// used with the firecracker pid to sample guest-visible memory ranges).
std::unique_ptr<ProcessVmReader> MakeProcessVmReader();

/// Rust: firecracker/sandbox.rs :: FirecrackerCapturedSnapshot.
struct CapturedSnapshot {
    SnapshotManifest manifest;
    std::string      snapshot_dir;
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
