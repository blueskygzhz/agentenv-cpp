// SPDX-License-Identifier: MIT
// Rust: src/managed_secret.rs
//
// Reads and creates secrets that AgentENV owns on local disk. The checks are
// the point of this module, not incidental:
//
//   * the parent directory must be named exactly `secrets`, so a caller cannot
//     turn some shared directory into a secret store and tighten its mode;
//   * both the directory and the file must be owned by the effective uid and
//     have modes 0700 / 0600;
//   * the secret is opened `O_NOFOLLOW`, so a symlink planted in its place is
//     rejected by the kernel rather than followed;
//   * creation goes through a temporary file plus a no-clobber link, so two
//     processes racing to generate a secret converge on one value.
#ifndef AGENTENV_MANAGED_SECRET_H_
#define AGENTENV_MANAGED_SECRET_H_

#include <cstddef>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace managed_secret {

/// Rust `enum CreateOutcome`.
struct CreateOutcome {
    enum class Kind {
        /// Rust `Created` — this process wrote the secret.
        Created,
        /// Rust `Existing(File)` — someone else won the race; the descriptor is
        /// already open on their file so the caller can read the winner.
        Existing,
    };

    Kind kind = Kind::Created;
    core::fs::FileDescriptor existing;

    bool created() const { return kind == Kind::Created; }
};

/// Rust `managed_secret::read`. Returns an empty optional when the secret does
/// not exist yet, which is the caller's signal to create it.
core::Expected<core::Optional<std::string>, std::string> Read(const std::string& path,
                                                              std::size_t max_len);

/// Rust `managed_secret::read_file` — validates an already-open descriptor and
/// reads it. Split out because `create` reuses it on the race path, where the
/// file is open but was never looked up by path.
core::Expected<std::string, std::string> ReadFile(const std::string& path, int fd,
                                                  std::size_t max_len);

/// Rust `managed_secret::create`.
core::Expected<CreateOutcome, std::string> Create(const std::string& path,
                                                  const std::string& contents);

}  // namespace managed_secret
}  // namespace agentenv
#endif  // AGENTENV_MANAGED_SECRET_H_
