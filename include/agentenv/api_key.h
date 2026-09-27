// SPDX-License-Identifier: MIT
// Rust: src/api_key.rs
#ifndef AGENTENV_API_KEY_H_
#define AGENTENV_API_KEY_H_

#include <cstddef>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {

/// Rust `API_KEY_ENV`.
extern const char* const kApiKeyEnv;
/// Rust `EXTERNAL_API_KEY_PATH` — where an operator or Kubernetes mounts one.
extern const char* const kExternalApiKeyPath;
/// Rust `MANAGED_API_KEY_RELATIVE_PATH`, relative to `home_path`.
extern const char* const kManagedApiKeyRelativePath;
/// Rust `GENERATED_API_KEY_PREFIX`.
extern const char* const kGeneratedApiKeyPrefix;

/// Rust `API_KEY_MAX_LEN`.
const std::size_t kApiKeyMaxLen = 256;
/// Rust `API_KEY_FILE_MAX_LEN` — the key plus a possible CRLF.
const std::size_t kApiKeyFileMaxLen = kApiKeyMaxLen + 2;
/// Rust's lower bound, spelled `(32..=API_KEY_MAX_LEN)`.
const std::size_t kApiKeyMinLen = 32;

/// Rust newtype `ApiKey(String)`.
///
/// The value is deliberately not exposed: comparison goes through `Matches`,
/// which is constant-time, and there is no accessor to print it. Rust enforces
/// the same by keeping the field private and giving `Debug` a redacted body.
class ApiKey {
 public:
    /// Rust `ApiKey::new` — 32..=256 URL-safe characters
    /// (`[A-Za-z0-9._~-]`).
    static core::Expected<ApiKey, std::string> New(const std::string& value);

    /// Rust `ApiKey::resolve_from`, with the sources in priority order:
    ///   1. the `AENV_API_KEY` environment value, when set;
    ///   2. an external secret file, when present;
    ///   3. the managed secret under `home_path`, read or generated.
    /// Exposed (rather than only `Resolve`) because it is the testable seam.
    static core::Expected<ApiKey, std::string> ResolveFrom(
        const core::Optional<std::string>& explicit_value, const std::string& external_path,
        const std::string& home_path);

    /// Rust `ApiKey::resolve` — reads `AENV_API_KEY` and the compiled-in
    /// external path, then falls back to the managed secret under `home_path`.
    static core::Expected<ApiKey, std::string> Resolve(const std::string& home_path);

    /// Rust `ApiKey::matches` — constant-time equality, so a caller cannot
    /// learn the key one byte at a time from response timing.
    bool Matches(const std::string& candidate) const;
    bool Matches(const unsigned char* candidate, std::size_t len) const;

    /// Rust `impl Debug` — `ApiKey([REDACTED])`.
    std::string ToDebugString() const { return "ApiKey([REDACTED])"; }

    /// Length is safe to expose (it is already bounded and non-secret) and the
    /// tests need it to build a same-length mismatching candidate.
    std::size_t size() const { return value_.size(); }

    /// Whether this key carries the generated-key prefix. Used by tests and by
    /// diagnostics that must not print the key itself.
    bool HasGeneratedPrefix() const;

 private:
    explicit ApiKey(std::string value) : value_(value) {}

    /// Rust `from_file_contents` — strips one trailing "\n" then one "\r",
    /// so both LF and CRLF files work, before validating.
    static core::Expected<ApiKey, std::string> FromFileContents(const std::string& value);

    /// Rust `read_external`.
    static core::Expected<core::Optional<ApiKey>, std::string> ReadExternal(
        const std::string& path);

    /// Rust `create` — generates 32 random bytes, hex-encodes them behind the
    /// prefix, and publishes them as a managed secret.
    static core::Expected<ApiKey, std::string> Create(const std::string& path);

    std::string value_;
};

}  // namespace agentenv
#endif  // AGENTENV_API_KEY_H_
