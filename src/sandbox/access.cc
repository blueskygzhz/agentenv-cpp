// SPDX-License-Identifier: MIT
// Rust: src/sandbox/access.rs
#include "agentenv/sandbox/access.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/managed_secret.h"

namespace agentenv {
namespace sandbox {

const char* const kManagedSeedRelativePath = "secrets/sandbox-access-token-hash-seed";
const char* const kTrafficAccessTokenPrefix = "sandbox-traffic";

namespace {

/// Rust `str::trim` — Unicode whitespace in Rust, ASCII here, which covers
/// every character an environment variable or TOML string realistically
/// carries into this path.
std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

/// Rust `validate_explicit_seed`.
core::Expected<std::string, std::string> ValidateExplicitSeed(const std::string& seed) {
    const std::string trimmed = Trim(seed);
    if (trimmed.empty()) {
        return core::make_unexpected(
            std::string("[sandbox].access_token_hash_seed must be non-empty when configured"));
    }
    return trimmed;
}

/// Rust `is_valid_managed_seed` — lowercase hex only. Accepting uppercase
/// would let two spellings of one seed exist, and the token derived from the
/// raw ASCII bytes would differ between them.
bool IsValidManagedSeed(const std::string& seed) {
    if (seed.size() != kSeedHexLen) return false;
    for (std::size_t i = 0; i < seed.size(); ++i) {
        const char c = seed[i];
        const bool digit = c >= '0' && c <= '9';
        const bool lower_hex = c >= 'a' && c <= 'f';
        if (!digit && !lower_hex) return false;
    }
    return true;
}

/// Rust `validate_managed_seed`.
core::Expected<std::string, std::string> ValidateManagedSeed(const std::string& path,
                                                             const std::string& contents) {
    // Rust `strip_suffix('\n')`: exactly one optional trailing newline, which
    // is what `create_managed_seed` writes.
    std::string seed = contents;
    if (!seed.empty() && seed[seed.size() - 1] == '\n') {
        seed.erase(seed.size() - 1);
    }
    if (!IsValidManagedSeed(seed)) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%zu", kSeedHexLen);
        return core::make_unexpected(std::string("managed sandbox access-token seed ") + path +
                                     " must contain exactly " + buffer +
                                     " lowercase hexadecimal characters, optionally followed by"
                                     " a newline");
    }
    return seed;
}

/// Rust `create_managed_seed`.
core::Expected<std::string, std::string> CreateManagedSeed(const std::string& path) {
    uint8_t random[kManagedSeedBytes];
    const core::Expected<core::Unit, std::string> filled =
        core::FillSecureRandom(random, sizeof(random));
    if (!filled.ok()) {
        return core::make_unexpected(std::string("generate managed sandbox access-token seed: ") +
                                     filled.error());
    }
    const std::string seed = core::HexEncode(random, sizeof(random));

    const core::Expected<managed_secret::CreateOutcome, std::string> outcome =
        managed_secret::Create(path, seed + "\n");
    if (!outcome.ok()) return core::make_unexpected(outcome.error());

    if (outcome.value().created()) {
        AGENTENV_INFO("generated managed sandbox access-token seed path=", path);
        return seed;
    }

    // Rust `CreateOutcome::Existing(file)`: another process won the race, so
    // read *their* seed instead of returning ours. Both processes must end up
    // with the same value or the tokens they mint would not interoperate.
    const core::Expected<std::string, std::string> contents = managed_secret::ReadFile(
        path, outcome.value().existing.get(), kManagedSeedFileMaxLen);
    if (!contents.ok()) return core::make_unexpected(contents.error());
    return ValidateManagedSeed(path, contents.value());
}

}  // namespace

core::Expected<std::string, std::string> ResolveSeed(const std::string& managed_path,
                                                     bool managed_seed_must_exist) {
    const core::Expected<core::Optional<std::string>, std::string> existing =
        managed_secret::Read(managed_path, kManagedSeedFileMaxLen);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (existing.value().has_value()) {
        return ValidateManagedSeed(managed_path, *existing.value());
    }

    if (managed_seed_must_exist) {
        return core::make_unexpected(
            std::string("managed sandbox access-token seed ") + managed_path +
            " is missing while token-protected persisted sandboxes exist; restore the file or"
            " configure AENV_SANDBOX_ACCESS_TOKEN_HASH_SEED");
    }

    return CreateManagedSeed(managed_path);
}

core::Expected<SandboxAccessTokenGenerator, std::string> SandboxAccessTokenGenerator::New(
    const std::string& seed) {
    const core::Expected<std::string, std::string> validated = ValidateExplicitSeed(seed);
    if (!validated.ok()) return core::make_unexpected(validated.error());

    SandboxAccessTokenGenerator generator;
    // The seed is used as the raw HMAC key, exactly as Rust does with
    // `seed.as_bytes()`; it is not hashed or decoded first.
    generator.seed_.assign(validated.value().begin(), validated.value().end());
    return generator;
}

core::Expected<SandboxAccessTokenGenerator, std::string>
SandboxAccessTokenGenerator::LoadOrCreate(const cfg::AppConfig& config,
                                          bool managed_seed_must_exist) {
    if (config.sandbox.access_token_hash_seed.has_value()) {
        // An explicit seed wins and, importantly, no managed state is created:
        // writing a file here would leave a stale seed behind that a later
        // unconfigured start would silently adopt.
        return New(*config.sandbox.access_token_hash_seed);
    }

    const std::string managed_seed_path =
        core::fs::Join(config.home_path, kManagedSeedRelativePath);
    const core::Expected<std::string, std::string> seed =
        ResolveSeed(managed_seed_path, managed_seed_must_exist);
    if (!seed.ok()) return core::make_unexpected(seed.error());

    if (config.cluster.scheduler_endpoint.has_value()) {
        // A node-local seed is fine for a single node but silently breaks
        // cross-node resume, so say so while it is still recoverable.
        AGENTENV_WARN(
            "using a node-local managed sandbox access-token seed; configure"
            " AENV_SANDBOX_ACCESS_TOKEN_HASH_SEED with the same value on every node in a"
            " clustered deployment path=",
            managed_seed_path);
    }

    return New(seed.value());
}

std::string SandboxAccessTokenGenerator::GenerateFor(const std::string& subject) const {
    const std::array<uint8_t, 32> tag = core::HmacSha256(
        seed_.empty() ? NULL : &seed_[0], seed_.size(), subject.data(), subject.size());
    return core::HexEncode(tag.data(), tag.size());
}

EnvdAccessToken SandboxAccessTokenGenerator::Generate(const core::SandboxId& subject) const {
    return EnvdAccessToken(GenerateFor(subject.ToString()));
}

std::string SandboxAccessTokenGenerator::GenerateTraffic(
    const core::SandboxId& subject) const {
    return GenerateFor(std::string(kTrafficAccessTokenPrefix) + "-" + subject.ToString());
}

bool SandboxAccessTokenGenerator::MatchesFor(const std::string& subject,
                                             const std::string& candidate) const {
    // Rust decodes into a zeroed 32-byte buffer and then verifies
    // unconditionally, combining the two results with a non-short-circuiting
    // `&`. Keep that shape: bailing out early on a malformed candidate would
    // make "wrong length" distinguishable from "wrong value" by timing.
    uint8_t candidate_bytes[32];
    std::memset(candidate_bytes, 0, sizeof(candidate_bytes));
    const bool decoded =
        core::HexDecodeExact(candidate, candidate_bytes, sizeof(candidate_bytes));

    const std::array<uint8_t, 32> expected = core::HmacSha256(
        seed_.empty() ? NULL : &seed_[0], seed_.size(), subject.data(), subject.size());
    const bool verified =
        core::ConstantTimeEquals(expected.data(), candidate_bytes, sizeof(candidate_bytes));
    return verified & decoded;
}

bool SandboxAccessTokenGenerator::Matches(const core::SandboxId& subject,
                                          const std::string& candidate) const {
    return MatchesFor(subject.ToString(), candidate);
}

bool SandboxAccessTokenGenerator::MatchesTraffic(const core::SandboxId& subject,
                                                 const std::string& candidate) const {
    return MatchesFor(std::string(kTrafficAccessTokenPrefix) + "-" + subject.ToString(),
                      candidate);
}

}  // namespace sandbox
}  // namespace agentenv
