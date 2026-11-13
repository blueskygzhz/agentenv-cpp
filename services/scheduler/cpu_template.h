// SPDX-License-Identifier: MIT
// Upstream (Go): services/scheduler/internal/cpu_template.go — the
// conservative CPU-feature intersection used to pick a migration-safe
// Firecracker `cpu_config_json` across a set of nodes.
#ifndef AGENTENV_SERVICES_SCHEDULER_CPU_TEMPLATE_H_
#define AGENTENV_SERVICES_SCHEDULER_CPU_TEMPLATE_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace services {
namespace scheduler {

/// Go: `IntersectCpuConfigs`.
///
/// Computes a bitwise-AND intersection of Firecracker `cpu_config_json`
/// strings: only entries present in *every* input survive, and the bitmaps of
/// shared entries are ANDed. The result is what every node in the set can
/// honour, so a guest booted with it stays migratable across all of them.
///
/// Returns an empty string (and no error) for an empty input, matching Go --
/// "no constraints collected" is distinct from "the empty constraint set".
///
/// The intersection is deliberately *conservative* rather than exact: the
/// candidate register set is seeded from the first config only, so a register
/// present in all of configs[1..n] but absent from configs[0] is dropped
/// instead of retained. Narrower than the true intersection is safe for
/// migration; wider would not be. This mirrors the upstream note and must not
/// be "fixed" without changing Go in step.
core::Expected<std::string, std::string> IntersectCpuConfigs(
    const std::vector<std::string>& jsons);

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
#endif  // AGENTENV_SERVICES_SCHEDULER_CPU_TEMPLATE_H_
