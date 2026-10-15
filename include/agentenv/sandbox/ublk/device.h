// SPDX-License-Identifier: MIT
// Rust: src/sandbox/ublk/device.rs — UblkDeviceManager and
//   SharedReadOnlyDevice / SharedReadOnlyDeviceInner.
//
// Device lifecycle over the daemon client. The part that carries real
// consequences is the shared read-only pool: the tools drive and a memory
// snapshot are opened by *many* sandboxes at once, so the device must be
// created once, reference-counted, and deleted only when the last holder goes
// away. Getting that wrong either deletes a device out from under a running
// guest or leaks one per launch.
#ifndef AGENTENV_SANDBOX_UBLK_DEVICE_H_
#define AGENTENV_SANDBOX_UBLK_DEVICE_H_

#include <map>
#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/ublk_daemon/client.h"

namespace agentenv {
namespace sandbox {
namespace ublk {

namespace daemon = storage::ublk_daemon;

/// Rust `UblkDevice`.
struct Device {
    uint32_t dev_id = 0;
    std::string device_path;
};

/// Rust `UblkCreateSpec::Overlaybd`.
struct CreateSpec {
    std::string image_config;
    std::string global_config;
};

class DeviceManager;

/// Rust `SharedReadOnlyDeviceInner`.
///
/// Held through a `shared_ptr`; the device is deleted when the last handle
/// drops. Not copyable on its own — handles are the copyable thing.
class SharedReadOnlyDeviceInner {
 public:
    SharedReadOnlyDeviceInner(DeviceManager* owner, Device device, std::string key);
    ~SharedReadOnlyDeviceInner();

    SharedReadOnlyDeviceInner(const SharedReadOnlyDeviceInner&) = delete;
    SharedReadOnlyDeviceInner& operator=(const SharedReadOnlyDeviceInner&) = delete;

    const Device& device() const { return device_; }
    const std::string& image_config_key() const { return key_; }
    bool released() const { return released_; }

 private:
    friend class DeviceManager;

    DeviceManager* owner_;
    Device device_;
    std::string key_;
    bool released_;
    /// Linux drops a block device's page cache when its last opener closes
    /// (`blkdev_put_whole` -> `kill_bdev`), and the daemon's `/dev/ublkcN`
    /// handle does not keep `/dev/ublkbN` open. Holding an fd between launches
    /// is what makes the tools drive warm for the next sandbox; it does not
    /// prevent ordinary reclaim under memory pressure.
    int cache_fd_;
};

/// Rust `SharedReadOnlyDevice` — a reference-counted handle.
///
/// Copying increments the count. The device is deleted only when the last copy
/// is gone, which is why this is a handle type and not the device itself.
class SharedReadOnlyDevice {
 public:
    SharedReadOnlyDevice() {}
    explicit SharedReadOnlyDevice(std::shared_ptr<SharedReadOnlyDeviceInner> inner)
        : inner_(std::move(inner)) {}

    bool valid() const { return static_cast<bool>(inner_); }
    const std::string& image_config_path() const { return inner_->image_config_key(); }
    const std::string& device_path() const { return inner_->device().device_path; }
    uint32_t dev_id() const { return inner_->device().dev_id; }

 private:
    std::shared_ptr<SharedReadOnlyDeviceInner> inner_;
};

/// Rust `OverlaybdRuntimeDevice`.
struct OverlaybdRuntimeDevice {
    Device device;
    std::string image_config_path;
    uint64_t actual_virtual_size = 0;
};

/// Rust `UblkDeviceManager`.
///
/// Not a global singleton here: Rust reaches it through `global()`, but a
/// process-wide handle would make the shared-device table impossible to
/// exercise. The ownership is the same — one manager per process in practice.
class DeviceManager {
 public:
    /// `client` may be null, in which case every operation reports that the
    /// daemon is unavailable rather than pretending to succeed.
    ///
    /// `pool_enabled` mirrors Rust's `pool_config.is_some()`. It changes two
    /// things: whether a shared device is *acquired* from the warm pool or
    /// created raw, and whether releasing returns it to the pool or deletes
    /// it. The two must agree, or a pooled device would be deleted and a raw
    /// one returned to a pool that never held it.
    DeviceManager(std::shared_ptr<daemon::DaemonClient> client, bool pool_enabled);
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    bool is_available() const { return static_cast<bool>(client_); }

    /// Rust `create_overlaybd_runtime_device` — the rootfs / extra-drive path.
    /// The daemon owns runtime materialization, so this never creates files.
    daemon::DaemonResult<OverlaybdRuntimeDevice> CreateOverlaybdRuntimeDevice(
        const std::string& source_image_config, const std::string& global_config,
        const std::string& runtime_dir, bool read_only,
        storage::overlaybd::UpperMode runtime_upper_mode,
        const core::Optional<uint64_t>& requested_virtual_size,
        const core::Optional<uint64_t>& known_source_virtual_size, bool allow_shrink);

    /// Rust `get_or_create_shared_mem`.
    daemon::DaemonResult<SharedReadOnlyDevice> GetOrCreateSharedMem(const CreateSpec& spec,
                                                                     uint64_t virtual_size);

    /// Rust `get_or_create_shared_tools`.
    ///
    /// Also retains the result in `recent_tools_device_`, so one idle tools
    /// image stays alive between launches. Without that the device — and its
    /// page cache — would be destroyed every time the last sandbox exits, and
    /// the next launch would re-read the whole image.
    daemon::DaemonResult<SharedReadOnlyDevice> GetOrCreateSharedTools(const CreateSpec& spec);

    /// Rust `release_device` — returns to the warm pool when pooling is on,
    /// deletes otherwise.
    daemon::DaemonResult<core::Unit> ReleaseDevice(const Device& device);

    /// Rust `delete_device`. Also the release path for dedicated memory
    /// devices, which must never be returned to the warm pool.
    daemon::DaemonResult<core::Unit> DeleteDevice(const Device& device);

    /// Rust `notify_sandbox_ready` — best-effort, as upstream: a failure only
    /// means held background downloads start after their fallback timeout.
    void NotifySandboxReady(const std::string& device_key);

    /// Rust `restack_snapshot_device`.
    daemon::DaemonResult<daemon::RestackSnapshotStats> RestackSnapshotDevice(
        const Device& device, const std::string& output_layer_path);

    /// Number of live shared entries. Diagnostics only.
    std::size_t shared_entry_count() const { return shared_devices_.size(); }

 private:
    friend class SharedReadOnlyDeviceInner;

    daemon::DaemonResult<SharedReadOnlyDevice> GetOrCreateSharedReadOnly(
        const CreateSpec& spec, const core::Optional<uint64_t>& virtual_size);

    /// Rust `release_shared_readonly_device` — drops the table entry, then
    /// releases the device.
    void OnSharedDeviceReleased(const std::string& key, const Device& device);

    std::shared_ptr<daemon::DaemonClient> client_;
    bool pool_enabled_;

    /// Rust `shared_readonly_devices: DashMap<PathBuf, Weak<..>>`.
    ///
    /// Weak, not strong: the table must not be what keeps a device alive, or
    /// nothing would ever be deleted.
    std::map<std::string, std::weak_ptr<SharedReadOnlyDeviceInner> > shared_devices_;

    /// Rust `recent_tools_device` — one retained idle tools device.
    SharedReadOnlyDevice recent_tools_device_;
};

}  // namespace ublk
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_UBLK_DEVICE_H_
