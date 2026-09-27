// SPDX-License-Identifier: MIT
// Rust: src/image/metadata.rs
#ifndef AGENTENV_IMAGE_METADATA_H_
#define AGENTENV_IMAGE_METADATA_H_

#include <map>
#include <string>
#include <vector>

namespace agentenv {
namespace image {

/// Rust struct `ImageBaseContext`.
struct ImageBaseContext {
    std::string             working_dir;
    std::string             user;
    std::vector<std::string> entrypoint;
    std::vector<std::string> cmd;
    std::map<std::string, std::string> labels;
};

/// Rust struct `ImageResolutionMetadata`.
struct ImageResolutionMetadata {
    std::string       resolved_digest;
    std::string       platform_os;
    std::string       platform_arch;
    ImageBaseContext  base;
    std::vector<std::string> env_entries;   // KEY=VALUE list
};

/// Rust fn `env_vars_from_entries`.
std::map<std::string, std::string> EnvVarsFromEntries(
    const std::vector<std::string>& entries);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_METADATA_H_
