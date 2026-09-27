// SPDX-License-Identifier: MIT
// Rust: crates/shell-util/  — shell argument quoting.
//
// This C++11 port is a strict alignment with the upstream Rust crate, whose
// entire public surface is a single free function:
//
//     pub fn shell_quote(s: &str) -> String
//
// It wraps a shell argument in single quotes if it contains characters that
// need quoting, escaping internal single quotes as `'\''`.
#ifndef AGENTENV_SHELL_UTIL_H_
#define AGENTENV_SHELL_UTIL_H_

#include <string>

namespace agentenv {
namespace shellutil {

/// Wraps a shell argument in single quotes if it contains characters that need
/// quoting. Internal single quotes are escaped as `'\''`.
///
/// Mirrors Rust `shell_util::shell_quote`:
/// - empty string  -> `''`
/// - "safe" strings (alphanumeric plus `- _ . / : @ + =`) pass through verbatim
/// - anything else  -> wrapped in single quotes with `'` -> `'\''`
std::string ShellQuote(const std::string& s);

}  // namespace shellutil
}  // namespace agentenv
#endif  // AGENTENV_SHELL_UTIL_H_
