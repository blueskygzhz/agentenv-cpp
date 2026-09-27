// SPDX-License-Identifier: MIT
// Rust: crates/linux-cap/src/lib.rs
//
// Linux capability inspection and delegation. Everything here is validation
// plus raw syscalls (`capset`, `prctl`) with no allocation on the hot path, so
// it is safe to call between `fork` and `exec`.
#ifndef AGENTENV_CORE_CAPABILITY_H_
#define AGENTENV_CORE_CAPABILITY_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace core {
namespace cap {

/// Rust `CAP_NET_ADMIN` / `CAP_SYS_ADMIN`.
const int kCapNetAdmin = 12;
const int kCapSysAdmin = 21;

/// Rust `CAPABILITY_SET_COUNT` * `CAPABILITIES_PER_SET`: version-3 capability
/// sets are two 32-bit words, so 64 is the highest expressible capability.
const std::size_t kCapabilitySetCount = 2;
const std::size_t kCapabilitiesPerSet = 32;
const std::size_t kMaxCapability = kCapabilitySetCount * kCapabilitiesPerSet;

/// Rust `capability_masks` — splits capabilities into the two version-3 words.
/// Rejects negatives and anything at or above `kMaxCapability` with EINVAL,
/// mirroring `invalid_capability()`.
Expected<std::vector<uint32_t>, std::string> CapabilityMasks(const std::vector<int>& capabilities);

/// Rust `CapabilitySets`.
class CapabilitySets {
 public:
    CapabilitySets() : inheritable_(0), permitted_(0), effective_(0) {}

    /// Rust `CapabilitySets::from_proc_status` — parses the `CapInh:`,
    /// `CapPrm:` and `CapEff:` hex fields out of a `/proc/*/status` dump.
    static Expected<CapabilitySets, std::string> FromProcStatus(const std::string& status);

    /// Rust `CapabilitySets::current` — reads `/proc/self/status`.
    static Expected<CapabilitySets, std::string> Current();

    /// Rust `is_delegable`: a capability can be handed to a child through the
    /// ambient set only if it is inheritable *and* permitted *and* effective.
    Expected<bool, std::string> IsDelegable(int capability) const;

    /// Rust `effective`.
    Expected<bool, std::string> Effective(int capability) const;

    uint64_t inheritable() const { return inheritable_; }
    uint64_t permitted() const { return permitted_; }
    uint64_t effective_set() const { return effective_; }

    bool operator==(const CapabilitySets& o) const {
        return inheritable_ == o.inheritable_ && permitted_ == o.permitted_ &&
               effective_ == o.effective_;
    }
    bool operator!=(const CapabilitySets& o) const { return !(*this == o); }

 private:
    uint64_t inheritable_;
    uint64_t permitted_;
    uint64_t effective_;
};

/// Rust `has_capabilities` — every capability is delegable.
Expected<bool, std::string> HasCapabilities(const std::vector<int>& capabilities);

/// Rust `has_effective_capabilities`.
Expected<bool, std::string> HasEffectiveCapabilities(const std::vector<int>& capabilities);

/// Rust `clear_ambient_capabilities` — `PR_CAP_AMBIENT_CLEAR_ALL`.
Expected<Unit, std::string> ClearAmbientCapabilities();

/// Rust `configure_current_process_capabilities` — replaces this *thread's*
/// capability sets with exactly `capabilities` and raises them into the
/// ambient set for the next `exec`. The bounding set is left unchanged.
///
/// Note that `capset` with pid 0 is thread-scoped on Linux, which is what makes
/// the scoped-launcher pattern in `privileges.h` possible.
Expected<Unit, std::string> ConfigureCurrentProcessCapabilities(
    const std::vector<int>& capabilities);

/// Rust `capability_name` (from src/privileges.rs) — the spelling used in the
/// operator-facing error message.
const char* CapabilityName(int capability);

}  // namespace cap
}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_CAPABILITY_H_
