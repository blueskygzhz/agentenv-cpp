// SPDX-License-Identifier: MIT
// Rust: src/sandbox/ublk/device.rs
#include "agentenv/sandbox/ublk/device.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

namespace agentenv {
namespace sandbox {
namespace ublk {

namespace {

/// Rust `std::fs::canonicalize(image_config).unwrap_or_else(|_| clone())`.
///
/// The key must be canonical so two launches naming the same image by
/// different paths share one device. Falling back to the raw path on failure
/// is deliberate: the image may not exist yet, and refusing here would break a
/// launch over a lookup detail.
std::string CanonicalKey(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) != NULL) return std::string(buf);
    return path;
}

}  // namespace

// ── SharedReadOnlyDeviceInner ──────────────────────────────────────────────

SharedReadOnlyDeviceInner::SharedReadOnlyDeviceInner(DeviceManager* owner, Device device,
                                                     std::string key)
    : owner_(owner), device_(std::move(device)), key_(std::move(key)), released_(false),
      cache_fd_(-1) {
    // Hold the block device open so its page cache survives between sandbox
    // launches. Best-effort: failing to open is not a reason to refuse the
    // device, it only means the next launch re-reads the image.
    cache_fd_ = ::open(device_.device_path.c_str(), O_RDONLY | O_CLOEXEC);
}

SharedReadOnlyDeviceInner::~SharedReadOnlyDeviceInner() {
    // Rust's `Drop` guards with `released.swap(true)`. Spelled the same way
    // here rather than as a load-then-store: the flag's reader is a lookup on
    // another thread holding a locked `weak_ptr`, so the publish must be a
    // single atomic step. A destructor cannot run twice, but the ordering
    // guarantee is what the reader depends on.
    if (released_.exchange(true)) return;

    // Close before deleting: the cache fd is an opener of the very device the
    // daemon is about to tear down.
    if (cache_fd_ >= 0) {
        ::close(cache_fd_);
        cache_fd_ = -1;
    }
    if (owner_ != NULL) owner_->OnSharedDeviceReleased(key_, device_);
}

// ── DeviceManager ──────────────────────────────────────────────────────────

DeviceManager::DeviceManager(std::shared_ptr<daemon::DaemonClient> client, bool pool_enabled)
    : client_(std::move(client)), pool_enabled_(pool_enabled) {}

DeviceManager::~DeviceManager() {
    // Drop the retained tools device first. Its destructor calls back into
    // `OnSharedDeviceReleased`, which touches `shared_devices_`, so it must run
    // while this object is still whole.
    //
    // Declared before the locked scope so it is destroyed *after* the guard:
    // the callback takes `mutex_`, and dropping the handle under the lock
    // would deadlock.
    SharedReadOnlyDevice retained;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        retained = recent_tools_device_;
        recent_tools_device_ = SharedReadOnlyDevice();
    }
}

daemon::DaemonResult<OverlaybdRuntimeDevice> DeviceManager::CreateOverlaybdRuntimeDevice(
    const std::string& source_image_config, const std::string& global_config,
    const std::string& runtime_dir, bool read_only,
    storage::overlaybd::UpperMode runtime_upper_mode,
    const core::Optional<uint64_t>& requested_virtual_size,
    const core::Optional<uint64_t>& known_source_virtual_size, bool allow_shrink) {
    if (!client_) {
        return core::make_unexpected(
            daemon::DaemonError::Recoverable("ublk daemon is not available"));
    }
    const daemon::DaemonResult<daemon::CreatedRuntimeDevice> created =
        client_->CreateOverlaybdRuntimeDevice(source_image_config, global_config, runtime_dir,
                                              read_only, runtime_upper_mode,
                                              requested_virtual_size,
                                              known_source_virtual_size, allow_shrink);
    if (!created.ok()) return core::make_unexpected(created.error());

    OverlaybdRuntimeDevice out;
    out.device.dev_id = created.value().dev_id;
    out.device.device_path = created.value().device_path;
    out.image_config_path = created.value().runtime_image_config_path;
    out.actual_virtual_size = created.value().actual_virtual_size;
    return out;
}

daemon::DaemonResult<SharedReadOnlyDevice> DeviceManager::GetOrCreateSharedMem(
    const CreateSpec& spec, uint64_t virtual_size) {
    return GetOrCreateSharedReadOnly(spec, core::Optional<uint64_t>(virtual_size));
}

daemon::DaemonResult<SharedReadOnlyDevice> DeviceManager::GetOrCreateSharedTools(
    const CreateSpec& spec) {
    // Tools capacity comes from the published block image, so no size is
    // requested -- Rust passes `None` here, which is not the same as passing 0.
    const daemon::DaemonResult<SharedReadOnlyDevice> device =
        GetOrCreateSharedReadOnly(spec, core::nullopt);
    if (!device.ok()) return device;
    // Retain one idle tools image. Without this the device, and its warm page
    // cache, would be destroyed whenever the last sandbox exits, and the next
    // launch would re-read the whole image. Active sandboxes retain any other
    // version through their own handles.
    //
    // The outgoing handle is copied out before the swap and dropped after the
    // guard: replacing a retained device of an *older* tools version is what
    // deletes it, and that deletion goes through `OnSharedDeviceReleased`.
    SharedReadOnlyDevice previous;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        previous = recent_tools_device_;
        recent_tools_device_ = device.value();
    }
    return device;
}

daemon::DaemonResult<SharedReadOnlyDevice> DeviceManager::GetOrCreateSharedReadOnly(
    const CreateSpec& spec, const core::Optional<uint64_t>& virtual_size) {
    if (!client_) {
        return core::make_unexpected(
            daemon::DaemonError::Recoverable("ublk daemon is not available"));
    }
    const std::string key = CanonicalKey(spec.image_config);

    // Fast path: an existing entry that is still alive and not mid-release.
    //
    // The table holds `weak_ptr`, so an entry can outlive its device; locking
    // is what distinguishes "shared device exists" from "an entry for it was
    // left behind". The `released()` check covers the narrower window where the
    // object exists but its teardown has begun.
    {
        std::lock_guard<std::mutex> guard(mutex_);
        std::map<std::string, std::weak_ptr<SharedReadOnlyDeviceInner> >::iterator it =
            shared_devices_.find(key);
        if (it != shared_devices_.end()) {
            std::shared_ptr<SharedReadOnlyDeviceInner> strong = it->second.lock();
            if (strong && !strong->released()) {
                // Returning a *copy* of the handle, so nothing is destroyed
                // under the lock: the refcount only goes up here.
                return SharedReadOnlyDevice(strong);
            }
        }
    }

    // Acquire from the warm pool, or create a raw device when pooling is off.
    // The two must stay paired with `ReleaseDevice`: a pooled device that gets
    // deleted is lost to the pool, and a raw one returned to the pool was
    // never in it.
    Device device;
    if (pool_enabled_) {
        const daemon::DaemonResult<daemon::CreatedDevice> acquired =
            client_->AcquireOverlaybd(spec.image_config, spec.global_config, virtual_size,
                                      daemon::AccessMode::Shared);
        if (!acquired.ok()) return core::make_unexpected(acquired.error());
        device.dev_id = acquired.value().dev_id;
        device.device_path = acquired.value().device_path;
    } else {
        const daemon::DaemonResult<daemon::CreatedDevice> created =
            client_->CreateOverlaybd(spec.image_config, spec.global_config);
        if (!created.ok()) return core::make_unexpected(created.error());
        device.dev_id = created.value().dev_id;
        device.device_path = created.value().device_path;
    }

    // Declared outside the locked scope below so that, if this caller loses
    // the race, `inner`'s destructor -- which calls `OnSharedDeviceReleased`
    // and takes `mutex_` -- runs after the guard is gone.
    std::shared_ptr<SharedReadOnlyDeviceInner> inner(
        new SharedReadOnlyDeviceInner(this, device, key));
    // Declared after `inner` and before the locked scope, so the two possible
    // results are both destroyed after the guard and never under it.
    SharedReadOnlyDevice reused;

    // Re-check under the same key. A concurrent caller may have inserted a
    // live device while this one was acquiring; reusing theirs and dropping
    // ours is what keeps one device per key. Dropping `inner` runs its
    // destructor, which releases the redundant device -- so the loser cleans
    // up after itself rather than leaking.
    {
        std::lock_guard<std::mutex> guard(mutex_);
        std::map<std::string, std::weak_ptr<SharedReadOnlyDeviceInner> >::iterator it =
            shared_devices_.find(key);
        if (it != shared_devices_.end()) {
            std::shared_ptr<SharedReadOnlyDeviceInner> winner = it->second.lock();
            if (winner && !winner->released() && winner != inner) {
                reused = SharedReadOnlyDevice(winner);
            }
        }
        if (!reused.valid()) {
            // Either there was no entry, or the one there was stale.
            shared_devices_[key] = std::weak_ptr<SharedReadOnlyDeviceInner>(inner);
        }
    }

    // `inner` is dropped on the way out of this function when the race was
    // lost, which is the redundant device's release -- and by now the lock is
    // free, so `OnSharedDeviceReleased` can take it.
    if (reused.valid()) return reused;
    return SharedReadOnlyDevice(inner);
}

void DeviceManager::OnSharedDeviceReleased(const std::string& key, const Device& device) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        // Remove the entry only if it is still *ours*. A fresh device may
        // already have taken this key, and removing that one would make the
        // next lookup create a second device for an image that already has
        // one.
        std::map<std::string, std::weak_ptr<SharedReadOnlyDeviceInner> >::iterator it =
            shared_devices_.find(key);
        if (it != shared_devices_.end()) {
            std::shared_ptr<SharedReadOnlyDeviceInner> current = it->second.lock();
            if (!current || current->released()) {
                shared_devices_.erase(it);
            }
        }
    }
    // Outside the lock: this is a round trip to the daemon, and holding
    // `mutex_` across it would block every sandbox launch for the duration of
    // one teardown.
    //
    // Best-effort, as Rust logs and continues: the handle is already gone, so
    // there is nobody left to report a failure to.
    (void)ReleaseDevice(device);
}

daemon::DaemonResult<core::Unit> DeviceManager::ReleaseDevice(const Device& device) {
    if (!client_) {
        return core::make_unexpected(
            daemon::DaemonError::Recoverable("ublk daemon is not available"));
    }
    // With pooling off there is no pool to return to, so release *is* delete.
    if (!pool_enabled_) return DeleteDevice(device);
    return client_->ReleaseOverlaybd(device.dev_id);
}

daemon::DaemonResult<core::Unit> DeviceManager::DeleteDevice(const Device& device) {
    if (!client_) {
        return core::make_unexpected(
            daemon::DaemonError::Recoverable("ublk daemon is not available"));
    }
    return client_->Delete(device.dev_id);
}

void DeviceManager::NotifySandboxReady(const std::string& device_key) {
    if (!client_) return;
    // Best-effort by design: a failed notification only means the daemon's
    // held background downloads start after their fallback timeout instead of
    // immediately, which is a slower boot rather than a broken one.
    (void)client_->NotifySandboxReady(device_key);
}

daemon::DaemonResult<daemon::RestackSnapshotStats> DeviceManager::RestackSnapshotDevice(
    const Device& device, const std::string& output_layer_path) {
    if (!client_) {
        return core::make_unexpected(
            daemon::DaemonError::Recoverable("ublk daemon is not available"));
    }
    // The terminal/recoverable distinction is the client's; it is passed
    // through untouched because only an explicit daemon `Error` proves the
    // live runtime was left intact.
    return client_->RestackSnapshot(device.dev_id, output_layer_path);
}

}  // namespace ublk
}  // namespace sandbox
}  // namespace agentenv
