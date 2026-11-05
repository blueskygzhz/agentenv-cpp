// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/client.rs
//
// What matters about this client is the requests it builds and how it reads
// the answers. Both are observable through an injected transport, so these
// tests need no server — and the parts that *would* need one (TLS, a real
// bucket) are the parts this class does not implement.
//
// The payoff of keeping `ObjectStore` an interface shows up at the end: the
// same compare-and-swap exercised against the in-memory fake is re-run here
// against the real client driven by a stub that implements ETag semantics
// over HTTP status codes.
#include "agentenv/snapshot/repository/oss/client.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "agentenv/snapshot/repository/oss/documents.h"
#include "microtest.h"
#include "oss_client_fixture.h"

using namespace agentenv;                                  // NOLINT
using namespace agentenv::snapshot::repository;            // NOLINT
using namespace agentenv::snapshot::repository::oss;       // NOLINT
using namespace agentenv::testing::oss_fixture;            // NOLINT

namespace sov = agentenv::storage::overlaybd;
namespace shttp = agentenv::storage::overlaybd::http;

// ---- configuration normalisation ------------------------------------------

// Required fields are blank-is-absent: a field of spaces is a config that was
// never filled in, and sending it produces a 403 whose cause is invisible in
// the config file.
MT_TEST(config_requires_bucket_endpoint_region_and_credentials) {
    MT_EXPECT_TRUE(OssClientConfig::FromBackendConfig(GoodConfig()).ok());

    cfg::OssBackendConfig blank_bucket = GoodConfig();
    blank_bucket.bucket = "   ";
    MT_EXPECT_TRUE(!OssClientConfig::FromBackendConfig(blank_bucket).ok());

    cfg::OssBackendConfig blank_endpoint = GoodConfig();
    blank_endpoint.endpoint = "";
    MT_EXPECT_TRUE(!OssClientConfig::FromBackendConfig(blank_endpoint).ok());

    cfg::OssBackendConfig no_region = GoodConfig();
    no_region.region = core::nullopt;
    MT_EXPECT_TRUE(!OssClientConfig::FromBackendConfig(no_region).ok());

    // `allow_anonymous: false` upstream — the catalog is private, and an
    // unsigned request would 403 with no sign the credential was absent.
    cfg::OssBackendConfig no_key = GoodConfig();
    no_key.access_key_id = core::nullopt;
    MT_EXPECT_TRUE(!OssClientConfig::FromBackendConfig(no_key).ok());

    cfg::OssBackendConfig no_secret = GoodConfig();
    no_secret.access_key_secret = core::Optional<std::string>("  ");
    MT_EXPECT_TRUE(!OssClientConfig::FromBackendConfig(no_secret).ok());
}

// Refused rather than ignored: degrading to anonymous fails later as a 403
// that looks like a permissions problem instead of a missing credential.
MT_TEST(a_credential_process_is_refused_rather_than_ignored) {
    cfg::OssBackendConfig c = GoodConfig();
    c.credential_process = core::Optional<std::string>("/usr/bin/aliyun-cred");
    const RepositoryResult<OssClientConfig> normalized =
        OssClientConfig::FromBackendConfig(c);
    MT_EXPECT_TRUE(!normalized.ok());
    MT_EXPECT_TRUE(!normalized.ok() &&
                   normalized.error().kind == RepositoryErrorKind::Unsupported);
}

MT_TEST(config_trims_the_prefix_slashes) {
    cfg::OssBackendConfig c = GoodConfig();
    c.prefix = core::Optional<std::string>("/repo/catalog/");
    MT_EXPECT_TRUE(Normalized(c).prefix == "repo/catalog");
}

// ---- CanAppend: which strategy the document layer picks -------------------

// Alibaba's native OSS cannot write conditionally. Getting this wrong in
// either direction is harmful: reporting false for Aliyun would run a CAS
// whose precondition is ignored, and reporting true for MinIO would refuse a
// store that works.
MT_TEST(can_append_is_true_only_for_aliyun_endpoints) {
    RecordingTransport t;

    cfg::OssBackendConfig minio = GoodConfig();
    MT_EXPECT_TRUE(!Client(&t, minio)->CanAppend());

    cfg::OssBackendConfig aliyun = GoodConfig();
    aliyun.endpoint = "https://oss-cn-hangzhou.aliyuncs.com";
    MT_EXPECT_TRUE(Client(&t, aliyun)->CanAppend());

    cfg::OssBackendConfig internal = GoodConfig();
    internal.endpoint = "https://oss-cn-hangzhou.aliyun-inc.com";
    MT_EXPECT_TRUE(Client(&t, internal)->CanAppend());

    MT_EXPECT_TRUE(IsAliyunHost("oss-cn-hangzhou.aliyuncs.com"));
    // A look-alike domain is not Aliyun; matching on a substring would hand
    // the wrong strategy to an impostor host.
    MT_EXPECT_TRUE(!IsAliyunHost("aliyuncs.com.evil.example"));
}

// ---- GET: one request carries both the bytes and the version -------------

// Rust does `stat` then a conditional `read` because opendal's read returns
// no metadata. One GET is both atomic and cheaper: the bytes and the ETag
// describing them come from the same response and cannot disagree.
MT_TEST(get_reads_bytes_and_etag_from_a_single_response) {
    RecordingTransport t;
    t.Push(200, "{\"id\":\"vol_a\"}", "\"abc123\"");
    OssClient* client = Client(&t, GoodConfig());

    const RepositoryResult<ObjectRead> read = client->GetWithEtag("volumes/records/vol_a.json");
    MT_EXPECT_TRUE(read.ok());
    MT_EXPECT_TRUE(read.ok() && read.value().kind == ObjectRead::Kind::Found);
    MT_EXPECT_TRUE(read.ok() && read.value().bytes == "{\"id\":\"vol_a\"}");
    // The quotes are part of the token: a conditional request must echo it
    // byte for byte, and stripping them yields a precondition that never
    // matches -- indistinguishable from permanent contention.
    MT_EXPECT_TRUE(read.ok() && read.value().etag.has_value() &&
                   *read.value().etag == "\"abc123\"");

    MT_EXPECT_EQ(static_cast<int>(t.requests.size()), 1);
    MT_EXPECT_TRUE(t.requests[0].method == "GET");
    // Path-style addressing for a non-Aliyun endpoint.
    MT_EXPECT_TRUE(t.requests[0].url ==
                   "https://minio.internal:9000/agentenv/volumes/records/vol_a.json");
    // Signed: SigV4 adds these, and their absence would be a 403.
    MT_EXPECT_TRUE(!HeaderOf(t.requests[0], "Authorization").empty());
    MT_EXPECT_TRUE(!HeaderOf(t.requests[0], "x-amz-date").empty());
    // A catalog read must not follow a redirect to somewhere unsigned.
    MT_EXPECT_TRUE(!t.requests[0].follow_redirects);
}

// 404 is an answer, not a failure: an update probes for a document it may
// still have to create.
MT_TEST(get_maps_404_to_not_found) {
    RecordingTransport t;
    t.Push(404, "<Error><Code>NoSuchKey</Code></Error>");
    OssClient* client = Client(&t, GoodConfig());

    const RepositoryResult<ObjectRead> read = client->GetWithEtag("volumes/records/nope.json");
    MT_EXPECT_TRUE(read.ok());
    MT_EXPECT_TRUE(read.ok() && read.value().kind == ObjectRead::Kind::NotFound);
}

// A found object with no ETag is reported as such rather than having one
// invented; the document layer refuses to update without a version token.
MT_TEST(get_reports_a_missing_etag_instead_of_inventing_one) {
    RecordingTransport t;
    t.Push(200, "body");  // no ETag header
    OssClient* client = Client(&t, GoodConfig());

    const RepositoryResult<ObjectRead> read = client->GetWithEtag("k");
    MT_EXPECT_TRUE(read.ok() && read.value().kind == ObjectRead::Kind::Found);
    MT_EXPECT_TRUE(read.ok() && !read.value().etag.has_value());
}

// ---- conditional PUT ------------------------------------------------------

MT_TEST(a_conditional_put_sends_if_match_for_an_existing_version) {
    RecordingTransport t;
    t.Push(200);
    OssClient* client = Client(&t, GoodConfig());

    const RepositoryResult<bool> put = client->PutConditionally(
        "volumes/records/vol_a.json", "body", core::Optional<std::string>("\"v1\""));
    MT_EXPECT_TRUE(put.ok() && put.value());

    MT_EXPECT_TRUE(t.requests[0].method == "PUT");
    MT_EXPECT_TRUE(HeaderOf(t.requests[0], "If-Match") == "\"v1\"");
    MT_EXPECT_TRUE(HeaderOf(t.requests[0], "If-None-Match").empty());
    const std::string body(t.requests[0].body.begin(), t.requests[0].body.end());
    MT_EXPECT_TRUE(body == "body");
}

// An absent token means create-only. `If-None-Match: *` is the form that
// fails exactly when something is already there; an empty If-Match would
// instead succeed against a document another node just created.
MT_TEST(a_create_only_put_sends_if_none_match_star) {
    RecordingTransport t;
    t.Push(200);
    OssClient* client = Client(&t, GoodConfig());

    MT_EXPECT_TRUE(client->PutConditionally("k", "body", core::nullopt).ok());
    MT_EXPECT_TRUE(HeaderOf(t.requests[0], "If-None-Match") == "*");
    MT_EXPECT_TRUE(HeaderOf(t.requests[0], "If-Match").empty());
}

// 412 is a lost race, not an error. Returning an error instead would abort
// the update loop that exists precisely to retry it.
MT_TEST(a_failed_precondition_is_a_lost_race_rather_than_an_error) {
    RecordingTransport t;
    t.Push(412, "<Error><Code>PreconditionFailed</Code></Error>");
    OssClient* client = Client(&t, GoodConfig());

    const RepositoryResult<bool> put =
        client->PutConditionally("k", "body", core::Optional<std::string>("\"stale\""));
    MT_EXPECT_TRUE(put.ok());
    MT_EXPECT_TRUE(put.ok() && !put.value());
}

// Some stores report a create-only collision as 409 instead of 412. Both mean
// "something is already there", which is the same lost race.
MT_TEST(a_409_is_also_a_lost_race) {
    RecordingTransport t;
    t.Push(409);
    OssClient* client = Client(&t, GoodConfig());
    const RepositoryResult<bool> put = client->PutConditionally("k", "b", core::nullopt);
    MT_EXPECT_TRUE(put.ok() && !put.value());
}

// A real failure still propagates: a 403 must not be read as contention, or
// the update loop would spin 32 times over a permissions problem.
MT_TEST(a_403_on_put_is_an_error_not_a_lost_race) {
    RecordingTransport t;
    // Two passes, each exhausting its attempts, all 403.
    for (int i = 0; i < 8; ++i) t.Push(403, "<Error><Code>AccessDenied</Code></Error>");
    OssClient* client = Client(&t, GoodConfig());

    const RepositoryResult<bool> put = client->PutConditionally("k", "b", core::nullopt);
    MT_EXPECT_TRUE(!put.ok());
    // The message carries the store's own code, which is what an operator
    // needs; the status alone sends them to the wrong place.
    MT_EXPECT_TRUE(!put.ok() && put.error().message.find("AccessDenied") != std::string::npos);
}

// ---- DELETE ---------------------------------------------------------------

MT_TEST(delete_treats_a_missing_object_as_success) {
    RecordingTransport t;
    t.Push(404);
    OssClient* client = Client(&t, GoodConfig());
    MT_EXPECT_TRUE(client->Delete("k").ok());
    MT_EXPECT_TRUE(t.requests[0].method == "DELETE");
}

MT_TEST(delete_accepts_the_204_s3_actually_sends) {
    RecordingTransport t;
    t.Push(204);
    OssClient* client = Client(&t, GoodConfig());
    MT_EXPECT_TRUE(client->Delete("k").ok());
}

// ---- retries --------------------------------------------------------------

// A transport failure means the request never landed, so retrying is safe.
MT_TEST(a_transport_failure_is_retried) {
    RecordingTransport t;
    t.transport_failures = 2;
    t.fallback.status = 200;
    OssClient* client = Client(&t, GoodConfig());

    MT_EXPECT_TRUE(client->Delete("k").ok());
    MT_EXPECT_EQ(static_cast<int>(t.requests.size()), 3);
}

// 5xx is the server asking to be retried; a 404 is its final answer and
// repeating it would only stall.
MT_TEST(a_5xx_is_retried_but_a_404_is_final) {
    RecordingTransport t;
    t.Push(503);
    t.Push(200, "body", "\"v1\"");
    OssClient* client = Client(&t, GoodConfig());
    const RepositoryResult<ObjectRead> read = client->GetWithEtag("k");
    MT_EXPECT_TRUE(read.ok() && read.value().kind == ObjectRead::Kind::Found);
    MT_EXPECT_EQ(static_cast<int>(t.requests.size()), 2);

    RecordingTransport t2;
    t2.Push(404);
    OssClient* c2 = Client(&t2, GoodConfig());
    MT_EXPECT_TRUE(c2->GetWithEtag("k").ok());
    MT_EXPECT_EQ(static_cast<int>(t2.requests.size()), 1);
}

// ---- listing --------------------------------------------------------------

MT_TEST(list_builds_a_sorted_canonical_query_and_parses_the_keys) {
    RecordingTransport t;
    t.Push(200,
           "<?xml version=\"1.0\"?>"
           "<ListBucketResult>"
           "<Prefix>volumes/records/</Prefix>"
           "<Contents><Key>volumes/records/vol_a.json</Key></Contents>"
           "<Contents><Key>volumes/records/vol_b.json</Key></Contents>"
           "<CommonPrefixes><Prefix>volumes/records/sub/</Prefix></CommonPrefixes>"
           "</ListBucketResult>");
    OssClient* client = Client(&t, GoodConfig());

    const RepositoryResult<std::vector<std::string> > keys = client->ListKeysPage(
        "volumes/records/", core::Optional<std::string>("volumes/records/vol_a.json"), 10);
    MT_EXPECT_TRUE(keys.ok());
    // Two objects -- the response's own `<Prefix>` echo and the
    // `<CommonPrefixes>` entry are not objects, and counting either would
    // invent a document that does not exist.
    MT_EXPECT_TRUE(keys.ok() && keys.value().size() == 2);
    MT_EXPECT_TRUE(keys.ok() && keys.value()[0] == "volumes/records/vol_a.json");
    MT_EXPECT_TRUE(keys.ok() && keys.value()[1] == "volumes/records/vol_b.json");

    // The canonical query must be sorted by key, because SigV4 hashes it:
    // `list-type` < `max-keys` < `prefix` < `start-after`.
    const std::string url = t.requests[0].url;
    const std::size_t q = url.find('?');
    MT_EXPECT_TRUE(q != std::string::npos);
    const std::string query = url.substr(q + 1);
    MT_EXPECT_TRUE(query.find("list-type=2") == 0);
    MT_EXPECT_TRUE(query.find("&max-keys=10") != std::string::npos);
    MT_EXPECT_TRUE(query.find("&prefix=") < query.find("&start-after="));
    // A bucket-level request: path style puts the bucket in the path and the
    // key is empty.
    MT_EXPECT_TRUE(url.find("https://minio.internal:9000/agentenv/?") == 0);
}

// A page is capped at the server's own maximum rather than asking for
// something it would silently clamp.
MT_TEST(list_caps_the_page_at_the_protocol_maximum) {
    RecordingTransport t;
    t.Push(200, "<ListBucketResult></ListBucketResult>");
    OssClient* client = Client(&t, GoodConfig());
    MT_EXPECT_TRUE(client->ListKeysPage("p/", core::nullopt, 5000).ok());
    MT_EXPECT_TRUE(t.requests[0].url.find("max-keys=1000") != std::string::npos);
}

MT_TEST(list_with_a_zero_limit_issues_no_request) {
    RecordingTransport t;
    OssClient* client = Client(&t, GoodConfig());
    const RepositoryResult<std::vector<std::string> > keys =
        client->ListKeysPage("p/", core::nullopt, 0);
    MT_EXPECT_TRUE(keys.ok() && keys.value().empty());
    MT_EXPECT_EQ(static_cast<int>(t.requests.size()), 0);
}

// With a prefix configured, keys are written under it and handed back
// without it -- so a caller sees the same keys it would on a repository with
// no prefix.
MT_TEST(a_configured_prefix_is_applied_to_keys_and_stripped_from_listings) {
    cfg::OssBackendConfig c = GoodConfig();
    c.prefix = core::Optional<std::string>("repo");

    RecordingTransport t;
    t.Push(200, "body", "\"v1\"");
    OssClient* client = Client(&t, c);
    MT_EXPECT_TRUE(client->FullKey("volumes/records/a.json") == "repo/volumes/records/a.json");
    MT_EXPECT_TRUE(client->GetWithEtag("volumes/records/a.json").ok());
    MT_EXPECT_TRUE(t.requests[0].url.find("/agentenv/repo/volumes/records/a.json") !=
                   std::string::npos);

    RecordingTransport t2;
    t2.Push(200,
            "<ListBucketResult>"
            "<Contents><Key>repo/volumes/records/a.json</Key></Contents>"
            "</ListBucketResult>");
    OssClient* c2 = Client(&t2, c);
    const RepositoryResult<std::vector<std::string> > keys =
        c2->ListKeysPage("volumes/records/", core::nullopt, 10);
    MT_EXPECT_TRUE(keys.ok() && keys.value().size() == 1);
    MT_EXPECT_TRUE(keys.ok() && keys.value()[0] == "volumes/records/a.json");
}

// A key outside the configured prefix means the server answered with
// something that was not asked for. Inventing a relative key from it would
// corrupt the listing, so it is refused.
MT_TEST(a_listed_key_outside_the_prefix_is_refused) {
    cfg::OssBackendConfig c = GoodConfig();
    c.prefix = core::Optional<std::string>("repo");

    RecordingTransport t;
    t.Push(200,
           "<ListBucketResult>"
           "<Contents><Key>elsewhere/a.json</Key></Contents>"
           "</ListBucketResult>");
    OssClient* client = Client(&t, c);
    MT_EXPECT_TRUE(!client->ListKeysPage("volumes/records/", core::nullopt, 10).ok());
}

// An escaped key is unescaped: `&amp;` in the XML is one `&` in the key, and
// leaving it escaped would name an object that does not exist.
MT_TEST(listed_keys_are_xml_unescaped) {
    const std::vector<std::string> keys = ParseListObjectsKeys(
        "<ListBucketResult><Contents><Key>a&amp;b&lt;c&gt;d.json</Key></Contents>"
        "</ListBucketResult>");
    MT_EXPECT_TRUE(keys.size() == 1);
    MT_EXPECT_TRUE(keys.size() == 1 && keys[0] == "a&b<c>d.json");

    MT_EXPECT_TRUE(ParseListObjectsTruncated("<R><IsTruncated>true</IsTruncated></R>"));
    MT_EXPECT_TRUE(!ParseListObjectsTruncated("<R><IsTruncated>false</IsTruncated></R>"));
    MT_EXPECT_TRUE(!ParseListObjectsTruncated("<R></R>"));
}

// ---- virtual-hosted addressing -------------------------------------------

// Aliyun endpoints require virtual-hosted style, where the bucket is in the
// host and the path is the key alone. Signing the wrong canonical URI is a
// 403 that looks like a credential problem.
MT_TEST(an_aliyun_endpoint_uses_virtual_hosted_addressing) {
    cfg::OssBackendConfig c = GoodConfig();
    c.endpoint = "https://oss-cn-hangzhou.aliyuncs.com";

    RecordingTransport t;
    t.Push(200, "body", "\"v1\"");
    OssClient* client = Client(&t, c);
    MT_EXPECT_TRUE(client->GetWithEtag("volumes/records/a.json").ok());
    MT_EXPECT_TRUE(t.requests[0].url ==
                   "https://agentenv.oss-cn-hangzhou.aliyuncs.com/volumes/records/a.json");
}

// ---- the document layer over the real client -----------------------------

// The same race the fake-backed tests cover, now over the real client: the
// competing write lands between this update's GET and its PUT, the
// precondition fails with a 412, and the retry decides again from the
// winner's document.
MT_TEST(the_cas_works_over_the_real_client_against_a_stub_server) {
    StubObjectServer server;
    const RepositoryResult<OssClient*> client =
        OssClient::NewWithTransport(Normalized(GoodConfig()), server.Fn());
    MT_EXPECT_TRUE(client.ok());

    const DocumentKey key = DocumentKey::VolumeRecord("vol_a");
    server.Seed(key.Path(), "v1");
    server.before_put = [&server, &key]() { server.Seed(key.Path(), "v2-other-node"); };

    std::vector<std::string> seen;
    const RepositoryResult<core::Optional<std::string> > updated = UpdateDocument(
        client.value(), key,
        [&seen](const core::Optional<std::string>& current)
            -> RepositoryResult<DocumentChange> {
            seen.push_back(current.has_value() ? *current : std::string("<absent>"));
            return DocumentChange::Put("v3-mine");
        },
        InstantPolicy());

    MT_EXPECT_TRUE(updated.ok());
    // Decided twice, the second time from the other node's bytes.
    MT_EXPECT_TRUE(seen.size() == 2);
    MT_EXPECT_TRUE(seen.size() == 2 && seen[0] == "v1");
    MT_EXPECT_TRUE(seen.size() == 2 && seen[1] == "v2-other-node");
    MT_EXPECT_TRUE(server.bodies[key.Path()] == "v3-mine");
}

// Create-only over the real client: the loser's `If-None-Match: *` is
// refused, and its retry writes on top of the winner rather than discarding
// it unseen.
MT_TEST(a_creation_race_over_the_real_client_does_not_lose_the_winners_write) {
    StubObjectServer server;
    const RepositoryResult<OssClient*> client =
        OssClient::NewWithTransport(Normalized(GoodConfig()), server.Fn());
    MT_EXPECT_TRUE(client.ok());

    const DocumentKey key = DocumentKey::VolumeRecord("vol_new");
    server.before_put = [&server, &key]() { server.Seed(key.Path(), "from-other-node"); };

    std::vector<std::string> seen;
    const RepositoryResult<core::Optional<std::string> > updated = UpdateDocument(
        client.value(), key,
        [&seen](const core::Optional<std::string>& current)
            -> RepositoryResult<DocumentChange> {
            seen.push_back(current.has_value() ? *current : std::string("<absent>"));
            return DocumentChange::Put("mine");
        },
        InstantPolicy());

    MT_EXPECT_TRUE(updated.ok());
    MT_EXPECT_TRUE(seen.size() == 2 && seen[0] == "<absent>");
    MT_EXPECT_TRUE(seen.size() == 2 && seen[1] == "from-other-node");
    MT_EXPECT_TRUE(server.bodies[key.Path()] == "mine");
}

// An append-only store is refused before any request is made: upstream takes
// a lease there, and running the CAS instead would report success while
// overwriting a concurrent update.
MT_TEST(the_document_layer_refuses_an_aliyun_client_without_issuing_a_request) {
    cfg::OssBackendConfig c = GoodConfig();
    c.endpoint = "https://oss-cn-hangzhou.aliyuncs.com";

    RecordingTransport t;
    OssClient* client = Client(&t, c);
    MT_EXPECT_TRUE(client->CanAppend());

    const RepositoryResult<core::Optional<std::string> > updated = UpdateDocument(
        client, DocumentKey::VolumeRecord("vol_a"),
        [](const core::Optional<std::string>&) -> RepositoryResult<DocumentChange> {
            return DocumentChange::Put("x");
        },
        InstantPolicy());
    MT_EXPECT_TRUE(!updated.ok());
    MT_EXPECT_TRUE(!updated.ok() && updated.error().kind == RepositoryErrorKind::Unsupported);
    MT_EXPECT_EQ(static_cast<int>(t.requests.size()), 0);
}

MT_MAIN
