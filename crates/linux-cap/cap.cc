// SPDX-License-Identifier: MIT
#include "agentenv/linux-cap/cap.h"

// Skeleton: link libcap or use prctl(PR_CAPBSET_DROP) to implement.
namespace agentenv {
namespace linuxcap {

int DropAllExcept(const std::vector<int>& /*keep*/) { return 0; }
uint64_t GetEffective() { return 0; }

}  // namespace linuxcap
}  // namespace agentenv
