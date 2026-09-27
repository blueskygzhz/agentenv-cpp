// SPDX-License-Identifier: MIT
// Rust: crates/linux-cap/  — capability manipulation.
#ifndef AGENTENV_LINUX_CAP_H_
#define AGENTENV_LINUX_CAP_H_

#include <cstdint>
#include <vector>

namespace agentenv {
namespace linuxcap {

// Standard Linux capability numbers (subset).
enum Cap : int {
    CAP_CHOWN         = 0,
    CAP_DAC_OVERRIDE  = 1,
    CAP_NET_ADMIN     = 12,
    CAP_SYS_ADMIN     = 21,
    CAP_SYS_CHROOT    = 18,
    CAP_MKNOD         = 27,
};

/// Drop all capabilities except those in `keep`. Returns 0 on success, -errno on failure.
int DropAllExcept(const std::vector<int>& keep);

/// Query current effective set — returns a bitset in the first 64 caps.
uint64_t GetEffective();

}  // namespace linuxcap
}  // namespace agentenv
#endif  // AGENTENV_LINUX_CAP_H_
