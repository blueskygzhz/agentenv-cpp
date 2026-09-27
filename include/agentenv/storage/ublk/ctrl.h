// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/ctrl.rs — UVMUblkCtrl + control-plane commands.
#ifndef AGENTENV_STORAGE_UBLK_CTRL_H_
#define AGENTENV_STORAGE_UBLK_CTRL_H_

#include <cstdint>
#include <future>
#include <memory>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace ublk {

/// Rust `ublk_available()` — is /dev/ublk-control present?
bool UblkAvailable();
/// Rust `ublk_module_loaded()`.
bool UblkModuleLoaded();
/// Rust `load_ublk_module()`.
core::Expected<core::Unit, std::string> LoadUblkModule();

/// Rust struct `UVMUblkCtrl` — control channel over /dev/ublk-control.
class UblkCtrl {
 public:
    virtual ~UblkCtrl() {}
    virtual std::future<core::Expected<uint64_t, std::string>> GetFeatures() = 0;
    virtual std::future<core::Expected<core::Unit, std::string>> StopDev() = 0;
 virtual std::future<core::Expected<core::Unit, std::string>> DelDev() = 0;
    virtual std::future<core::Expected<core::Unit, std::string>>
        UpdateSize(uint64_t new_bytes) = 0;
    virtual uint32_t DevId() const = 0;
};

/// Rust struct `UVMUblkCtrlBuilder`.
class UblkCtrlBuilder {
 public:
    UblkCtrlBuilder& DevId(uint32_t id) { dev_id_ = id; return *this; }
    core::Expected<std::unique_ptr<UblkCtrl>, std::string> Build();
 private:
    uint32_t dev_id_ = 0;
};

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_CTRL_H_
