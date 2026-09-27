// SPDX-License-Identifier: MIT
// Rust: src/api_key.rs
#include "agentenv/api_key.h"

#include <cstdlib>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/managed_secret.h"

namespace agentenv {

const char* const kApiKeyEnv = "AENV_API_KEY";
const char* const kExternalApiKeyPath = "/run/secrets/api-key";
const char* const kManagedApiKeyRelativePath = "secrets/api-key";
const char* const kGeneratedApiKeyPrefix = "e2b_";

namespace {

using core::Optional;
using core::Unit;
namespace fs = core::fs;

bool IsUrlSafeByte(unsigned char byte) {
    const bool alphanumeric = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                              (byte >= '0' && byte <= '9');
    return alphanumeric || byte == '.' || byte == '_' || byte == '~' || byte == '-';
}

/// Rust `rand::rngs::SysRng::try_fill_bytes` — the OS CSPRNG. `/dev/urandom`
/// is used directly rather than `rand()`, which is not cryptographically
/// suitable and would make generated keys predictable.
core::Expected<std::string, std::string> RandomBytes(std::size_t count) {
    core::Expected<fs::FileDescriptor, std::string> urandom =
        fs::OpenReadFollow("/dev/urandom");
    if (!urandom.ok()) {
        return core::make_unexpected(std::string("generate managed API key: ") +
                                     urandom.error());
    }

    std::string out;
    out.reserve(count);
    while (out.size() < count) {
        const core::Expected<std::string, std::string> chunk =
            fs::ReadFdToString(urandom.value().get(), count - out.size());
        if (!chunk.ok()) {
            return core::make_unexpected(std::string("generate managed API key: ") +
                                         chunk.error());
        }
        if (chunk.value().empty()) {
            // /dev/urandom never reaches EOF; treat it as a broken source
            // rather than silently producing a short key.
            return core::make_unexpected(
                std::string("generate managed API key: /dev/urandom returned no data"));
        }
        out += chunk.value();
    }
    return out;
}

/// Rust `hex::encode`.
std::string HexEncode(const std::string& bytes) {
    static const char* const kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(bytes[i]);
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

/// Rust `subtle::ConstantTimeEq`. The loop must not short-circuit: an early
/// return would leak the length of the matching prefix through timing.
bool ConstantTimeEquals(const unsigned char* a, const unsigned char* b, std::size_t len) {
    unsigned char difference = 0;
    for (std::size_t i = 0; i < len; ++i) {
        difference = static_cast<unsigned char>(difference | (a[i] ^ b[i]));
    }
    return difference == 0;
}

std::string StripSuffix(const std::string& value, char suffix) {
    if (!value.empty() && value[value.size() - 1] == suffix) {
        return value.substr(0, value.size() - 1);
    }
    return value;
}

}  // namespace

core::Expected<ApiKey, std::string> ApiKey::New(const std::string& value) {
    bool valid = value.size() >= kApiKeyMinLen && value.size() <= kApiKeyMaxLen;
    if (valid) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (!IsUrlSafeByte(static_cast<unsigned char>(value[i]))) {
                valid = false;
                break;
            }
        }
    }
    if (!valid) {
        std::ostringstream oss;
        oss << "API key must contain between " << kApiKeyMinLen << " and " << kApiKeyMaxLen
            << " URL-safe characters";
        return core::make_unexpected(oss.str());
    }
    return ApiKey(value);
}

bool ApiKey::Matches(const unsigned char* candidate, std::size_t len) const {
    // The length comparison is not constant-time, which is fine: the key's
    // length is not the secret. The content comparison is.
    if (len != value_.size()) return false;
    return ConstantTimeEquals(candidate,
                              reinterpret_cast<const unsigned char*>(value_.data()), len);
}

bool ApiKey::Matches(const std::string& candidate) const {
    return Matches(reinterpret_cast<const unsigned char*>(candidate.data()), candidate.size());
}

bool ApiKey::HasGeneratedPrefix() const {
    const std::string prefix = kGeneratedApiKeyPrefix;
    return value_.size() >= prefix.size() && value_.compare(0, prefix.size(), prefix) == 0;
}

core::Expected<ApiKey, std::string> ApiKey::FromFileContents(const std::string& value) {
    // One "\n" then one "\r": handles both LF and CRLF, and deliberately does
    // not trim arbitrary whitespace, so a key with a stray space stays invalid.
    return New(StripSuffix(StripSuffix(value, '\n'), '\r'));
}

core::Expected<Optional<ApiKey>, std::string> ApiKey::ReadExternal(const std::string& path) {
    // Symlinks are followed here on purpose: a Kubernetes projected secret is
    // a symlink into a timestamped directory.
    core::Expected<Optional<fs::FileDescriptor>, std::string> opened =
        fs::OpenReadFollowOptional(path);
    if (!opened.ok()) return core::make_unexpected(opened.error());
    if (!opened.value().has_value()) return Optional<ApiKey>();

    const core::Expected<fs::FileStat, std::string> stat = fs::StatFd(opened.value()->get());
    if (!stat.ok()) return core::make_unexpected(stat.error());
    if (!stat.value().is_regular) {
        return core::make_unexpected(std::string("API key secret must be a regular file"));
    }

    // Read one byte past the cap so an oversized file is detected rather than
    // silently truncated into a valid-looking key.
    const core::Expected<std::string, std::string> contents =
        fs::ReadFdToString(opened.value()->get(), kApiKeyFileMaxLen + 1);
    if (!contents.ok()) return core::make_unexpected(contents.error());
    if (contents.value().size() > kApiKeyFileMaxLen) {
        std::ostringstream oss;
        oss << "API key file must be at most " << kApiKeyFileMaxLen << " bytes";
        return core::make_unexpected(oss.str());
    }

    const core::Expected<ApiKey, std::string> key = FromFileContents(contents.value());
    if (!key.ok()) return core::make_unexpected(key.error());
    return Optional<ApiKey>(key.value());
}

core::Expected<ApiKey, std::string> ApiKey::Create(const std::string& path) {
    const core::Expected<std::string, std::string> random = RandomBytes(32);
    if (!random.ok()) return core::make_unexpected(random.error());

    const std::string key = std::string(kGeneratedApiKeyPrefix) + HexEncode(random.value());

    core::Expected<managed_secret::CreateOutcome, std::string> outcome =
        managed_secret::Create(path, key + "\n");
    if (!outcome.ok()) return core::make_unexpected(outcome.error());

    if (outcome.value().created()) {
        AGENTENV_INFO("generated managed API key path=" << path);
        return New(key);
    }

    // Another process published first; adopt their value so every replica
    // agrees on one key.
    const core::Expected<std::string, std::string> existing =
        managed_secret::ReadFile(path, outcome.value().existing.get(), kApiKeyFileMaxLen);
    if (!existing.ok()) {
        return core::make_unexpected(std::string("load concurrently generated API key: ") +
                                     existing.error());
    }
    const core::Expected<ApiKey, std::string> adopted = FromFileContents(existing.value());
    if (!adopted.ok()) {
        return core::make_unexpected(std::string("invalid concurrently generated API key: ") +
                                     adopted.error());
    }
    return adopted.value();
}

core::Expected<ApiKey, std::string> ApiKey::ResolveFrom(
    const Optional<std::string>& explicit_value, const std::string& external_path,
    const std::string& home_path) {
    if (explicit_value.has_value()) {
        const core::Expected<ApiKey, std::string> key = New(*explicit_value);
        if (!key.ok()) {
            return core::make_unexpected(std::string("invalid AENV_API_KEY: ") + key.error());
        }
        return key.value();
    }

    const core::Expected<Optional<ApiKey>, std::string> external = ReadExternal(external_path);
    if (!external.ok()) {
        return core::make_unexpected(std::string("load external API key: ") + external.error());
    }
    if (external.value().has_value()) {
        AGENTENV_INFO("loaded API key from external secret path=" << external_path);
        return *external.value();
    }

    const std::string managed_path = fs::Join(home_path, kManagedApiKeyRelativePath);
    const core::Expected<Optional<std::string>, std::string> managed =
        managed_secret::Read(managed_path, kApiKeyFileMaxLen);
    if (!managed.ok()) {
        return core::make_unexpected(std::string("load managed API key: ") + managed.error());
    }
    if (managed.value().has_value()) {
        const core::Expected<ApiKey, std::string> key = FromFileContents(*managed.value());
        if (!key.ok()) {
            return core::make_unexpected(std::string("invalid managed API key: ") + key.error());
        }
        return key.value();
    }

    return Create(managed_path);
}

core::Expected<ApiKey, std::string> ApiKey::Resolve(const std::string& home_path) {
    const char* env = ::getenv(kApiKeyEnv);
    Optional<std::string> explicit_value;
    if (env != NULL) explicit_value = std::string(env);

    return ResolveFrom(explicit_value, kExternalApiKeyPath, home_path);
}

}  // namespace agentenv
