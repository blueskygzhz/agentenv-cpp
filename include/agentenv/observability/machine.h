// SPDX-License-Identifier: MIT
// Rust: src/observability/machine.rs — mostly-static machine descriptors.
#ifndef AGENTENV_OBSERVABILITY_MACHINE_H_
#define AGENTENV_OBSERVABILITY_MACHINE_H_

#include <string>
#include <vector>

#include "agentenv/core/optional.h"
#include "agentenv/observability/model.h"

namespace agentenv {
namespace observability {

/// Rust `detect_machine_info` — reads `/proc/cpuinfo` once at service
/// construction. Every field falls back to `"unknown"`; `cpu_config_json` is
/// deliberately left unset here (the reporter fills it in separately).
MachineInfo DetectMachineInfo();

/// Rust `dump_cpu_config` — runs `<path> template dump -o /dev/stdout` and
/// returns its stdout. Yields nothing when the binary is missing, exits
/// non-zero, or prints an empty document.
///
/// Rust wraps this in `spawn_blocking` because it sits in an async fn; here it
/// is a plain blocking call, which is what `spawn_blocking` degrades to.
core::Optional<std::string> DumpCpuConfig(const std::string& path);

/// Rust `first_cpuinfo_value` — first `/proc/cpuinfo` row whose trimmed key
/// matches any candidate and whose trimmed value is non-empty.
core::Optional<std::string> FirstCpuinfoValue(const std::string& cpuinfo,
                                              const std::vector<std::string>& keys);

}  // namespace observability
}  // namespace agentenv
#endif  // AGENTENV_OBSERVABILITY_MACHINE_H_
