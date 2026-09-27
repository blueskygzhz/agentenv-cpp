// SPDX-License-Identifier: MIT
// Rust: src/setup/deps.rs — the shared helpers used by the dependency
// provisioning paths (and by `setup/overlaybd`).
//
// Porting note on the two external capabilities Rust gets from crates:
//
//   * `download_file` uses `reqwest`. There is no HTTP client in this tree, so
//     the port shells out to `curl`. That is not a shortcut: the bundled
//     `config/deps_manifest.toml` already declares `curl` a *required* runtime
//     command, so relying on it adds no new dependency and keeps the build
//     free of third-party libraries.
//   * archive extraction uses `flate2` + `tar`. The port shells out to `tar`,
//     a POSIX-standard tool, for the same reason.
//
// Both are invoked through `core::process`, i.e. argv is passed straight to
// `execvp` with no shell, so a URL or path can never be interpreted as a
// command.
#ifndef AGENTENV_SETUP_DEPS_H_
#define AGENTENV_SETUP_DEPS_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace setup {
namespace deps {

/// Rust `detect_arch` — only the two architectures AgentENV releases are
/// built for; anything else is a hard error rather than a guess.
core::Expected<std::string, std::string> DetectArch();

/// Rust `set_file_mode`.
core::Expected<core::Unit, std::string> SetFileMode(const std::string& path, uint32_t mode);

/// Rust `set_executable` — 0755.
core::Expected<core::Unit, std::string> SetExecutable(const std::string& path);

/// Rust `copy_file` — creates the destination's parent, copies, and optionally
/// marks the result executable.
core::Expected<core::Unit, std::string> CopyFile(const std::string& source,
                                                 const std::string& destination,
                                                 bool executable);

/// Rust `file_exists_nonempty` — a zero-length file counts as absent, because
/// that is what a truncated earlier download leaves behind.
bool FileExistsNonEmpty(const std::string& path);

/// Rust `download_file`.
///
/// Skips the transfer when the destination already exists and is non-empty.
/// Downloads to a sibling `.tmp` and renames, so a partial transfer never
/// appears under the final name.
core::Expected<core::Unit, std::string> DownloadFile(const std::string& url,
                                                     const std::string& destination);

/// Rust `download_executable_file`.
core::Expected<core::Unit, std::string> DownloadExecutableFile(const std::string& url,
                                                               const std::string& destination);

/// Extracts a `.tar.gz` into `destination`, which is created if needed.
/// Replaces Rust's `GzDecoder` + `tar::Archive::unpack`.
core::Expected<core::Unit, std::string> ExtractTarGz(const std::string& archive_path,
                                                     const std::string& destination);

/// Rust `copy_dir_recursive`.
core::Expected<core::Unit, std::string> CopyDirRecursive(const std::string& source,
                                                         const std::string& destination);

/// Sets the group owner without touching the user, i.e. Rust
/// `chown(path, None, Some(gid))`.
core::Expected<core::Unit, std::string> SetGroupOwner(const std::string& path, uint32_t gid);

}  // namespace deps
}  // namespace setup
}  // namespace agentenv
#endif  // AGENTENV_SETUP_DEPS_H_
