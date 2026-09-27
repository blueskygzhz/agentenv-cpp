// SPDX-License-Identifier: MIT
// Rust: src/sandbox/access.rs test module, plus RFC 4231 vectors for the
// HMAC-SHA256 primitive this port had to add to `core::digest`.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/sandbox/access.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::SandboxId;
using agentenv::core::Unit;
namespace fs = agentenv::core::fs;
namespace sandbox = agentenv::sandbox;

struct TempRoot {
    std::string path;

    TempRoot() {
        const agentenv::core::Expected<std::string, std::string> temp =
            fs::CreateTempDir("agentenv-access-");
        MT_EXPECT_TRUE(temp.ok());
        path = temp.value();
    }
    ~TempRoot() { fs::RemoveDirAll(path); }

    std::string Join(const std::string& leaf) const { return fs::Join(path, leaf); }
};

/// Rust test fixture: `SandboxId::try_from("01936f8e-72f5-7000-8000-000000000001")`.
SandboxId FixtureSandboxId() {
    agentenv::core::Uuid uuid;
    MT_EXPECT_TRUE(
        agentenv::core::Uuid::Parse("01936f8e-72f5-7000-8000-000000000001", &uuid));
    return SandboxId(uuid);
}

/// Rust test helper `create_private_managed_seed_directory`.
void CreatePrivateManagedSeedDirectory(const std::string& path) {
    MT_EXPECT_TRUE(fs::CreateDirAll(path).ok());
    MT_EXPECT_EQ(::chmod(path.c_str(), 0700), 0);
}

uint32_t ModeOf(const std::string& path) {
    struct stat info;
    MT_EXPECT_EQ(::stat(path.c_str(), &info), 0);
    return info.st_mode & 0777;
}

std::string RepeatChar(char c, std::size_t n) { return std::string(n, c); }

}  // namespace

// ---------------------------------------------------------------------------
// HMAC-SHA256 primitive (RFC 4231)
// ---------------------------------------------------------------------------

MT_TEST(hmac_sha256_matches_rfc_4231_vectors) {
    // Case 1: 20-byte key, "Hi There".
    MT_EXPECT_EQ(agentenv::core::HmacSha256Hex(RepeatChar('\x0b', 20), "Hi There"),
                 std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32"
                             "cff7"));

    // Case 2: key shorter than the block, so it is zero-padded.
    MT_EXPECT_EQ(agentenv::core::HmacSha256Hex("Jefe", "what do ya want for nothing?"),
                 std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec"
                             "3843"));

    // Case 3: 20-byte 0xaa key over 50 bytes of 0xdd.
    MT_EXPECT_EQ(
        agentenv::core::HmacSha256Hex(RepeatChar('\xaa', 20), RepeatChar('\xdd', 50)),
        std::string("773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"));

    // Case 6: 131-byte key, longer than SHA-256's 64-byte block, so RFC 2104
    // replaces it with its own digest. This is the branch that a naive
    // implementation gets wrong.
    MT_EXPECT_EQ(agentenv::core::HmacSha256Hex(
                     RepeatChar('\xaa', 131),
                     "Test Using Larger Than Block-Size Key - Hash Key First"),
                 std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee3"
                             "7f54"));

    // Case 7: 131-byte key with a long message.
    MT_EXPECT_EQ(
        agentenv::core::HmacSha256Hex(
            RepeatChar('\xaa', 131),
            "This is a test using a larger than block-size key and a larger than "
            "block-size data. The key needs to be hashed before being used by the HMAC "
            "algorithm."),
        std::string("9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"));
}

MT_TEST(hmac_sha256_handles_empty_key_and_message) {
    // Neither is a realistic call here, but a wrong length guard would show up
    // as a crash rather than a wrong tag.
    MT_EXPECT_EQ(agentenv::core::HmacSha256Hex("", "").size(), static_cast<std::size_t>(64));
    MT_EXPECT_EQ(agentenv::core::HmacSha256Hex("key", "").size(), static_cast<std::size_t>(64));
    MT_EXPECT_EQ(agentenv::core::HmacSha256Hex("", "message").size(),
                 static_cast<std::size_t>(64));
}

MT_TEST(hmac_sha256_treats_a_64_byte_key_as_exactly_one_block) {
    // The boundary between the zero-pad branch and the hash-the-key branch.
    // Both must leave a 64-byte key untouched.
    const std::string key = RepeatChar('\xaa', 64);
    const std::string direct = agentenv::core::HmacSha256Hex(key, "payload");
    MT_EXPECT_EQ(direct.size(), static_cast<std::size_t>(64));
    // A 65-byte key must produce a different tag (it gets hashed first).
    MT_EXPECT_TRUE(direct != agentenv::core::HmacSha256Hex(RepeatChar('\xaa', 65), "payload"));
}

MT_TEST(constant_time_equals_still_compares_correctly) {
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 4};
    const uint8_t c[4] = {1, 2, 3, 5};
    MT_EXPECT_TRUE(agentenv::core::ConstantTimeEquals(a, b, 4));
    MT_EXPECT_TRUE(!agentenv::core::ConstantTimeEquals(a, c, 4));
    // A difference in the first byte must be caught as reliably as the last.
    const uint8_t d[4] = {0, 2, 3, 4};
    MT_EXPECT_TRUE(!agentenv::core::ConstantTimeEquals(a, d, 4));
    MT_EXPECT_TRUE(agentenv::core::ConstantTimeEquals(a, c, 0));
}

MT_TEST(hex_decode_exact_rejects_wrong_lengths_and_non_hex) {
    uint8_t out[4];
    MT_EXPECT_TRUE(agentenv::core::HexDecodeExact("01ff80aa", out, 4));
    MT_EXPECT_EQ(out[0], static_cast<uint8_t>(0x01));
    MT_EXPECT_EQ(out[1], static_cast<uint8_t>(0xff));
    MT_EXPECT_EQ(out[3], static_cast<uint8_t>(0xaa));

    // Short, long, odd, and non-hex must all fail rather than decode a prefix.
    MT_EXPECT_TRUE(!agentenv::core::HexDecodeExact("01ff80", out, 4));
    MT_EXPECT_TRUE(!agentenv::core::HexDecodeExact("01ff80aabb", out, 4));
    MT_EXPECT_TRUE(!agentenv::core::HexDecodeExact("01ff80a", out, 4));
    MT_EXPECT_TRUE(!agentenv::core::HexDecodeExact("01ff80zz", out, 4));
    MT_EXPECT_TRUE(!agentenv::core::HexDecodeExact("", out, 4));
}

MT_TEST(fill_secure_random_produces_distinct_buffers) {
    uint8_t first[32];
    uint8_t second[32];
    MT_EXPECT_TRUE(agentenv::core::FillSecureRandom(first, sizeof(first)).ok());
    MT_EXPECT_TRUE(agentenv::core::FillSecureRandom(second, sizeof(second)).ok());
    // Not a statistical test; this only catches a stub that returns zeros.
    MT_EXPECT_TRUE(!agentenv::core::ConstantTimeEquals(first, second, sizeof(first)));

    bool any_nonzero = false;
    for (std::size_t i = 0; i < sizeof(first); ++i) {
        if (first[i] != 0) any_nonzero = true;
    }
    MT_EXPECT_TRUE(any_nonzero);
    MT_EXPECT_TRUE(agentenv::core::FillSecureRandom(first, 0).ok());
}

// ---------------------------------------------------------------------------
// Token generation — the cross-language vectors
// ---------------------------------------------------------------------------

MT_TEST(generates_e2b_compatible_access_tokens) {
    // Rust: `generates_e2b_compatible_access_tokens`. These two hex strings
    // are copied verbatim from the Rust test, so they pin the whole chain
    // (seed bytes -> HMAC key -> subject encoding -> hex) against the original
    // implementation. If any step differed, these would not match.
    const agentenv::core::Expected<sandbox::SandboxAccessTokenGenerator, std::string>
        generator = sandbox::SandboxAccessTokenGenerator::New("test-seed");
    MT_EXPECT_TRUE(generator.ok());
    const SandboxId subject = FixtureSandboxId();

    const sandbox::EnvdAccessToken envd_token = generator.value().Generate(subject);
    const std::string traffic_token = generator.value().GenerateTraffic(subject);

    MT_EXPECT_EQ(
        envd_token.Expose(),
        std::string("4f00f2a93a87c37161ae01c59b6d4f84506668113441277e9f6272dd4bfae1a7"));
    MT_EXPECT_EQ(
        traffic_token,
        std::string("586547d7c10facb0f4871297fdbfd9d2b4376f4b02b2e1487646c1c87a293bd8"));

    for (std::size_t i = 0; i < envd_token.Expose().size(); ++i) {
        MT_EXPECT_TRUE(::isxdigit(
                           static_cast<unsigned char>(envd_token.Expose()[i])) != 0);
    }
    MT_EXPECT_TRUE(generator.value().Matches(subject, envd_token.Expose()));
    MT_EXPECT_TRUE(generator.value().MatchesTraffic(subject, traffic_token));
    MT_EXPECT_TRUE(!generator.value().Matches(subject, "not-a-token"));
    MT_EXPECT_TRUE(!generator.value().Matches(subject, RepeatChar('0', 64)));
    // Domain separation: the envd token must not open the traffic path.
    MT_EXPECT_TRUE(!generator.value().MatchesTraffic(subject, envd_token.Expose()));
}

MT_TEST(tokens_differ_per_sandbox_and_per_seed) {
    const sandbox::SandboxAccessTokenGenerator first =
        sandbox::SandboxAccessTokenGenerator::New("seed-a").value();
    const sandbox::SandboxAccessTokenGenerator second =
        sandbox::SandboxAccessTokenGenerator::New("seed-b").value();
    const SandboxId subject = FixtureSandboxId();
    const SandboxId other = SandboxId::Fresh();

    MT_EXPECT_TRUE(first.Generate(subject).Expose() != second.Generate(subject).Expose());
    MT_EXPECT_TRUE(first.Generate(subject).Expose() != first.Generate(other).Expose());
    // A token minted for one sandbox must not authenticate another.
    MT_EXPECT_TRUE(!first.Matches(other, first.Generate(subject).Expose()));
    MT_EXPECT_TRUE(!second.Matches(subject, first.Generate(subject).Expose()));
}

MT_TEST(generation_is_deterministic_for_one_seed) {
    // The property the whole design rests on: a persisted sandbox resumed
    // later, or reached through a different node, presents the same token.
    const sandbox::SandboxAccessTokenGenerator generator =
        sandbox::SandboxAccessTokenGenerator::New("stable-seed").value();
    const SandboxId subject = FixtureSandboxId();
    MT_EXPECT_EQ(generator.Generate(subject).Expose(), generator.Generate(subject).Expose());
    MT_EXPECT_EQ(generator.GenerateTraffic(subject), generator.GenerateTraffic(subject));
}

MT_TEST(matches_rejects_wrong_case_and_wrong_length_hex) {
    const sandbox::SandboxAccessTokenGenerator generator =
        sandbox::SandboxAccessTokenGenerator::New("seed").value();
    const SandboxId subject = FixtureSandboxId();
    const std::string token = generator.Generate(subject).Expose();

    MT_EXPECT_TRUE(generator.Matches(subject, token));
    // Truncated or extended candidates fail on the exact-length decode.
    MT_EXPECT_TRUE(!generator.Matches(subject, token.substr(0, 63)));
    MT_EXPECT_TRUE(!generator.Matches(subject, token + "00"));
    MT_EXPECT_TRUE(!generator.Matches(subject, ""));
}

MT_TEST(rejects_empty_seed_and_redacts_secrets) {
    // Rust: `rejects_empty_seed_and_redacts_secrets`.
    MT_EXPECT_TRUE(!sandbox::SandboxAccessTokenGenerator::New("  ").ok());
    MT_EXPECT_TRUE(!sandbox::SandboxAccessTokenGenerator::New("").ok());
    MT_EXPECT_TRUE(!sandbox::SandboxAccessTokenGenerator::New("\t\n ").ok());

    const sandbox::SandboxAccessTokenGenerator generator =
        sandbox::SandboxAccessTokenGenerator::New("super-secret").value();
    const sandbox::EnvdAccessToken token = generator.Generate(SandboxId());

    // Neither the seed nor the token may appear in a debug rendering; these
    // strings end up in logs.
    MT_EXPECT_TRUE(generator.DebugString().find("super-secret") == std::string::npos);
    MT_EXPECT_TRUE(token.DebugString().find(token.Expose()) == std::string::npos);
}

MT_TEST(seed_is_trimmed_before_use) {
    // `parse_trimmed_string` on the Rust config side means a padded
    // environment variable must derive the same tokens as a clean one.
    const sandbox::SandboxAccessTokenGenerator padded =
        sandbox::SandboxAccessTokenGenerator::New("  test-seed \n").value();
    const sandbox::SandboxAccessTokenGenerator clean =
        sandbox::SandboxAccessTokenGenerator::New("test-seed").value();
    MT_EXPECT_EQ(padded.Generate(FixtureSandboxId()).Expose(),
                 clean.Generate(FixtureSandboxId()).Expose());
}

// ---------------------------------------------------------------------------
// Managed seed lifecycle
// ---------------------------------------------------------------------------

MT_TEST(explicit_seed_takes_precedence_without_creating_managed_state) {
    // Rust: `explicit_seed_takes_precedence_without_creating_managed_state`.
    TempRoot root;
    agentenv::cfg::AppConfig config;
    config.home_path = root.path;
    config.sandbox.access_token_hash_seed =
        Optional<std::string>(std::string("configured-seed"));

    const agentenv::core::Expected<sandbox::SandboxAccessTokenGenerator, std::string>
        generator = sandbox::SandboxAccessTokenGenerator::LoadOrCreate(config, false);
    MT_EXPECT_TRUE(generator.ok());

    const std::vector<uint8_t>& seed = generator.value().SeedForTesting();
    MT_EXPECT_EQ(std::string(seed.begin(), seed.end()), std::string("configured-seed"));
    // No managed file was written: doing so would leave a seed behind that a
    // later unconfigured start would adopt instead of failing.
    MT_EXPECT_TRUE(!fs::Exists(root.Join(sandbox::kManagedSeedRelativePath)));
}

MT_TEST(managed_seed_is_private_and_stable) {
    // Rust: `managed_seed_is_private_and_stable`.
    TempRoot root;
    const std::string managed_path = root.Join(sandbox::kManagedSeedRelativePath);

    const agentenv::core::Expected<std::string, std::string> first =
        sandbox::ResolveSeed(managed_path, false);
    MT_EXPECT_TRUE(first.ok());
    const agentenv::core::Expected<std::string, std::string> second =
        sandbox::ResolveSeed(managed_path, false);
    MT_EXPECT_TRUE(second.ok());

    // The second call must read the first seed, not mint a new one.
    MT_EXPECT_EQ(first.value(), second.value());
    MT_EXPECT_EQ(first.value().size(), static_cast<std::size_t>(64));
    for (std::size_t i = 0; i < first.value().size(); ++i) {
        const char c = first.value()[i];
        MT_EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }

    // The seed is a long-lived credential, so the file and its directory must
    // not be readable by other users.
    const Optional<std::string> parent = fs::Parent(managed_path);
    MT_EXPECT_TRUE(parent.has_value());
    MT_EXPECT_EQ(ModeOf(*parent), static_cast<uint32_t>(0700));
    MT_EXPECT_EQ(ModeOf(managed_path), static_cast<uint32_t>(0600));

    const SandboxId subject = SandboxId::Fresh();
    MT_EXPECT_EQ(
        sandbox::SandboxAccessTokenGenerator::New(first.value()).value().Generate(subject).Expose(),
        sandbox::SandboxAccessTokenGenerator::New(second.value()).value().Generate(subject).Expose());
}

MT_TEST(empty_or_invalid_managed_seed_is_not_replaced) {
    // Rust: `empty_or_invalid_managed_seed_is_not_replaced`. Overwriting a
    // corrupt seed would invalidate every token already issued under it, so
    // the failure must be reported instead of repaired.
    TempRoot root;
    const std::string managed_path = root.Join(sandbox::kManagedSeedRelativePath);
    const Optional<std::string> parent = fs::Parent(managed_path);
    MT_EXPECT_TRUE(parent.has_value());
    CreatePrivateManagedSeedDirectory(*parent);

    const char* const contents[] = {"", "invalid\n", "ABCDEF\n"};
    for (std::size_t i = 0; i < sizeof(contents) / sizeof(contents[0]); ++i) {
        MT_EXPECT_TRUE(fs::Write(managed_path, contents[i]).ok());
        MT_EXPECT_EQ(::chmod(managed_path.c_str(), 0600), 0);

        const agentenv::core::Expected<std::string, std::string> resolved =
            sandbox::ResolveSeed(managed_path, false);
        MT_EXPECT_TRUE(!resolved.ok());
        MT_EXPECT_TRUE(resolved.error().find("64 lowercase hexadecimal") !=
                       std::string::npos);
        // The file is left exactly as it was.
        MT_EXPECT_EQ(fs::ReadToString(managed_path).value(), std::string(contents[i]));
    }
}

MT_TEST(uppercase_managed_seed_is_rejected) {
    // A seed is used as raw ASCII key material, so "AB" and "ab" would derive
    // different tokens; only one spelling may be accepted.
    TempRoot root;
    const std::string managed_path = root.Join(sandbox::kManagedSeedRelativePath);
    const Optional<std::string> parent = fs::Parent(managed_path);
    CreatePrivateManagedSeedDirectory(*parent);
    MT_EXPECT_TRUE(fs::Write(managed_path, RepeatChar('A', 64) + "\n").ok());
    MT_EXPECT_EQ(::chmod(managed_path.c_str(), 0600), 0);

    MT_EXPECT_TRUE(!sandbox::ResolveSeed(managed_path, false).ok());
}

MT_TEST(permissive_managed_seed_is_rejected) {
    // Rust: `permissive_managed_seed_is_rejected`.
    TempRoot root;
    const std::string managed_path = root.Join(sandbox::kManagedSeedRelativePath);
    const Optional<std::string> parent = fs::Parent(managed_path);
    CreatePrivateManagedSeedDirectory(*parent);
    MT_EXPECT_TRUE(fs::Write(managed_path, RepeatChar('a', 64) + "\n").ok());
    MT_EXPECT_EQ(::chmod(managed_path.c_str(), 0640), 0);

    const agentenv::core::Expected<std::string, std::string> resolved =
        sandbox::ResolveSeed(managed_path, false);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_TRUE(resolved.error().find("permissions 0600") != std::string::npos);
}

MT_TEST(managed_seed_symlink_is_rejected) {
    // Rust: `managed_seed_symlink_is_rejected`. The secret is opened
    // O_NOFOLLOW, so a symlink planted in its place cannot redirect the read.
    TempRoot root;
    const std::string managed_path = root.Join(sandbox::kManagedSeedRelativePath);
    const std::string target_path = root.Join("seed-target");
    const Optional<std::string> parent = fs::Parent(managed_path);
    CreatePrivateManagedSeedDirectory(*parent);
    MT_EXPECT_TRUE(fs::Write(target_path, RepeatChar('a', 64) + "\n").ok());
    MT_EXPECT_EQ(::chmod(target_path.c_str(), 0600), 0);
    if (::symlink(target_path.c_str(), managed_path.c_str()) != 0) return;

    const agentenv::core::Expected<std::string, std::string> resolved =
        sandbox::ResolveSeed(managed_path, false);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_TRUE(resolved.error().find("open managed secret") != std::string::npos);
}

MT_TEST(oversized_managed_seed_is_rejected_before_reading_contents) {
    // Rust: `oversized_managed_seed_is_rejected_before_reading_contents`.
    TempRoot root;
    const std::string managed_path = root.Join(sandbox::kManagedSeedRelativePath);
    const Optional<std::string> parent = fs::Parent(managed_path);
    CreatePrivateManagedSeedDirectory(*parent);

    std::FILE* file = std::fopen(managed_path.c_str(), "wb");
    MT_EXPECT_TRUE(file != NULL);
    std::fclose(file);
    MT_EXPECT_EQ(::truncate(managed_path.c_str(), 1024 * 1024), 0);
    MT_EXPECT_EQ(::chmod(managed_path.c_str(), 0600), 0);

    const agentenv::core::Expected<std::string, std::string> resolved =
        sandbox::ResolveSeed(managed_path, false);
    MT_EXPECT_TRUE(!resolved.ok());
    // The size is checked first, so a huge file is never read into memory.
    MT_EXPECT_TRUE(resolved.error().find("must be at most 65 bytes") != std::string::npos);
}

MT_TEST(managed_seed_directory_symlink_is_rejected) {
    // Rust: `managed_seed_directory_symlink_is_rejected`. Without this, a
    // symlinked `secrets/` would let an attacker choose where the seed lands.
    TempRoot root;
    const std::string managed_path = root.Join(sandbox::kManagedSeedRelativePath);
    const std::string target_directory = root.Join("target-secrets");
    const Optional<std::string> parent = fs::Parent(managed_path);
    CreatePrivateManagedSeedDirectory(target_directory);
    if (::symlink(target_directory.c_str(), parent->c_str()) != 0) return;

    const agentenv::core::Expected<std::string, std::string> resolved =
        sandbox::ResolveSeed(managed_path, false);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_TRUE(resolved.error().find("not a symbolic link") != std::string::npos);
}

MT_TEST(missing_managed_seed_is_not_recreated_for_persisted_state) {
    // Rust: `missing_managed_seed_is_not_recreated_for_persisted_state`.
    // Minting a fresh seed here would lock the operator out of every
    // token-protected sandbox already on disk.
    TempRoot root;
    const std::string managed_path = root.Join(sandbox::kManagedSeedRelativePath);

    const agentenv::core::Expected<std::string, std::string> resolved =
        sandbox::ResolveSeed(managed_path, true);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_TRUE(resolved.error().find("persisted sandboxes exist") != std::string::npos);
    MT_EXPECT_TRUE(!fs::Exists(managed_path));
}

MT_TEST(managed_seed_trailing_newline_is_optional) {
    // `create` writes a trailing newline, but an operator-restored file may
    // not have one; both must resolve to the same seed.
    TempRoot root;
    const std::string managed_path = root.Join(sandbox::kManagedSeedRelativePath);
    const Optional<std::string> parent = fs::Parent(managed_path);
    CreatePrivateManagedSeedDirectory(*parent);
    MT_EXPECT_TRUE(fs::Write(managed_path, RepeatChar('a', 64)).ok());
    MT_EXPECT_EQ(::chmod(managed_path.c_str(), 0600), 0);

    const agentenv::core::Expected<std::string, std::string> resolved =
        sandbox::ResolveSeed(managed_path, false);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value(), RepeatChar('a', 64));
}

MT_TEST(load_or_create_mints_a_managed_seed_when_unconfigured) {
    TempRoot root;
    agentenv::cfg::AppConfig config;
    config.home_path = root.path;

    const agentenv::core::Expected<sandbox::SandboxAccessTokenGenerator, std::string> first =
        sandbox::SandboxAccessTokenGenerator::LoadOrCreate(config, false);
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(fs::Exists(root.Join(sandbox::kManagedSeedRelativePath)));

    // A restart must reuse the same seed, otherwise every live token breaks.
    const agentenv::core::Expected<sandbox::SandboxAccessTokenGenerator, std::string> second =
        sandbox::SandboxAccessTokenGenerator::LoadOrCreate(config, false);
    MT_EXPECT_TRUE(second.ok());
    const SandboxId subject = FixtureSandboxId();
    MT_EXPECT_EQ(first.value().Generate(subject).Expose(),
                 second.value().Generate(subject).Expose());
}

int main() { return microtest::RunAll(); }
