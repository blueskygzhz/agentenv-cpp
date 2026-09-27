// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/p2p/{artifact,cache,facade}.rs test modules, plus the
// `core::Url` and `core::DescribeFile` helpers this port needed to add.
#include <string>
#include <vector>

#include <unistd.h>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/url.h"
#include "agentenv/overlaybd/p2p/artifact.h"
#include "agentenv/overlaybd/p2p/cache.h"
#include "agentenv/overlaybd/p2p/facade.h"
#include "agentenv/overlaybd/p2p/runtime.h"
#include "agentenv/p2p/mock.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::Unit;
namespace fs = agentenv::core::fs;
namespace obp = agentenv::overlaybd::p2p;
namespace p2p = agentenv::p2p;

const char* const kDigestA =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

struct TempRoot {
    std::string path;

    TempRoot() {
        const agentenv::core::Expected<std::string, std::string> temp =
            fs::CreateTempDir("agentenv-obp2p-");
        MT_EXPECT_TRUE(temp.ok());
        path = temp.value();
    }
    ~TempRoot() { fs::RemoveDirAll(path); }

    std::string Join(const std::string& leaf) const { return fs::Join(path, leaf); }

    void WriteFile(const std::string& relative, const std::string& content) const {
        const std::string full = fs::Join(path, relative);
        const Optional<std::string> parent = fs::Parent(full);
        MT_EXPECT_TRUE(parent.has_value());
        MT_EXPECT_TRUE(fs::CreateDirAll(*parent).ok());
        MT_EXPECT_TRUE(fs::Write(full, content).ok());
    }
};

obp::HeaderMap RangeHeaders(const std::string& range) {
    obp::HeaderMap headers;
    headers.Set("Range", range);
    return headers;
}

/// Test clock for the descriptor cache, advanced explicitly.
uint64_t g_now_ms = 0;
uint64_t TestClock() { return g_now_ms; }

p2p::P2pArtifactDescriptor DescriptorWith(const std::string& key,
                                          const std::string& metadata_json) {
    p2p::P2pArtifactDescriptor descriptor;
    descriptor.key = key;
    descriptor.metadata_json = metadata_json;
    descriptor.providers.push_back(p2p::P2pArtifactProvider::MakeLocal());
    return descriptor;
}

}  // namespace

// ---------------------------------------------------------------------------
// core::Url
// ---------------------------------------------------------------------------

MT_TEST(url_parses_scheme_host_port_path_query) {
    const agentenv::core::Expected<agentenv::core::Url, std::string> url =
        agentenv::core::Url::Parse("https://Example.COM:8443/a/b?sig=one");
    MT_EXPECT_TRUE(url.ok());
    // Scheme and host are lowercased; the path and query are not.
    MT_EXPECT_EQ(url.value().scheme, std::string("https"));
    MT_EXPECT_EQ(url.value().host, std::string("example.com"));
    MT_EXPECT_EQ(*url.value().port, static_cast<uint16_t>(8443));
    MT_EXPECT_EQ(url.value().path, std::string("/a/b"));
    MT_EXPECT_EQ(*url.value().query, std::string("sig=one"));
    MT_EXPECT_EQ(url.value().Authority(), std::string("example.com:8443"));
}

MT_TEST(url_defaults_path_and_omits_absent_port) {
    const agentenv::core::Expected<agentenv::core::Url, std::string> url =
        agentenv::core::Url::Parse("http://example.com");
    MT_EXPECT_TRUE(url.ok());
    MT_EXPECT_EQ(url.value().path, std::string("/"));
    MT_EXPECT_TRUE(!url.value().port.has_value());
    MT_EXPECT_TRUE(!url.value().query.has_value());
    MT_EXPECT_EQ(url.value().Authority(), std::string("example.com"));
}

MT_TEST(url_handles_ipv6_literals) {
    // The port separator must be found after the closing bracket, not at the
    // first colon inside the address.
    const agentenv::core::Expected<agentenv::core::Url, std::string> url =
        agentenv::core::Url::Parse("http://[::1]:9731/accelerator");
    MT_EXPECT_TRUE(url.ok());
    MT_EXPECT_EQ(url.value().host, std::string("[::1]"));
    MT_EXPECT_EQ(*url.value().port, static_cast<uint16_t>(9731));
    MT_EXPECT_EQ(url.value().path, std::string("/accelerator"));
}

MT_TEST(url_rejects_ambiguous_or_unsupported_forms) {
    MT_EXPECT_TRUE(!agentenv::core::Url::Parse("").ok());
    MT_EXPECT_TRUE(!agentenv::core::Url::Parse("/just/a/path").ok());
    MT_EXPECT_TRUE(!agentenv::core::Url::Parse("http://").ok());
    // Userinfo and fragments would give one blob two canonical keys.
    MT_EXPECT_TRUE(!agentenv::core::Url::Parse("http://user:pw@example.com/a").ok());
    MT_EXPECT_TRUE(!agentenv::core::Url::Parse("http://example.com/a#frag").ok());
    MT_EXPECT_TRUE(!agentenv::core::Url::Parse("http://example.com:99999/a").ok());
    MT_EXPECT_TRUE(!agentenv::core::Url::Parse("http://example.com:/a").ok());
    MT_EXPECT_TRUE(!agentenv::core::Url::Parse("1http://example.com/a").ok());
}

// ---------------------------------------------------------------------------
// core::DescribeFile
// ---------------------------------------------------------------------------

MT_TEST(describe_file_reports_stable_digest) {
    // Rust: `describe_file_reports_stable_digest`, same fixture bytes.
    TempRoot root;
    root.WriteFile("artifact.bin", "artifact");

    const agentenv::core::Expected<agentenv::core::FileDigest, std::string> described =
        agentenv::core::DescribeFile(root.Join("artifact.bin"));
    MT_EXPECT_TRUE(described.ok());
    MT_EXPECT_EQ(described.value().size, static_cast<uint64_t>(8));
    MT_EXPECT_EQ(described.value().sha256,
                 std::string("sha256:c7c5c1d70c5dec4416ab6158afd0b223ef40c29b1dc1f97ed9428b94"
                             "d4cadb1c"));
}

MT_TEST(describe_file_handles_empty_and_large_files) {
    TempRoot root;
    root.WriteFile("empty.bin", "");
    const agentenv::core::Expected<agentenv::core::FileDigest, std::string> empty =
        agentenv::core::DescribeFile(root.Join("empty.bin"));
    MT_EXPECT_TRUE(empty.ok());
    MT_EXPECT_EQ(empty.value().size, static_cast<uint64_t>(0));
    // SHA-256 of zero bytes.
    MT_EXPECT_EQ(empty.value().sha256,
                 std::string("sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b"
                             "7852b855"));

    // Larger than the 128 KiB streaming buffer, so the multi-chunk path runs.
    const std::string large(300 * 1024, 'x');
    root.WriteFile("large.bin", large);
    const agentenv::core::Expected<agentenv::core::FileDigest, std::string> described =
        agentenv::core::DescribeFile(root.Join("large.bin"));
    MT_EXPECT_TRUE(described.ok());
    MT_EXPECT_EQ(described.value().size, static_cast<uint64_t>(300 * 1024));
    // Must agree with the in-memory helper over the same bytes.
    MT_EXPECT_EQ(described.value().sha256,
                 std::string("sha256:") + agentenv::core::Sha256Hex(large));
}

MT_TEST(describe_file_reports_a_missing_file) {
    TempRoot root;
    MT_EXPECT_TRUE(!agentenv::core::DescribeFile(root.Join("nope")).ok());
}

// ---------------------------------------------------------------------------
// artifact — canonical identity
// ---------------------------------------------------------------------------

MT_TEST(canonical_key_ignores_presigned_query_and_prefers_digest) {
    // Rust: `canonical_key_ignores_presigned_query_and_prefers_digest`.
    // This is the whole point of canonicalization: two nodes see different
    // presigned URLs for one blob and must still share it.
    const std::string url_a =
        std::string("https://example.com/v2/ns/repo/blobs/") + kDigestA +
        "?X-Amz-Signature=one";
    const std::string url_b =
        std::string("https://example.com/v2/ns/repo/blobs/") + kDigestA +
        "?X-Amz-Signature=two";

    const agentenv::core::Expected<obp::CanonicalBlobIdentity, std::string> a =
        obp::CanonicalBlobIdentity::FromOriginUrl(url_a);
    const agentenv::core::Expected<obp::CanonicalBlobIdentity, std::string> b =
        obp::CanonicalBlobIdentity::FromOriginUrl(url_b);
    MT_EXPECT_TRUE(a.ok());
    MT_EXPECT_TRUE(b.ok());
    MT_EXPECT_TRUE(a.value() == b.value());

    MT_EXPECT_EQ(obp::LayerArtifactKey(a.value()),
                 std::string("overlaybd-layer/v1/") + kDigestA);
}

MT_TEST(canonical_key_falls_back_to_scheme_host_path_without_query) {
    // Rust: `canonical_key_falls_back_to_scheme_host_path_without_query`.
    const std::string url_a = "https://example.com:8443/signed/object/path?sig=one";
    const std::string url_b = "https://example.com:8443/signed/object/path?sig=two";

    const agentenv::core::Expected<obp::CanonicalBlobIdentity, std::string> a =
        obp::CanonicalBlobIdentity::FromOriginUrl(url_a);
    MT_EXPECT_TRUE(a.ok());
    MT_EXPECT_TRUE(a.value() == obp::CanonicalBlobIdentity::FromOriginUrl(url_b).value());
    MT_EXPECT_EQ(a.value().key,
                 std::string("url/https://example.com:8443/signed/object/path"));
    // No digest is available in this form.
    MT_EXPECT_TRUE(!a.value().digest.has_value());
}

MT_TEST(canonical_key_requires_digest_boundary) {
    // Rust: `canonical_key_requires_digest_boundary`. 66 hex digits must not
    // be truncated into a plausible 64-digit digest, or two distinct blobs
    // would collide onto one catalog key.
    const std::string url =
        std::string("https://example.com/v2/ns/repo/blobs/") + kDigestA + "ff";
    const agentenv::core::Expected<obp::CanonicalBlobIdentity, std::string> identity =
        obp::CanonicalBlobIdentity::FromOriginUrl(url);
    MT_EXPECT_TRUE(identity.ok());
    MT_EXPECT_TRUE(!identity.value().digest.has_value());
    MT_EXPECT_EQ(identity.value().key,
                 std::string("url/https://example.com/v2/ns/repo/blobs/") + kDigestA + "ff");
}

MT_TEST(extract_sha256_digest_accepts_every_boundary_character) {
    // Each of these terminates a path segment or starts a query/fragment.
    const char* const boundaries[] = {"/", "?", "&", "#", ":", "@", ",", ";"};
    for (std::size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        const std::string path = std::string("/blobs/") + kDigestA + boundaries[i] + "rest";
        const Optional<std::string> digest = obp::ExtractSha256Digest(path);
        MT_EXPECT_TRUE(digest.has_value());
        MT_EXPECT_EQ(*digest, std::string(kDigestA));
    }

    // End of string is also a valid boundary.
    MT_EXPECT_TRUE(obp::ExtractSha256Digest(std::string("/blobs/") + kDigestA).has_value());
    // Too short, and no marker at all.
    MT_EXPECT_TRUE(!obp::ExtractSha256Digest("/blobs/sha256:abcd").has_value());
    MT_EXPECT_TRUE(!obp::ExtractSha256Digest("/blobs/plain").has_value());
}

MT_TEST(canonical_digest_is_lowercased) {
    // Casing must not fork the key space.
    const std::string upper =
        "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    MT_EXPECT_EQ(obp::CanonicalBlobIdentity::FromDigest(upper).key,
                 std::string("digest/") + kDigestA);
    MT_EXPECT_EQ(obp::LayerKeyFromDigest(upper), obp::LayerKeyFromDigest(kDigestA));
}

MT_TEST(uuid_key_does_not_overlap_digest_key) {
    // Rust: `uuid_key_does_not_overlap_digest_key`.
    MT_EXPECT_EQ(obp::LayerKeyFromUuid("11111111-2222-3333-4444-555555555555"),
                 std::string("overlaybd-layer/v1/uuid/11111111-2222-3333-4444-555555555555"));
    MT_EXPECT_EQ(
        obp::LayerKeyFromDigest(
            "sha256:1111111122223333444455555555555566666666777788889999aaaabbbbcccc"),
        std::string("overlaybd-layer/v1/"
                    "sha256:1111111122223333444455555555555566666666777788889999aaaabbbbcccc"));
}

MT_TEST(url_artifact_key_is_hashed_to_a_fixed_length) {
    const agentenv::core::Expected<obp::CanonicalBlobIdentity, std::string> identity =
        obp::CanonicalBlobIdentity::FromOriginUrl("https://example.com/signed/path?sig=x");
    MT_EXPECT_TRUE(identity.ok());

    const p2p::P2pArtifactKey key = obp::LayerArtifactKey(identity.value());
    // Hashed, so the key length is bounded and the path is not leaked.
    MT_EXPECT_EQ(key, std::string("overlaybd-layer/v1/url/") +
                          agentenv::core::Sha256Hex(identity.value().key));
    MT_EXPECT_TRUE(key.find("/signed/path") == std::string::npos);
}

// ---------------------------------------------------------------------------
// artifact — LayerMetadata
// ---------------------------------------------------------------------------

MT_TEST(uuid_metadata_validates_canonical_uuid) {
    // Rust: `uuid_metadata_validates_canonical_uuid`.
    const std::string uuid = "11111111-2222-3333-4444-555555555555";
    const obp::LayerMetadata metadata =
        obp::LayerMetadata::FromUuid(uuid, Optional<uint64_t>(static_cast<uint64_t>(1234)));
    MT_EXPECT_TRUE(metadata.Validate(obp::CanonicalBlobIdentity::FromUuid(uuid)).ok());
}

MT_TEST(metadata_rejects_a_mismatched_uuid) {
    const obp::LayerMetadata metadata = obp::LayerMetadata::FromUuid(
        "11111111-2222-3333-4444-555555555555", Optional<uint64_t>());
    const agentenv::core::Expected<Unit, std::string> validated = metadata.Validate(
        obp::CanonicalBlobIdentity::FromUuid("99999999-2222-3333-4444-555555555555"));
    MT_EXPECT_TRUE(!validated.ok());
    // The canonical key differs first, so that is the reported reason.
    MT_EXPECT_TRUE(validated.error().find("canonical key mismatch") != std::string::npos);
}

MT_TEST(metadata_rejects_a_foreign_protocol) {
    obp::LayerMetadata metadata = obp::LayerMetadata::FromDigest(
        kDigestA, Optional<uint64_t>(), Optional<std::string>());
    metadata.protocol = "some-other-protocol-v9";

    const agentenv::core::Expected<Unit, std::string> validated =
        metadata.Validate(obp::CanonicalBlobIdentity::FromDigest(kDigestA));
    MT_EXPECT_TRUE(!validated.ok());
    MT_EXPECT_TRUE(validated.error().find("unexpected protocol") != std::string::npos);
}

MT_TEST(metadata_rejects_a_digest_mismatch) {
    const std::string other =
        "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    obp::LayerMetadata metadata = obp::LayerMetadata::FromDigest(
        kDigestA, Optional<uint64_t>(), Optional<std::string>());
    // Same canonical key, different digest field: a hand-crafted descriptor.
    metadata.digest = other;

    const agentenv::core::Expected<Unit, std::string> validated =
        metadata.Validate(obp::CanonicalBlobIdentity::FromDigest(kDigestA));
    MT_EXPECT_TRUE(!validated.ok());
    MT_EXPECT_TRUE(validated.error().find("digest mismatch") != std::string::npos);
}

MT_TEST(metadata_json_round_trips) {
    const obp::LayerMetadata metadata = obp::LayerMetadata::FromDigest(
        kDigestA, Optional<uint64_t>(static_cast<uint64_t>(4096)),
        Optional<std::string>(std::string("abc123")));

    const agentenv::core::Expected<obp::LayerMetadata, std::string> parsed =
        obp::LayerMetadata::ParseJson(metadata.ToJson());
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value() == metadata);
    MT_EXPECT_EQ(*parsed.value().size, static_cast<uint64_t>(4096));
    MT_EXPECT_EQ(*parsed.value().source_url_hash, std::string("abc123"));
    // A UUID-less digest form keeps uuid absent, not empty-string.
    MT_EXPECT_TRUE(!parsed.value().uuid.has_value());
}

MT_TEST(metadata_parse_rejects_incomplete_objects) {
    // Without protocol/canonical_key nothing can be validated, so a partial
    // object must not parse into something that looks usable.
    MT_EXPECT_TRUE(!obp::LayerMetadata::ParseJson("{}").ok());
    MT_EXPECT_TRUE(!obp::LayerMetadata::ParseJson("{\"protocol\":\"x\"}").ok());
    MT_EXPECT_TRUE(!obp::LayerMetadata::ParseJson("not json").ok());
    MT_EXPECT_TRUE(!obp::LayerMetadata::ParseJson("[]").ok());
}

MT_TEST(metadata_from_descriptor_reads_the_descriptor_payload) {
    const obp::LayerMetadata metadata = obp::LayerMetadata::FromDigest(
        kDigestA, Optional<uint64_t>(static_cast<uint64_t>(10)), Optional<std::string>());
    const p2p::P2pArtifactDescriptor descriptor =
        DescriptorWith(obp::LayerKeyFromDigest(kDigestA), metadata.ToJson());

    const agentenv::core::Expected<obp::LayerMetadata, std::string> parsed =
        obp::LayerMetadata::FromDescriptor(descriptor);
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value() == metadata);

    // A descriptor with no metadata at all (the "null" default) must fail.
    p2p::P2pArtifactDescriptor bare;
    bare.key = "k";
    MT_EXPECT_TRUE(!obp::LayerMetadata::FromDescriptor(bare).ok());
}

// ---------------------------------------------------------------------------
// DescriptorCache
// ---------------------------------------------------------------------------

MT_TEST(descriptor_cache_distinguishes_miss_from_uncached) {
    obp::DescriptorCache cache(60000, 5000, 16);
    g_now_ms = 0;
    cache.SetClockForTesting(&TestClock);

    // Nothing cached: outer optional empty.
    MT_EXPECT_TRUE(!cache.Get("k").has_value());

    // A cached miss: outer present, inner empty. This is the distinction that
    // lets the facade skip a repeat lookup for an absent layer.
    cache.Insert("k", Optional<p2p::P2pArtifactDescriptor>());
    const Optional<Optional<p2p::P2pArtifactDescriptor> > cached = cache.Get("k");
    MT_EXPECT_TRUE(cached.has_value());
    MT_EXPECT_TRUE(!cached->has_value());
}

MT_TEST(descriptor_cache_uses_a_shorter_ttl_for_misses) {
    obp::DescriptorCache cache(60000, 5000, 16);
    g_now_ms = 0;
    cache.SetClockForTesting(&TestClock);

    cache.Insert("hit", Optional<p2p::P2pArtifactDescriptor>(DescriptorWith("hit", "{}")));
    cache.Insert("miss", Optional<p2p::P2pArtifactDescriptor>());

    // Past the miss TTL but well inside the hit TTL: a layer that is absent
    // now may be published soon, so misses expire much sooner.
    g_now_ms = 6000;
    MT_EXPECT_TRUE(cache.Get("hit").has_value());
    MT_EXPECT_TRUE(!cache.Get("miss").has_value());

    // Past the hit TTL too.
    g_now_ms = 61000;
    MT_EXPECT_TRUE(!cache.Get("hit").has_value());
}

MT_TEST(descriptor_cache_expires_exactly_at_the_ttl_boundary) {
    obp::DescriptorCache cache(1000, 1000, 16);
    g_now_ms = 0;
    cache.SetClockForTesting(&TestClock);
    cache.Insert("k", Optional<p2p::P2pArtifactDescriptor>());

    // Rust uses `elapsed() <= ttl`, so the boundary itself is still fresh.
    g_now_ms = 1000;
    MT_EXPECT_TRUE(cache.Get("k").has_value());
    g_now_ms = 1001;
    MT_EXPECT_TRUE(!cache.Get("k").has_value());
}

MT_TEST(descriptor_cache_prunes_old_entries_at_max_size) {
    // Rust: `descriptor_cache_prunes_old_entries_at_max_size`.
    obp::DescriptorCache cache(60000, 60000, 2);
    g_now_ms = 0;
    cache.SetClockForTesting(&TestClock);

    cache.Insert("old-a", Optional<p2p::P2pArtifactDescriptor>());
    cache.Insert("old-b", Optional<p2p::P2pArtifactDescriptor>());
    cache.Insert("new-c", Optional<p2p::P2pArtifactDescriptor>());

    // Oldest-first eviction. All three share a millisecond here, which is why
    // the port tracks an insertion sequence rather than relying on the clock.
    MT_EXPECT_TRUE(!cache.Get("old-a").has_value());
    MT_EXPECT_TRUE(cache.Get("old-b").has_value());
    MT_EXPECT_TRUE(cache.Get("new-c").has_value());
}

MT_TEST(descriptor_cache_prefers_evicting_expired_entries) {
    obp::DescriptorCache cache(1000, 1000, 2);
    g_now_ms = 0;
    cache.SetClockForTesting(&TestClock);

    cache.Insert("stale", Optional<p2p::P2pArtifactDescriptor>());
    g_now_ms = 5000;  // "stale" is now expired
    cache.Insert("fresh-a", Optional<p2p::P2pArtifactDescriptor>());
    cache.Insert("fresh-b", Optional<p2p::P2pArtifactDescriptor>());

    // The expired entry is dropped instead of a live one, which would have
    // cost a fresh lookup.
    MT_EXPECT_TRUE(!cache.Get("stale").has_value());
    MT_EXPECT_TRUE(cache.Get("fresh-a").has_value());
    MT_EXPECT_TRUE(cache.Get("fresh-b").has_value());
}

MT_TEST(descriptor_cache_with_zero_max_entries_caches_nothing) {
    obp::DescriptorCache cache(60000, 60000, 0);
    cache.Insert("k", Optional<p2p::P2pArtifactDescriptor>());
    MT_EXPECT_EQ(cache.Size(), static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(!cache.Get("k").has_value());
}

MT_TEST(descriptor_cache_remove_makes_a_publish_visible_immediately) {
    obp::DescriptorCache cache(60000, 60000, 16);
    cache.Insert("k", Optional<p2p::P2pArtifactDescriptor>());
    MT_EXPECT_TRUE(cache.Get("k").has_value());

    // Without this, a freshly published layer would stay invisible until the
    // miss TTL elapsed.
    cache.Remove("k");
    MT_EXPECT_TRUE(!cache.Get("k").has_value());
}

// ---------------------------------------------------------------------------
// facade — range parsing
// ---------------------------------------------------------------------------

MT_TEST(parse_single_range_accepts_a_closed_byte_range) {
    const agentenv::core::Expected<obp::RequestRange, obp::HttpFacadeError> range =
        obp::ParseSingleRange(RangeHeaders("bytes=10-19"));
    MT_EXPECT_TRUE(range.ok());
    MT_EXPECT_EQ(range.value().start, static_cast<uint64_t>(10));
    MT_EXPECT_EQ(range.value().end, static_cast<uint64_t>(19));
    // Inclusive end, so this is ten bytes.
    MT_EXPECT_EQ(range.value().Len().value(), static_cast<std::size_t>(10));

    // A single byte.
    MT_EXPECT_EQ(obp::ParseSingleRange(RangeHeaders("bytes=0-0")).value().Len().value(),
                 static_cast<std::size_t>(1));
}

MT_TEST(parse_single_range_rejects_unsupported_forms) {
    struct Case {
        const char* header;
        int status;
    };
    const Case cases[] = {
        // Multi-range would need a multipart body.
        {"bytes=0-1,5-6", obp::kStatusRangeNotSatisfiable},
        // Open-ended forms need a total size the facade may not know.
        {"bytes=5-", obp::kStatusRangeNotSatisfiable},
        {"bytes=-5", obp::kStatusRangeNotSatisfiable},
        {"items=0-1", obp::kStatusRangeNotSatisfiable},
        {"bytes=abc-def", obp::kStatusRangeNotSatisfiable},
        {"bytes=10-5", obp::kStatusRangeNotSatisfiable},
        {"bytes=0", obp::kStatusRangeNotSatisfiable},
        // A signed value must not sneak through as a negative offset.
        {"bytes=-1--1", obp::kStatusRangeNotSatisfiable},
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const agentenv::core::Expected<obp::RequestRange, obp::HttpFacadeError> range =
            obp::ParseSingleRange(RangeHeaders(cases[i].header));
        MT_EXPECT_TRUE(!range.ok());
        MT_EXPECT_EQ(range.error().status, cases[i].status);
    }
}

MT_TEST(parse_single_range_requires_the_header) {
    const agentenv::core::Expected<obp::RequestRange, obp::HttpFacadeError> range =
        obp::ParseSingleRange(obp::HeaderMap());
    MT_EXPECT_TRUE(!range.ok());
    // Missing entirely is a bad request, not an unsatisfiable range.
    MT_EXPECT_EQ(range.error().status, obp::kStatusBadRequest);
    MT_EXPECT_TRUE(range.error().message.find("missing Range header") != std::string::npos);
}

MT_TEST(request_range_len_rejects_an_unrepresentable_span) {
    obp::RequestRange range;
    range.start = 0;
    range.end = UINT64_MAX;  // len would overflow to 0
    MT_EXPECT_TRUE(!range.Len().ok());
    MT_EXPECT_EQ(range.Len().error().status, obp::kStatusRangeNotSatisfiable);
}

MT_TEST(parse_content_range_reads_start_end_and_total) {
    const agentenv::core::Expected<obp::ContentRange, std::string> range =
        obp::ParseContentRange("bytes 10-19/100");
    MT_EXPECT_TRUE(range.ok());
    MT_EXPECT_EQ(range.value().start, static_cast<uint64_t>(10));
    MT_EXPECT_EQ(range.value().end, static_cast<uint64_t>(19));
    MT_EXPECT_EQ(*range.value().total, static_cast<uint64_t>(100));

    // `*` means the origin does not know the total.
    const agentenv::core::Expected<obp::ContentRange, std::string> unknown =
        obp::ParseContentRange("bytes 0-9/*");
    MT_EXPECT_TRUE(unknown.ok());
    MT_EXPECT_TRUE(!unknown.value().total.has_value());
}

MT_TEST(parse_content_range_rejects_malformed_values) {
    MT_EXPECT_TRUE(!obp::ParseContentRange("items 0-9/100").ok());
    MT_EXPECT_TRUE(!obp::ParseContentRange("bytes 0-9").ok());
    MT_EXPECT_TRUE(!obp::ParseContentRange("bytes 09/100").ok());
    MT_EXPECT_TRUE(!obp::ParseContentRange("bytes 9-0/100").ok());
    MT_EXPECT_TRUE(!obp::ParseContentRange("bytes a-b/100").ok());
    MT_EXPECT_TRUE(!obp::ParseContentRange("bytes 0-9/abc").ok());
}

// ---------------------------------------------------------------------------
// facade — header policy
// ---------------------------------------------------------------------------

MT_TEST(conditional_headers_are_rejected) {
    // The facade has no validators to compare against, so answering a
    // conditional request unconditionally would be wrong, not just unsupported.
    const char* const conditional[] = {"If-Match", "if-none-match", "If-Modified-Since",
                                       "If-Range", "IF-UNMODIFIED-SINCE"};
    for (std::size_t i = 0; i < sizeof(conditional) / sizeof(conditional[0]); ++i) {
        obp::HeaderMap headers;
        headers.Set(conditional[i], "x");
        const agentenv::core::Expected<Unit, obp::HttpFacadeError> result =
            obp::RejectConditionalHeaders(headers);
        MT_EXPECT_TRUE(!result.ok());
        MT_EXPECT_EQ(result.error().status, obp::kStatusPreconditionFailed);
    }

    obp::HeaderMap plain;
    plain.Set("Range", "bytes=0-1");
    plain.Set("User-Agent", "overlaybd");
    MT_EXPECT_TRUE(obp::RejectConditionalHeaders(plain).ok());
}

MT_TEST(only_authorization_and_user_agent_are_forwarded) {
    obp::HeaderMap headers;
    headers.Set("Authorization", "Bearer token");
    headers.Set("User-Agent", "overlaybd/1");
    headers.Set("Cookie", "session=secret");
    headers.Set("X-Custom", "value");
    headers.Set("Host", "facade.local");

    const std::vector<std::pair<std::string, std::string> > forwarded =
        obp::ForwardedHeaders(headers);
    // An allowlist: anything not named is dropped, so a future client header
    // cannot leak to the origin by default.
    MT_EXPECT_EQ(forwarded.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(forwarded[0].first, std::string("authorization"));
    MT_EXPECT_EQ(forwarded[0].second, std::string("Bearer token"));
    MT_EXPECT_EQ(forwarded[1].first, std::string("user-agent"));

    MT_EXPECT_EQ(obp::ForwardedHeaders(obp::HeaderMap()).size(),
                 static_cast<std::size_t>(0));
}

MT_TEST(header_map_is_case_insensitive) {
    obp::HeaderMap headers;
    headers.Set("Content-Range", "bytes 0-9/10");
    MT_EXPECT_TRUE(headers.Get("content-range").has_value());
    MT_EXPECT_TRUE(headers.Get("CONTENT-RANGE").has_value());
    // Re-setting under different casing replaces rather than duplicates.
    headers.Set("CONTENT-RANGE", "bytes 0-4/10");
    MT_EXPECT_EQ(headers.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(*headers.Get("Content-Range"), std::string("bytes 0-4/10"));
}

// ---------------------------------------------------------------------------
// facade — origin URL handling
// ---------------------------------------------------------------------------

MT_TEST(reconstruct_origin_url_reattaches_the_query) {
    // The query must come back: a presigned URL is unusable without its
    // signature. Only the *canonical key* drops it.
    MT_EXPECT_EQ(
        obp::ReconstructOriginUrl("/p2p-http/https://example.com/blob?sig=abc").value(),
        std::string("https://example.com/blob?sig=abc"));
    MT_EXPECT_EQ(obp::ReconstructOriginUrl("/p2p-http/https://example.com/blob").value(),
                 std::string("https://example.com/blob"));
}

MT_TEST(reconstruct_origin_url_rejects_bad_paths) {
    MT_EXPECT_TRUE(!obp::ReconstructOriginUrl("").ok());
    MT_EXPECT_TRUE(!obp::ReconstructOriginUrl("/other/prefix").ok());
    // Prefix present but nothing after it.
    MT_EXPECT_TRUE(!obp::ReconstructOriginUrl("/p2p-http/").ok());
    MT_EXPECT_TRUE(!obp::ReconstructOriginUrl("/p2p-http").ok());
}

MT_TEST(validate_origin_url_allows_only_http_schemes) {
    MT_EXPECT_TRUE(obp::ValidateOriginUrl("http://example.com/a").ok());
    MT_EXPECT_TRUE(obp::ValidateOriginUrl("https://example.com/a").ok());

    // `file://` would turn the facade into a local file read primitive.
    const char* const rejected[] = {"file:///etc/passwd", "ftp://example.com/a",
                                    "gopher://example.com", "not a url"};
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        const agentenv::core::Expected<Unit, obp::HttpFacadeError> result =
            obp::ValidateOriginUrl(rejected[i]);
        MT_EXPECT_TRUE(!result.ok());
        MT_EXPECT_EQ(result.error().status, obp::kStatusBadRequest);
    }
}

// ---------------------------------------------------------------------------
// facade — publish validation
// ---------------------------------------------------------------------------

MT_TEST(validate_digest_requires_sha256_and_64_hex) {
    MT_EXPECT_TRUE(obp::ValidateDigest(kDigestA).ok());

    const char* const rejected[] = {
        "sha512:aaaa",                 // wrong algorithm
        "aaaaaaaa",                    // no prefix
        "sha256:",                     // empty hex
        "sha256:aaaa",                 // too short
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!obp::ValidateDigest(rejected[i]).ok());
    }
    // 64 characters but not all hex.
    MT_EXPECT_TRUE(!obp::ValidateDigest(std::string("sha256:") + std::string(63, 'a') + "z").ok());
    // 65 hex characters.
    MT_EXPECT_TRUE(!obp::ValidateDigest(std::string("sha256:") + std::string(65, 'a')).ok());
}

MT_TEST(publish_path_allowlist_fails_closed) {
    std::vector<std::string> none;
    const agentenv::core::Expected<Unit, obp::HttpFacadeError> unconfigured =
        obp::ValidatePublishPath("/data/store/layer", none);
    // No roots must mean "deny", not "allow anything".
    MT_EXPECT_TRUE(!unconfigured.ok());
    MT_EXPECT_EQ(unconfigured.error().status, obp::kStatusForbidden);
    MT_EXPECT_TRUE(unconfigured.error().message.find("no publish roots are configured") !=
                   std::string::npos);
}

MT_TEST(publish_path_allowlist_compares_whole_components) {
    std::vector<std::string> roots;
    roots.push_back("/data/store");

    MT_EXPECT_TRUE(obp::ValidatePublishPath("/data/store/layer.commit", roots).ok());
    MT_EXPECT_TRUE(obp::ValidatePublishPath("/data/store", roots).ok());

    // A plain string prefix test would wrongly accept this sibling directory.
    const agentenv::core::Expected<Unit, obp::HttpFacadeError> sibling =
        obp::ValidatePublishPath("/data/storeX/layer", roots);
    MT_EXPECT_TRUE(!sibling.ok());
    MT_EXPECT_EQ(sibling.error().status, obp::kStatusForbidden);

    MT_EXPECT_TRUE(!obp::ValidatePublishPath("/etc/passwd", roots).ok());
}

MT_TEST(publish_path_allowlist_accepts_any_configured_root) {
    std::vector<std::string> roots;
    roots.push_back("/data/a");
    roots.push_back("/data/b");
    MT_EXPECT_TRUE(obp::ValidatePublishPath("/data/b/layer", roots).ok());
    MT_EXPECT_TRUE(!obp::ValidatePublishPath("/data/c/layer", roots).ok());
}

MT_TEST(canonicalize_publish_roots_creates_and_resolves) {
    TempRoot root;
    std::vector<std::string> roots;
    roots.push_back(root.Join("store/./nested/.."));

    const agentenv::core::Expected<std::vector<std::string>, std::string> canonical =
        obp::CanonicalizePublishRoots(roots);
    MT_EXPECT_TRUE(canonical.ok());
    MT_EXPECT_EQ(canonical.value().size(), static_cast<std::size_t>(1));
    // Canonical form is what makes the prefix check sound.
    MT_EXPECT_TRUE(canonical.value()[0].find("/./") == std::string::npos);
    MT_EXPECT_TRUE(canonical.value()[0].find("/..") == std::string::npos);
    MT_EXPECT_TRUE(fs::IsDir(canonical.value()[0]));
}

MT_TEST(publish_layer_request_json_round_trips) {
    obp::PublishLayerRequest request;
    request.path = "/data/store/layer.commit";
    request.digest = kDigestA;
    request.size = 4096;
    request.source_url = std::string("https://example.com/blob?sig=x");

    const agentenv::core::Expected<obp::PublishLayerRequest, std::string> parsed =
        obp::PublishLayerRequest::ParseJson(request.ToJson());
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().path, request.path);
    MT_EXPECT_EQ(parsed.value().digest, request.digest);
    MT_EXPECT_EQ(parsed.value().size, request.size);
    MT_EXPECT_EQ(*parsed.value().source_url, *request.source_url);

    // source_url is optional (Rust `#[serde(default)]`).
    const agentenv::core::Expected<obp::PublishLayerRequest, std::string> minimal =
        obp::PublishLayerRequest::ParseJson(
            std::string("{\"path\":\"/a\",\"digest\":\"") + kDigestA + "\",\"size\":1}");
    MT_EXPECT_TRUE(minimal.ok());
    MT_EXPECT_TRUE(!minimal.value().source_url.has_value());
}

MT_TEST(publish_layer_request_rejects_missing_fields) {
    MT_EXPECT_TRUE(!obp::PublishLayerRequest::ParseJson("{}").ok());
    MT_EXPECT_TRUE(!obp::PublishLayerRequest::ParseJson("{\"path\":\"/a\"}").ok());
    MT_EXPECT_TRUE(
        !obp::PublishLayerRequest::ParseJson("{\"path\":\"/a\",\"digest\":\"d\"}").ok());
    // A negative size would become a huge unsigned value.
    MT_EXPECT_TRUE(!obp::PublishLayerRequest::ParseJson(
                        "{\"path\":\"/a\",\"digest\":\"d\",\"size\":-1}")
                        .ok());
    MT_EXPECT_TRUE(!obp::PublishLayerRequest::ParseJson("[]").ok());
}

// ---------------------------------------------------------------------------
// facade — response framing
// ---------------------------------------------------------------------------

MT_TEST(build_range_response_headers_uses_inclusive_end) {
    const agentenv::core::Expected<obp::RangeResponseHeaders, obp::HttpFacadeError> headers =
        obp::BuildRangeResponseHeaders(obp::ParseSingleRange(RangeHeaders("bytes=10-19")).value(),
                                       10, Optional<uint64_t>(static_cast<uint64_t>(100)));
    MT_EXPECT_TRUE(headers.ok());
    MT_EXPECT_EQ(headers.value().status, obp::kStatusPartialContent);
    MT_EXPECT_EQ(headers.value().content_length, static_cast<uint64_t>(10));
    MT_EXPECT_EQ(headers.value().content_range, std::string("bytes 10-19/100"));
    MT_EXPECT_EQ(headers.value().accept_ranges, std::string("bytes"));
}

MT_TEST(build_range_response_headers_reports_unknown_total_as_star) {
    const agentenv::core::Expected<obp::RangeResponseHeaders, obp::HttpFacadeError> headers =
        obp::BuildRangeResponseHeaders(obp::ParseSingleRange(RangeHeaders("bytes=0-4")).value(),
                                       5, Optional<uint64_t>());
    MT_EXPECT_TRUE(headers.ok());
    MT_EXPECT_EQ(headers.value().content_range, std::string("bytes 0-4/*"));
}

MT_TEST(build_range_response_headers_shortens_to_the_actual_length) {
    // The P2P side may return fewer bytes than requested when the layer ends
    // early; the Content-Range must reflect what is actually sent.
    const agentenv::core::Expected<obp::RangeResponseHeaders, obp::HttpFacadeError> headers =
        obp::BuildRangeResponseHeaders(obp::ParseSingleRange(RangeHeaders("bytes=90-199")).value(),
                                       10, Optional<uint64_t>(static_cast<uint64_t>(100)));
    MT_EXPECT_TRUE(headers.ok());
    MT_EXPECT_EQ(headers.value().content_range, std::string("bytes 90-99/100"));
}

MT_TEST(build_range_response_headers_rejects_an_empty_body) {
    const agentenv::core::Expected<obp::RangeResponseHeaders, obp::HttpFacadeError> headers =
        obp::BuildRangeResponseHeaders(obp::ParseSingleRange(RangeHeaders("bytes=0-9")).value(),
                                       0, Optional<uint64_t>());
    MT_EXPECT_TRUE(!headers.ok());
    MT_EXPECT_EQ(headers.error().status, obp::kStatusRangeNotSatisfiable);
}

// ---------------------------------------------------------------------------
// facade — fetch length clamping
// ---------------------------------------------------------------------------

MT_TEST(p2p_fetch_len_clamps_to_the_layer_end) {
    obp::RequestRange range;
    range.start = 90;
    range.end = 199;

    // 110 bytes requested but only 10 exist.
    MT_EXPECT_EQ(obp::P2pFetchLen(range, 110, Optional<uint64_t>(static_cast<uint64_t>(100)))
                     .value(),
                 static_cast<std::size_t>(10));
    // Fully inside the layer: unchanged.
    MT_EXPECT_EQ(obp::P2pFetchLen(range, 110, Optional<uint64_t>(static_cast<uint64_t>(1000)))
                     .value(),
                 static_cast<std::size_t>(110));
    // Unknown total: nothing to clamp against.
    MT_EXPECT_EQ(obp::P2pFetchLen(range, 110, Optional<uint64_t>()).value(),
                 static_cast<std::size_t>(110));
}

MT_TEST(p2p_fetch_len_rejects_a_start_past_the_end) {
    obp::RequestRange range;
    range.start = 100;
    range.end = 109;
    const agentenv::core::Expected<std::size_t, std::string> result =
        obp::P2pFetchLen(range, 10, Optional<uint64_t>(static_cast<uint64_t>(100)));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("starts past layer end") != std::string::npos);
}

// ---------------------------------------------------------------------------
// facade — origin response validation
// ---------------------------------------------------------------------------

MT_TEST(origin_response_must_honour_the_range) {
    obp::RequestRange range;
    range.start = 10;
    range.end = 19;

    obp::HeaderMap headers;
    headers.Set("Content-Range", "bytes 10-19/100");
    obp::ContentRange parsed;
    const agentenv::core::Expected<uint64_t, std::string> len =
        obp::ValidateOriginRangeResponse(obp::kStatusPartialContent, headers, range, &parsed);
    MT_EXPECT_TRUE(len.ok());
    MT_EXPECT_EQ(len.value(), static_cast<uint64_t>(10));
    MT_EXPECT_EQ(*parsed.total, static_cast<uint64_t>(100));
}

MT_TEST(origin_response_rejects_a_200_that_ignored_range) {
    obp::RequestRange range;
    range.start = 10;
    range.end = 19;

    // Framing a whole-blob 200 as a partial response would silently corrupt
    // the layer the runtime reads.
    const agentenv::core::Expected<uint64_t, std::string> ignored =
        obp::ValidateOriginRangeResponse(obp::kStatusOk, obp::HeaderMap(), range, NULL);
    MT_EXPECT_TRUE(!ignored.ok());
    MT_EXPECT_TRUE(ignored.error().find("ignored Range request") != std::string::npos);

    const agentenv::core::Expected<uint64_t, std::string> not_found =
        obp::ValidateOriginRangeResponse(404, obp::HeaderMap(), range, NULL);
    MT_EXPECT_TRUE(!not_found.ok());
    MT_EXPECT_TRUE(not_found.error().find("unexpected status 404") != std::string::npos);
}

MT_TEST(origin_response_rejects_a_shifted_or_oversized_window) {
    obp::RequestRange range;
    range.start = 10;
    range.end = 19;

    obp::HeaderMap shifted;
    shifted.Set("Content-Range", "bytes 0-9/100");
    const agentenv::core::Expected<uint64_t, std::string> wrong_start =
        obp::ValidateOriginRangeResponse(obp::kStatusPartialContent, shifted, range, NULL);
    MT_EXPECT_TRUE(!wrong_start.ok());
    MT_EXPECT_TRUE(wrong_start.error().find("wrong range start") != std::string::npos);

    obp::HeaderMap oversized;
    oversized.Set("Content-Range", "bytes 10-99/100");
    const agentenv::core::Expected<uint64_t, std::string> past =
        obp::ValidateOriginRangeResponse(obp::kStatusPartialContent, oversized, range, NULL);
    MT_EXPECT_TRUE(!past.ok());
    MT_EXPECT_TRUE(past.error().find("range past request") != std::string::npos);

    // A shorter window than requested is acceptable: the layer may end early.
    obp::HeaderMap shorter;
    shorter.Set("Content-Range", "bytes 10-14/100");
    MT_EXPECT_EQ(
        obp::ValidateOriginRangeResponse(obp::kStatusPartialContent, shorter, range, NULL).value(),
        static_cast<uint64_t>(5));
}

MT_TEST(origin_response_requires_content_range) {
    obp::RequestRange range;
    range.start = 0;
    range.end = 9;
    const agentenv::core::Expected<uint64_t, std::string> missing =
        obp::ValidateOriginRangeResponse(obp::kStatusPartialContent, obp::HeaderMap(), range,
                                         NULL);
    MT_EXPECT_TRUE(!missing.ok());
    MT_EXPECT_TRUE(missing.error().find("missing Content-Range") != std::string::npos);
}

MT_TEST(origin_response_rejects_contradictory_content_length) {
    obp::RequestRange range;
    range.start = 0;
    range.end = 9;

    obp::HeaderMap headers;
    headers.Set("Content-Range", "bytes 0-9/100");
    headers.Set("Content-Length", "5");  // says 5, range says 10
    const agentenv::core::Expected<uint64_t, std::string> mismatch =
        obp::ValidateOriginRangeResponse(obp::kStatusPartialContent, headers, range, NULL);
    MT_EXPECT_TRUE(!mismatch.ok());
    MT_EXPECT_TRUE(mismatch.error().find("does not match Content-Range") != std::string::npos);

    // Agreeing headers pass.
    headers.Set("Content-Length", "10");
    MT_EXPECT_TRUE(
        obp::ValidateOriginRangeResponse(obp::kStatusPartialContent, headers, range, NULL).ok());

    headers.Set("Content-Length", "not-a-number");
    MT_EXPECT_TRUE(
        !obp::ValidateOriginRangeResponse(obp::kStatusPartialContent, headers, range, NULL).ok());
}

// ---------------------------------------------------------------------------
// FacadeCore
// ---------------------------------------------------------------------------

MT_TEST(facade_core_caches_a_lookup_miss) {
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    obp::FacadeConfig config;
    obp::FacadeCore core(transport, config, std::shared_ptr<obp::OriginRangeFetcher>());

    const obp::CanonicalBlobIdentity canonical =
        obp::CanonicalBlobIdentity::FromDigest(kDigestA);
    const p2p::P2pArtifactKey key = obp::LayerArtifactKey(canonical);

    MT_EXPECT_TRUE(!core.LookupDescriptor(key, canonical).has_value());
    const int after_first = transport->LookupCount();
    // The second call is served from the cached miss, sparing a discovery
    // round-trip on every subsequent ranged read.
    MT_EXPECT_TRUE(!core.LookupDescriptor(key, canonical).has_value());
    MT_EXPECT_EQ(transport->LookupCount(), after_first);
}

MT_TEST(facade_core_publish_rejects_non_loopback_clients) {
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    obp::FacadeCore core(transport, obp::FacadeConfig(),
                         std::shared_ptr<obp::OriginRangeFetcher>());

    obp::PublishLayerRequest request;
    request.path = "/data/store/layer";
    request.digest = kDigestA;
    request.size = 1;

    const agentenv::core::Expected<Unit, obp::HttpFacadeError> remote =
        core.HandlePublishLayer(false, request);
    MT_EXPECT_TRUE(!remote.ok());
    MT_EXPECT_EQ(remote.error().status, obp::kStatusForbidden);
    MT_EXPECT_TRUE(remote.error().message.find("only accepts localhost") != std::string::npos);
    // Nothing was published.
    MT_EXPECT_EQ(transport->PublishCount(), 0);
}

MT_TEST(facade_core_publish_verifies_size_and_digest) {
    TempRoot root;
    root.WriteFile("store/layer.commit", "layer bytes");

    const agentenv::core::Expected<std::vector<std::string>, std::string> roots =
        obp::CanonicalizePublishRoots(std::vector<std::string>(1, root.Join("store")));
    MT_EXPECT_TRUE(roots.ok());

    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    obp::FacadeConfig config;
    config.allowed_publish_roots = roots.value();
    obp::FacadeCore core(transport, config, std::shared_ptr<obp::OriginRangeFetcher>());

    const agentenv::core::Expected<agentenv::core::FileDigest, std::string> described =
        agentenv::core::DescribeFile(root.Join("store/layer.commit"));
    MT_EXPECT_TRUE(described.ok());

    obp::PublishLayerRequest request;
    request.path = root.Join("store/layer.commit");
    request.digest = described.value().sha256;
    request.size = described.value().size;

    MT_EXPECT_TRUE(core.HandlePublishLayer(true, request).ok());
    MT_EXPECT_EQ(transport->PublishCount(), 1);
    MT_EXPECT_TRUE(transport->HasBlob(obp::LayerKeyFromDigest(request.digest)));

    // A wrong size is caught before hashing.
    obp::PublishLayerRequest wrong_size = request;
    wrong_size.size = request.size + 1;
    const agentenv::core::Expected<Unit, obp::HttpFacadeError> size_error =
        core.HandlePublishLayer(true, wrong_size);
    MT_EXPECT_TRUE(!size_error.ok());
    MT_EXPECT_TRUE(size_error.error().message.find("layer size mismatch") != std::string::npos);

    // A wrong digest is caught by the re-hash: localhost is a process
    // boundary, not a trust boundary.
    obp::PublishLayerRequest wrong_digest = request;
    wrong_digest.digest = kDigestA;
    const agentenv::core::Expected<Unit, obp::HttpFacadeError> digest_error =
        core.HandlePublishLayer(true, wrong_digest);
    MT_EXPECT_TRUE(!digest_error.ok());
    MT_EXPECT_TRUE(digest_error.error().message.find("layer digest mismatch") !=
                   std::string::npos);
}

MT_TEST(facade_core_publish_rejects_a_path_outside_the_roots) {
    TempRoot root;
    root.WriteFile("store/allowed.commit", "ok");
    root.WriteFile("outside/secret.bin", "no");

    const agentenv::core::Expected<std::vector<std::string>, std::string> roots =
        obp::CanonicalizePublishRoots(std::vector<std::string>(1, root.Join("store")));
    MT_EXPECT_TRUE(roots.ok());

    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    obp::FacadeConfig config;
    config.allowed_publish_roots = roots.value();
    obp::FacadeCore core(transport, config, std::shared_ptr<obp::OriginRangeFetcher>());

    const agentenv::core::Expected<agentenv::core::FileDigest, std::string> described =
        agentenv::core::DescribeFile(root.Join("outside/secret.bin"));
    MT_EXPECT_TRUE(described.ok());

    obp::PublishLayerRequest request;
    request.path = root.Join("outside/secret.bin");
    request.digest = described.value().sha256;
    request.size = described.value().size;

    const agentenv::core::Expected<Unit, obp::HttpFacadeError> result =
        core.HandlePublishLayer(true, request);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_EQ(result.error().status, obp::kStatusForbidden);
    MT_EXPECT_EQ(transport->PublishCount(), 0);
}

MT_TEST(facade_core_publish_resolves_symlinks_before_the_allowlist_check) {
    TempRoot root;
    root.WriteFile("outside/secret.bin", "no");
    MT_EXPECT_TRUE(fs::CreateDirAll(root.Join("store")).ok());

    // A symlink inside an allowed root pointing out of it. Without
    // canonicalization first, the prefix check would pass.
    if (::symlink(root.Join("outside/secret.bin").c_str(),
                  root.Join("store/escape.commit").c_str()) != 0) {
        return;  // filesystem without symlink support
    }

    const agentenv::core::Expected<std::vector<std::string>, std::string> roots =
        obp::CanonicalizePublishRoots(std::vector<std::string>(1, root.Join("store")));
    MT_EXPECT_TRUE(roots.ok());

    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    obp::FacadeConfig config;
    config.allowed_publish_roots = roots.value();
    obp::FacadeCore core(transport, config, std::shared_ptr<obp::OriginRangeFetcher>());

    const agentenv::core::Expected<agentenv::core::FileDigest, std::string> described =
        agentenv::core::DescribeFile(root.Join("outside/secret.bin"));
    MT_EXPECT_TRUE(described.ok());

    obp::PublishLayerRequest request;
    request.path = root.Join("store/escape.commit");
    request.digest = described.value().sha256;
    request.size = described.value().size;

    const agentenv::core::Expected<Unit, obp::HttpFacadeError> result =
        core.HandlePublishLayer(true, request);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_EQ(result.error().status, obp::kStatusForbidden);
}

MT_TEST(facade_core_uuid_request_reports_a_miss_as_not_found) {
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    obp::FacadeCore core(transport, obp::FacadeConfig(),
                         std::shared_ptr<obp::OriginRangeFetcher>());

    // A UUID-addressed layer has no origin to fall back to.
    const agentenv::core::Expected<obp::OriginRangeResult, obp::HttpFacadeError> result =
        core.HandleUuidRequest("11111111-2222-3333-4444-555555555555",
                               RangeHeaders("bytes=0-9"));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_EQ(result.error().status, obp::kStatusNotFound);
}

MT_TEST(facade_core_origin_request_validates_before_looking_up) {
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    obp::FacadeCore core(transport, obp::FacadeConfig(),
                         std::shared_ptr<obp::OriginRangeFetcher>());

    // A conditional header is refused before any lookup happens.
    obp::HeaderMap conditional = RangeHeaders("bytes=0-9");
    conditional.Set("If-Match", "x");
    const agentenv::core::Expected<obp::OriginRangeResult, obp::HttpFacadeError> precondition =
        core.HandleOriginRequest("/p2p-http/https://example.com/blob", conditional);
    MT_EXPECT_TRUE(!precondition.ok());
    MT_EXPECT_EQ(precondition.error().status, obp::kStatusPreconditionFailed);

    // A bad scheme, likewise.
    const agentenv::core::Expected<obp::OriginRangeResult, obp::HttpFacadeError> scheme =
        core.HandleOriginRequest("/p2p-http/file:///etc/passwd", RangeHeaders("bytes=0-9"));
    MT_EXPECT_TRUE(!scheme.ok());
    MT_EXPECT_EQ(scheme.error().status, obp::kStatusBadRequest);

    MT_EXPECT_EQ(transport->LookupCount(), 0);
}

MT_TEST(facade_core_origin_request_fails_gateway_without_a_fetcher) {
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    obp::FacadeCore core(transport, obp::FacadeConfig(),
                         std::shared_ptr<obp::OriginRangeFetcher>());

    // Nothing in the catalog and no origin client: a 502, not a crash.
    const agentenv::core::Expected<obp::OriginRangeResult, obp::HttpFacadeError> result =
        core.HandleOriginRequest("/p2p-http/https://example.com/blob",
                                 RangeHeaders("bytes=0-9"));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_EQ(result.error().status, obp::kStatusBadGateway);
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

MT_TEST(facade_addresses_are_built_from_the_bound_authority) {
    const obp::FacadeAddresses addresses = obp::FacadeAddresses::ForAuthority("127.0.0.1:38211");
    MT_EXPECT_EQ(addresses.address, std::string("http://127.0.0.1:38211/p2p-http"));
    MT_EXPECT_EQ(addresses.uuid_address, std::string("http://127.0.0.1:38211/p2p-uuid"));
    MT_EXPECT_EQ(addresses.publish_address,
                 std::string("http://127.0.0.1:38211/p2p-control/publish-layer"));
}

MT_TEST(facade_config_is_absent_when_p2p_is_disabled) {
    agentenv::cfg::AppConfig config;
    config.p2p.enabled = false;
    p2p::MockTransport transport;

    MT_EXPECT_TRUE(!obp::FacadeConfigFromAppConfig(config, transport,
                                                   std::vector<std::string>())
                        .has_value());
}

MT_TEST(facade_config_takes_its_timeouts_from_the_ublk_section) {
    agentenv::cfg::AppConfig config;
    config.p2p.enabled = true;
    config.ublk.overlaybd.p2p_lookup_timeout_ms = 123;
    config.ublk.overlaybd.p2p_fetch_range_timeout_ms = 4567;
    p2p::MockTransport transport;

    std::vector<std::string> roots;
    roots.push_back("/data/store");
    const Optional<obp::FacadeConfig> facade_config =
        obp::FacadeConfigFromAppConfig(config, transport, roots);
    if (!facade_config.has_value()) return;  // transport reported no endpoint

    // These bound a per-read operation and are far tighter than the
    // transport's own `[p2p]` timeouts.
    MT_EXPECT_EQ(facade_config->lookup_timeout_ms, static_cast<uint64_t>(123));
    MT_EXPECT_EQ(facade_config->fetch_range_timeout_ms, static_cast<uint64_t>(4567));
    MT_EXPECT_EQ(facade_config->allowed_publish_roots.size(), static_cast<std::size_t>(1));
    // Cache knobs keep their defaults.
    MT_EXPECT_EQ(facade_config->descriptor_cache_ttl_ms, obp::kDefaultDescriptorCacheTtlMs);
    MT_EXPECT_EQ(facade_config->descriptor_cache_max_entries,
                 obp::kDefaultDescriptorCacheMaxEntries);
}

MT_TEST(disabled_runtime_exposes_no_addresses) {
    // A disabled runtime is a normal state; the absent addresses are what make
    // the generated overlaybd config fall back to `enable: false`.
    obp::OverlaybdP2pRuntime runtime;
    MT_EXPECT_TRUE(!runtime.enabled());
    MT_EXPECT_TRUE(!runtime.ReadFacadeAddress().has_value());
    MT_EXPECT_TRUE(!runtime.UuidAddress().has_value());
    MT_EXPECT_TRUE(!runtime.PublishAddress().has_value());
    MT_EXPECT_TRUE(runtime.Shutdown().ok());
}

MT_TEST(started_runtime_exposes_its_addresses_until_shutdown) {
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    std::shared_ptr<obp::FacadeCore> core(new obp::FacadeCore(
        transport, obp::FacadeConfig(), std::shared_ptr<obp::OriginRangeFetcher>()));

    obp::OverlaybdP2pRuntime runtime = obp::OverlaybdP2pRuntime::Started(
        core, obp::FacadeAddresses::ForAuthority("127.0.0.1:1"));
    MT_EXPECT_TRUE(runtime.enabled());
    MT_EXPECT_EQ(*runtime.ReadFacadeAddress(), std::string("http://127.0.0.1:1/p2p-http"));

    MT_EXPECT_TRUE(runtime.Shutdown().ok());
    MT_EXPECT_TRUE(!runtime.enabled());
    // Idempotent, matching Rust's consuming shutdown plus Drop.
    MT_EXPECT_TRUE(runtime.Shutdown().ok());
}

MT_TEST(started_runtime_with_no_core_stays_disabled) {
    const obp::OverlaybdP2pRuntime runtime = obp::OverlaybdP2pRuntime::Started(
        std::shared_ptr<obp::FacadeCore>(), obp::FacadeAddresses::ForAuthority("127.0.0.1:1"));
    MT_EXPECT_TRUE(!runtime.enabled());
}

int main() { return microtest::RunAll(); }
