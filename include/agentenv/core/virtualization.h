// SPDX-License-Identifier: MIT
// Rust: src/virtualization.rs — node-wide virtualization backend and snapshot
// compatibility domain.
//
// Ported 1:1 including `Default` (= Kvm), `Display` and `FromStr` semantics.
#ifndef AGENTENV_CORE_VIRTUALIZATION_H_
#define AGENTENV_CORE_VIRTUALIZATION_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace core {

/// Rust enum `VirtualizationMode` (`#[serde(rename_all = "lowercase")]`).
enum class VirtualizationMode {
    Kvm,  // Rust `#[default]`
    Pvm,
};

/// Rust `impl Default for VirtualizationMode`.
inline VirtualizationMode VirtualizationModeDefault() { return VirtualizationMode::Kvm; }

/// Rust `impl Display for VirtualizationMode`.
const char* VirtualizationModeToString(VirtualizationMode mode);

/// Rust `impl FromStr for VirtualizationMode` — trims and lowercases, and
/// reproduces the upstream error message verbatim:
///   `unsupported virtualization mode "<other>"; expected "kvm" or "pvm"`
Expected<VirtualizationMode, std::string> VirtualizationModeParse(const std::string& raw);

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_VIRTUALIZATION_H_
