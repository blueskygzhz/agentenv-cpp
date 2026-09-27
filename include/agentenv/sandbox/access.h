// SPDX-License-Identifier: MIT
// Rust: src/sandbox/access.rs
//
// Derives the per-sandbox bearer tokens that envd and the traffic proxy check.
// Both are HMAC-SHA256 over the sandbox id under a node-wide seed, which is
// what makes them reproducible: a sandbox that is persisted and later resumed
// must present the same token, and in a cluster every node must derive the
// same value. That is also why the seed is either configured explicitly or
// stored as a managed secret rather than regenerated per process.
#ifndef AGENTENV_SANDBOX_ACCESS_H_
#define AGENTENV_SANDBOX_ACCESS_H_

#include <string>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"

namespace agentenv {
namespace sandbox {

/// Rust `struct EnvdAccessToken(String)`.
///
/// A newtype rather than a bare `std::string` so that the token cannot reach a
/// log line by accident: `DebugString` is redacted, and reading the real value
/// requires the deliberately-named `Expose`.
class EnvdAccessToken {
 public:
    EnvdAccessToken() {}
    explicit EnvdAccessToken(const std::string& value) : value_(value) {}

    /// Rust `EnvdAccessToken::expose`.
    const std::string& Expose() const { return value_; }

    /// Rust `impl fmt::Debug for EnvdAccessToken`.
    std::string DebugString() const { return "EnvdAccessToken(<redacted>)"; }

    bool operator==(const EnvdAccessToken& o) const { return value_ == o.value_; }
    bool operator!=(const EnvdAccessToken& o) const { return !(*this == o); }

 private:
    std::string value_;
};

/// Rust `struct SandboxAccessTokenGenerator`.
class SandboxAccessTokenGenerator {
 public:
    SandboxAccessTokenGenerator() {}

    /// Rust `SandboxAccessTokenGenerator::new` — trims the seed and rejects an
    /// empty one, so a blank environment variable cannot silently produce a
    /// node whose tokens are derived from nothing.
    static core::Expected<SandboxAccessTokenGenerator, std::string> New(
        const std::string& seed);

    /// Rust `SandboxAccessTokenGenerator::load_or_create`.
    ///
    /// `managed_seed_must_exist` is set when token-protected persisted
    /// sandboxes are present: regenerating the seed there would invalidate
    /// every stored token, so a missing file is an error instead.
    static core::Expected<SandboxAccessTokenGenerator, std::string> LoadOrCreate(
        const cfg::AppConfig& config, bool managed_seed_must_exist);

    /// Rust `SandboxAccessTokenGenerator::generate` — the envd token.
    EnvdAccessToken Generate(const core::SandboxId& subject) const;

    /// Rust `SandboxAccessTokenGenerator::generate_traffic`.
    ///
    /// Domain-separated by a prefix so that the envd token and the traffic
    /// token for one sandbox are unrelated: leaking the proxy token must not
    /// grant access to the exec channel.
    std::string GenerateTraffic(const core::SandboxId& subject) const;

    /// Rust `SandboxAccessTokenGenerator::matches`.
    bool Matches(const core::SandboxId& subject, const std::string& candidate) const;

    /// Rust `SandboxAccessTokenGenerator::matches_traffic`.
    bool MatchesTraffic(const core::SandboxId& subject, const std::string& candidate) const;

    /// Rust `impl fmt::Debug for SandboxAccessTokenGenerator`.
    std::string DebugString() const { return "SandboxAccessTokenGenerator(<redacted>)"; }

    /// Exposed for tests only, mirroring Rust's `generator.seed` field access
    /// from its own test module.
    const std::vector<uint8_t>& SeedForTesting() const { return seed_; }

 private:
    std::string GenerateFor(const std::string& subject) const;
    bool MatchesFor(const std::string& subject, const std::string& candidate) const;

    std::vector<uint8_t> seed_;
};

// ---- internals exposed for tests (Rust's private fns, same module) --------

/// Rust `MANAGED_SEED_RELATIVE_PATH`.
extern const char* const kManagedSeedRelativePath;
/// Rust `MANAGED_SEED_BYTES`.
const std::size_t kManagedSeedBytes = 32;
/// Rust `SEED_HEX_LEN`.
const std::size_t kSeedHexLen = kManagedSeedBytes * 2;
/// Rust `MANAGED_SEED_FILE_MAX_LEN` — the hex plus one optional newline, so an
/// oversized file is rejected by length before its contents are read.
const std::size_t kManagedSeedFileMaxLen = kSeedHexLen + 1;
/// Rust `TRAFFIC_ACCESS_TOKEN_PREFIX`.
extern const char* const kTrafficAccessTokenPrefix;

/// Rust `resolve_seed`.
core::Expected<std::string, std::string> ResolveSeed(const std::string& managed_path,
                                                     bool managed_seed_must_exist);

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_ACCESS_H_
