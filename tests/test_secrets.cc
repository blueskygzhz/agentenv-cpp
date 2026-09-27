// SPDX-License-Identifier: MIT
// Rust: src/api_key.rs `mod tests` + src/managed_secret.rs `mod tests`.
#include <string>
#include <vector>

#include <unistd.h>

#include "agentenv/api_key.h"
#include "agentenv/core/fs.h"
#include "agentenv/managed_secret.h"
#include "microtest.h"

namespace {

using agentenv::ApiKey;
using agentenv::core::Optional;
using agentenv::core::Unit;
namespace fs = agentenv::core::fs;
namespace secret = agentenv::managed_secret;

/// Rust test constant `TEST_KEY`.
const char* const kTestKey =
    "e2b_0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct TempRoot {
    std::string path;

    TempRoot() {
        const agentenv::core::Expected<std::string, std::string> temp =
            fs::CreateTempDir("agentenv-secret-");
        MT_EXPECT_TRUE(temp.ok());
        path = temp.value();
    }
    ~TempRoot() { fs::RemoveDirAll(path); }

    std::string Join(const std::string& leaf) const { return fs::Join(path, leaf); }
};

std::string Repeat(char c, std::size_t count) { return std::string(count, c); }

}  // namespace

// ---------------------------------------------------------------------------
// ApiKey validation
// ---------------------------------------------------------------------------

MT_TEST(validation_enforces_length_and_url_safe_characters) {
    // Rust: `validation_enforces_length_and_url_safe_characters`.
    MT_EXPECT_TRUE(ApiKey::New(Repeat('a', 32)).ok());
    MT_EXPECT_TRUE(ApiKey::New(Repeat('a', agentenv::kApiKeyMaxLen)).ok());
    MT_EXPECT_TRUE(!ApiKey::New(Repeat('a', 31)).ok());
    MT_EXPECT_TRUE(!ApiKey::New(Repeat('a', agentenv::kApiKeyMaxLen + 1)).ok());
    // 31 safe characters plus one unsafe one: fails on both counts.
    MT_EXPECT_TRUE(!ApiKey::New(Repeat('a', 31) + "!").ok());
}

MT_TEST(validation_accepts_every_url_safe_character) {
    // The URL-safe set is `[A-Za-z0-9._~-]`; all four punctuation marks must
    // be accepted, and anything outside rejected.
    MT_EXPECT_TRUE(ApiKey::New(Repeat('a', 28) + "._~-").ok());

    const char* const rejected[] = {"!", "/", "+", "=", " ", "\n", "\t", "%", "@"};
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!ApiKey::New(Repeat('a', 32) + rejected[i]).ok());
    }
}

MT_TEST(matches_uses_the_validated_key) {
    // Rust: `matches_uses_the_validated_key`.
    const agentenv::core::Expected<ApiKey, std::string> key = ApiKey::New(kTestKey);
    MT_EXPECT_TRUE(key.ok());

    MT_EXPECT_TRUE(key.value().Matches(kTestKey));
    MT_EXPECT_TRUE(!key.value().Matches("wrong-key"));
}

MT_TEST(matches_rejects_same_length_and_prefix_candidates) {
    const agentenv::core::Expected<ApiKey, std::string> key = ApiKey::New(kTestKey);
    MT_EXPECT_TRUE(key.ok());

    // Same length, differs only in the final byte: the constant-time compare
    // must still reject it.
    std::string tampered = kTestKey;
    tampered[tampered.size() - 1] = tampered[tampered.size() - 1] == 'f' ? 'e' : 'f';
    MT_EXPECT_EQ(tampered.size(), key.value().size());
    MT_EXPECT_TRUE(!key.value().Matches(tampered));

    // A correct prefix must not be accepted.
    MT_EXPECT_TRUE(!key.value().Matches(std::string(kTestKey).substr(0, 40)));
    // Nor a longer string that starts with the key.
    MT_EXPECT_TRUE(!key.value().Matches(std::string(kTestKey) + "x"));
    MT_EXPECT_TRUE(!key.value().Matches(""));
}

MT_TEST(debug_output_never_reveals_the_key) {
    const agentenv::core::Expected<ApiKey, std::string> key = ApiKey::New(kTestKey);
    MT_EXPECT_TRUE(key.ok());
    // Rust `impl Debug` writes a fixed redacted string.
    MT_EXPECT_EQ(key.value().ToDebugString(), std::string("ApiKey([REDACTED])"));
    MT_EXPECT_TRUE(key.value().ToDebugString().find("0123456789") == std::string::npos);
}

// ---------------------------------------------------------------------------
// ApiKey resolution
// ---------------------------------------------------------------------------

MT_TEST(configured_sources_take_precedence) {
    // Rust: `configured_sources_take_precedence`.
    TempRoot root;
    const std::string external = root.Join("external");
    MT_EXPECT_TRUE(fs::Write(external, std::string(kTestKey) + "\n").ok());

    // 1. An explicit value wins.
    const agentenv::core::Expected<ApiKey, std::string> explicit_key =
        ApiKey::ResolveFrom(Optional<std::string>(std::string(kTestKey)), external, root.path);
    MT_EXPECT_TRUE(explicit_key.ok());
    MT_EXPECT_TRUE(explicit_key.value().Matches(kTestKey));

    // 2. Otherwise the external file.
    const agentenv::core::Expected<ApiKey, std::string> external_key =
        ApiKey::ResolveFrom(Optional<std::string>(), external, root.path);
    MT_EXPECT_TRUE(external_key.ok());
    MT_EXPECT_TRUE(external_key.value().Matches(kTestKey));

    // Neither path generated a managed secret.
    MT_EXPECT_TRUE(!fs::Exists(root.Join(agentenv::kManagedApiKeyRelativePath)));
}

MT_TEST(external_secret_must_be_a_regular_file) {
    // Rust: `external_secret_must_be_a_regular_file`.
    TempRoot root;
    const std::string external = root.Join("external");
    MT_EXPECT_TRUE(fs::CreateDirAll(external).ok());

    const agentenv::core::Expected<ApiKey, std::string> resolved =
        ApiKey::ResolveFrom(Optional<std::string>(), external, root.path);
    MT_EXPECT_TRUE(!resolved.ok());
    MT_EXPECT_TRUE(resolved.error().find("load external API key") != std::string::npos);
    MT_EXPECT_TRUE(resolved.error().find("must be a regular file") != std::string::npos);
}

MT_TEST(external_secret_accepts_crlf_and_bare_values) {
    TempRoot root;

    // Rust strips one "\n" then one "\r", so LF, CRLF and no terminator all
    // resolve to the same key.
    const char* const variants[] = {"", "\n", "\r\n"};
    for (std::size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        const std::string external = root.Join("external");
        MT_EXPECT_TRUE(fs::Write(external, std::string(kTestKey) + variants[i]).ok());

        const agentenv::core::Expected<ApiKey, std::string> resolved =
            ApiKey::ResolveFrom(Optional<std::string>(), external, root.path);
        MT_EXPECT_TRUE(resolved.ok());
        MT_EXPECT_TRUE(resolved.value().Matches(kTestKey));
    }

    // A trailing space is *not* trimmed, so the key stays invalid.
    const std::string external = root.Join("external");
    MT_EXPECT_TRUE(fs::Write(external, std::string(kTestKey) + " \n").ok());
    MT_EXPECT_TRUE(!ApiKey::ResolveFrom(Optional<std::string>(), external, root.path).ok());
}

MT_TEST(external_secret_allows_kubernetes_style_symlinks) {
    // Rust: `external_secret_allows_kubernetes_style_symlinks`. A projected
    // secret is a symlink, so the external path must stay followable.
    TempRoot root;
    const std::string target = root.Join("target");
    const std::string external = root.Join("external");
    MT_EXPECT_TRUE(fs::Write(target, std::string(kTestKey) + "\n").ok());
    MT_EXPECT_EQ(::symlink(target.c_str(), external.c_str()), 0);

    const agentenv::core::Expected<ApiKey, std::string> resolved =
        ApiKey::ResolveFrom(Optional<std::string>(), external, root.path);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().Matches(kTestKey));
}

MT_TEST(invalid_explicit_value_is_reported_as_such) {
    TempRoot root;
    const agentenv::core::Expected<ApiKey, std::string> resolved = ApiKey::ResolveFrom(
        Optional<std::string>(std::string("too-short")), root.Join("missing"), root.path);
    MT_EXPECT_TRUE(!resolved.ok());
    // The message must name the env var, since that is what the operator set.
    MT_EXPECT_TRUE(resolved.error().find("invalid AENV_API_KEY") != std::string::npos);
}

MT_TEST(managed_key_is_private_and_stable) {
    // Rust: `managed_key_is_private_and_stable`.
    TempRoot root;
    const std::string missing_external = root.Join("missing");

    const agentenv::core::Expected<ApiKey, std::string> first =
        ApiKey::ResolveFrom(Optional<std::string>(), missing_external, root.path);
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(first.value().HasGeneratedPrefix());

    // A second resolve must adopt the stored key, not mint a new one.
    const agentenv::core::Expected<ApiKey, std::string> second =
        ApiKey::ResolveFrom(Optional<std::string>(), missing_external, root.path);
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_EQ(first.value().size(), second.value().size());

    // Permissions are the security property: 0700 dir, 0600 file.
    const std::string key_path = root.Join(agentenv::kManagedApiKeyRelativePath);
    const Optional<std::string> parent = fs::Parent(key_path);
    MT_EXPECT_TRUE(parent.has_value());
    MT_EXPECT_EQ(fs::Stat(*parent).value().mode, static_cast<uint32_t>(0700));
    MT_EXPECT_EQ(fs::Stat(key_path).value().mode, static_cast<uint32_t>(0600));
}

MT_TEST(generated_key_is_long_and_url_safe) {
    TempRoot root;
    const agentenv::core::Expected<ApiKey, std::string> key =
        ApiKey::ResolveFrom(Optional<std::string>(), root.Join("missing"), root.path);
    MT_EXPECT_TRUE(key.ok());

    // "e2b_" + 32 random bytes hex-encoded = 4 + 64.
    MT_EXPECT_EQ(key.value().size(), static_cast<std::size_t>(68));

    // Re-reading the file must yield a value that still validates.
    const agentenv::core::Expected<std::string, std::string> stored =
        fs::ReadToString(root.Join(agentenv::kManagedApiKeyRelativePath));
    MT_EXPECT_TRUE(stored.ok());
    // Stored with a trailing newline.
    MT_EXPECT_EQ(stored.value()[stored.value().size() - 1], '\n');
    MT_EXPECT_TRUE(key.value().Matches(stored.value().substr(0, stored.value().size() - 1)));
}

MT_TEST(two_generated_keys_differ) {
    // A fixed or weakly-seeded generator would be a critical flaw, so two
    // independent stores must not produce the same key.
    TempRoot first_root;
    TempRoot second_root;

    const agentenv::core::Expected<ApiKey, std::string> first = ApiKey::ResolveFrom(
        Optional<std::string>(), first_root.Join("missing"), first_root.path);
    const agentenv::core::Expected<ApiKey, std::string> second = ApiKey::ResolveFrom(
        Optional<std::string>(), second_root.Join("missing"), second_root.path);
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(second.ok());

    const agentenv::core::Expected<std::string, std::string> first_bytes =
        fs::ReadToString(first_root.Join(agentenv::kManagedApiKeyRelativePath));
    MT_EXPECT_TRUE(first_bytes.ok());
    MT_EXPECT_TRUE(!second.value().Matches(
        first_bytes.value().substr(0, first_bytes.value().size() - 1)));
}

// ---------------------------------------------------------------------------
// managed_secret
// ---------------------------------------------------------------------------

MT_TEST(creation_tightens_an_empty_volume_directory) {
    // Rust: `creation_tightens_an_empty_volume_directory`.
    TempRoot root;
    const std::string directory = root.Join("secrets");
    const std::string path = fs::Join(directory, "api-key");

    // A mounted volume often arrives as 0755; creation must tighten it.
    MT_EXPECT_TRUE(fs::CreateDirAll(directory).ok());
    MT_EXPECT_TRUE(fs::SetPermissions(directory, 0755).ok());

    const agentenv::core::Expected<Optional<std::string>, std::string> before =
        secret::Read(path, 64);
    MT_EXPECT_TRUE(before.ok());
    MT_EXPECT_TRUE(!before.value().has_value());

    const agentenv::core::Expected<secret::CreateOutcome, std::string> created =
        secret::Create(path, "secret");
    MT_EXPECT_TRUE(created.ok());
    MT_EXPECT_TRUE(created.value().created());
    MT_EXPECT_EQ(fs::Stat(directory).value().mode, static_cast<uint32_t>(0700));
}

MT_TEST(creation_rejects_a_non_dedicated_parent) {
    // Rust: `creation_rejects_a_non_dedicated_parent`. The parent must be
    // named `secrets`, so an arbitrary directory cannot be tightened to 0700.
    TempRoot root;
    MT_EXPECT_TRUE(fs::SetPermissions(root.path, 0750).ok());

    const agentenv::core::Expected<secret::CreateOutcome, std::string> created =
        secret::Create(root.Join("api-key"), "secret");
    MT_EXPECT_TRUE(!created.ok());
    MT_EXPECT_TRUE(created.error().find("dedicated directory named secrets") !=
                   std::string::npos);
    // Crucially, the rejection left the directory's mode alone.
    MT_EXPECT_EQ(fs::Stat(root.path).value().mode, static_cast<uint32_t>(0750));
}

MT_TEST(read_round_trips_a_created_secret) {
    TempRoot root;
    const std::string path = fs::Join(root.Join("secrets"), "api-key");

    MT_EXPECT_TRUE(secret::Create(path, "hunter2\n").ok());

    const agentenv::core::Expected<Optional<std::string>, std::string> read =
        secret::Read(path, 64);
    MT_EXPECT_TRUE(read.ok());
    MT_EXPECT_TRUE(read.value().has_value());
    MT_EXPECT_EQ(*read.value(), std::string("hunter2\n"));
}

MT_TEST(read_rejects_a_loose_file_mode) {
    TempRoot root;
    const std::string path = fs::Join(root.Join("secrets"), "api-key");
    MT_EXPECT_TRUE(secret::Create(path, "secret").ok());

    // A world-readable secret must be refused rather than silently used.
    MT_EXPECT_TRUE(fs::SetPermissions(path, 0644).ok());
    const agentenv::core::Expected<Optional<std::string>, std::string> read =
        secret::Read(path, 64);
    MT_EXPECT_TRUE(!read.ok());
    MT_EXPECT_TRUE(read.error().find("must have permissions 0600") != std::string::npos);
}

MT_TEST(read_rejects_a_loose_directory_mode) {
    TempRoot root;
    const std::string directory = root.Join("secrets");
    const std::string path = fs::Join(directory, "api-key");
    MT_EXPECT_TRUE(secret::Create(path, "secret").ok());

    // Loosening the directory after the fact must also be caught: another
    // user could otherwise replace the file.
    MT_EXPECT_TRUE(fs::SetPermissions(directory, 0755).ok());
    const agentenv::core::Expected<Optional<std::string>, std::string> read =
        secret::Read(path, 64);
    MT_EXPECT_TRUE(!read.ok());
    MT_EXPECT_TRUE(read.error().find("must have permissions 0700") != std::string::npos);
}

MT_TEST(read_rejects_a_symlinked_secret) {
    TempRoot root;
    const std::string directory = root.Join("secrets");
    MT_EXPECT_TRUE(fs::CreateDirAllWithMode(directory, 0700).ok());

    const std::string target = root.Join("elsewhere");
    MT_EXPECT_TRUE(fs::Write(target, "secret").ok());
    MT_EXPECT_TRUE(fs::SetPermissions(target, 0600).ok());

    const std::string path = fs::Join(directory, "api-key");
    MT_EXPECT_EQ(::symlink(target.c_str(), path.c_str()), 0);

    // O_NOFOLLOW makes the kernel reject it (ELOOP); our own secret must never
    // be a symlink, unlike the external one.
    const agentenv::core::Expected<Optional<std::string>, std::string> read =
        secret::Read(path, 64);
    MT_EXPECT_TRUE(!read.ok());
}

MT_TEST(read_enforces_the_size_cap) {
    TempRoot root;
    const std::string path = fs::Join(root.Join("secrets"), "api-key");
    MT_EXPECT_TRUE(secret::Create(path, std::string(100, 'x')).ok());

    MT_EXPECT_TRUE(secret::Read(path, 100).ok());
    const agentenv::core::Expected<Optional<std::string>, std::string> too_big =
        secret::Read(path, 99);
    MT_EXPECT_TRUE(!too_big.ok());
    MT_EXPECT_TRUE(too_big.error().find("at most 99 bytes") != std::string::npos);
}

MT_TEST(create_reports_an_existing_secret_instead_of_overwriting) {
    TempRoot root;
    const std::string path = fs::Join(root.Join("secrets"), "api-key");

    MT_EXPECT_TRUE(secret::Create(path, "first").ok());

    // The second creation must lose the race and hand back the winner's file.
    const agentenv::core::Expected<secret::CreateOutcome, std::string> second =
        secret::Create(path, "second");
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_TRUE(!second.value().created());
    MT_EXPECT_TRUE(second.value().existing.valid());

    const agentenv::core::Expected<std::string, std::string> contents =
        secret::ReadFile(path, second.value().existing.get(), 64);
    MT_EXPECT_TRUE(contents.ok());
    MT_EXPECT_EQ(contents.value(), std::string("first"));

    // And the original bytes are untouched on disk.
    MT_EXPECT_EQ(fs::ReadToString(path).value(), std::string("first"));
}

MT_TEST(create_leaves_no_temporary_files_behind) {
    TempRoot root;
    const std::string directory = root.Join("secrets");
    const std::string path = fs::Join(directory, "api-key");

    MT_EXPECT_TRUE(secret::Create(path, "first").ok());
    MT_EXPECT_TRUE(secret::Create(path, "second").ok());  // loses the race

    const agentenv::core::Expected<std::vector<std::string>, std::string> entries =
        fs::ReadDir(directory);
    MT_EXPECT_TRUE(entries.ok());
    // Only the secret itself: the losing attempt cleaned up its temporary.
    MT_EXPECT_EQ(entries.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(entries.value()[0], std::string("api-key"));
}

int main() { return microtest::RunAll(); }
