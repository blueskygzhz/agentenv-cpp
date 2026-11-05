// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/documents.rs
//
// The compare-and-swap that serialises catalog writes across nodes. Every
// assertion here is about a race or about the decision that avoids one —
// asserting the happy path alone would pass against an unconditional write,
// which is the bug this layer exists to prevent.
#include "agentenv/snapshot/repository/oss/documents.h"

#include <string>
#include <vector>

#include "microtest.h"
#include "oss_object_store_fake.h"

using namespace agentenv;                                  // NOLINT
using namespace agentenv::snapshot::repository;            // NOLINT
using namespace agentenv::snapshot::repository::oss;       // NOLINT
using agentenv::testing::FakeObjectStore;

namespace {

/// Instant and deterministic: every attempt is exhausted without sleeping,
/// and the delay is pinned to the low end of each window.
DocumentUpdatePolicy TestPolicy() {
    DocumentUpdatePolicy policy;
    policy.jitter = [](int64_t lo, int64_t /*hi*/) { return lo; };
    policy.sleeper = [](int64_t) {};
    return policy;
}

DocumentKey VolumeKey(const std::string& id) { return DocumentKey::VolumeRecord(id); }

/// An update that replaces the document with `bytes` regardless of what is
/// there, recording what it was handed.
DocumentUpdate PutAlways(const std::string& bytes, int* calls = 0,
                         std::vector<std::string>* seen = 0) {
    return [bytes, calls, seen](const core::Optional<std::string>& current)
               -> RepositoryResult<DocumentChange> {
        if (calls) ++*calls;
        if (seen) seen->push_back(current.has_value() ? *current : std::string("<absent>"));
        return DocumentChange::Put(bytes);
    };
}

}  // namespace

// ---- the create-only path -------------------------------------------------

// An absent document is written create-only (`If-None-Match: *`). Writing it
// with an empty If-Match instead would succeed against a document another
// node created in between, losing that creation.
MT_TEST(update_creates_an_absent_document_only_if_it_is_still_absent) {
    FakeObjectStore store;
    int calls = 0;
    std::vector<std::string> seen;

    const RepositoryResult<core::Optional<std::string> > updated = UpdateDocument(
        &store, VolumeKey("vol_a"), PutAlways("first", &calls, &seen), TestPolicy());

    MT_EXPECT_TRUE(updated.ok());
    // The decision function saw no current document...
    MT_EXPECT_EQ(calls, 1);
    MT_EXPECT_TRUE(seen.size() == 1 && seen[0] == "<absent>");
    // ...and `update` returns the value it decided from, which was absent.
    MT_EXPECT_TRUE(updated.ok() && !updated.value().has_value());
    MT_EXPECT_TRUE(store.Has(VolumeKey("vol_a").Path()));
    MT_EXPECT_TRUE(store.Get(VolumeKey("vol_a").Path()) == "first");
}

// The race that create-only exists to stop: another node creates the document
// between this update's read and its write. The write must be refused, and the
// retry must then decide again from the *new* contents rather than from the
// absence it originally saw.
MT_TEST(a_creation_that_loses_the_race_retries_against_the_winners_document) {
    FakeObjectStore store;
    const std::string path = VolumeKey("vol_a").Path();
    int calls = 0;
    std::vector<std::string> seen;

    // The competing writer runs in the gap between read and write.
    store.set_before_put([&store, &path]() { store.Seed(path, "from-other-node"); });

    const RepositoryResult<core::Optional<std::string> > updated =
        UpdateDocument(&store, VolumeKey("vol_a"), PutAlways("mine", &calls, &seen), TestPolicy());

    MT_EXPECT_TRUE(updated.ok());
    // Two decisions: the first from an absent document, the second from the
    // other node's. A CAS that re-sent the first decision without re-reading
    // would overwrite "from-other-node" having never seen it.
    MT_EXPECT_EQ(calls, 2);
    MT_EXPECT_TRUE(seen.size() == 2);
    MT_EXPECT_TRUE(seen.size() == 2 && seen[0] == "<absent>");
    MT_EXPECT_TRUE(seen.size() == 2 && seen[1] == "from-other-node");
    // One write was refused, one accepted.
    MT_EXPECT_EQ(store.refused_puts(), 1);
    MT_EXPECT_TRUE(store.Get(path) == "mine");
    // And the returned "previous" value is the one the winning attempt saw.
    MT_EXPECT_TRUE(updated.ok() && updated.value().has_value() &&
                   *updated.value() == "from-other-node");
}

// ---- the If-Match path ----------------------------------------------------

// Same race, but on an existing document: a concurrent replacement invalidates
// the token, so the write is refused and the decision is remade.
MT_TEST(an_update_that_loses_the_race_redecides_from_the_new_contents) {
    FakeObjectStore store;
    const std::string path = VolumeKey("vol_b").Path();
    store.Seed(path, "v1");

    int calls = 0;
    std::vector<std::string> seen;
    store.set_before_put([&store, &path]() { store.Seed(path, "v2-from-other-node"); });

    const RepositoryResult<core::Optional<std::string> > updated = UpdateDocument(
        &store, VolumeKey("vol_b"), PutAlways("v3-mine", &calls, &seen), TestPolicy());

    MT_EXPECT_TRUE(updated.ok());
    MT_EXPECT_EQ(calls, 2);
    MT_EXPECT_TRUE(seen.size() == 2 && seen[0] == "v1");
    MT_EXPECT_TRUE(seen.size() == 2 && seen[1] == "v2-from-other-node");
    MT_EXPECT_EQ(store.refused_puts(), 1);
    MT_EXPECT_TRUE(store.Get(path) == "v3-mine");
}

// A document that keeps changing is reported rather than retried forever, and
// the attempt cap is the one upstream uses.
MT_TEST(an_update_that_never_wins_stops_after_the_attempt_limit) {
    FakeObjectStore store;
    const std::string path = VolumeKey("vol_c").Path();
    store.Seed(path, "v1");

    int calls = 0;
    // Re-armed every time, so this writer always wins the gap.
    struct Rearm {
        FakeObjectStore* store;
        std::string path;
        int n;
        void operator()() {
            store->Seed(path, "v" + std::to_string(++n));
            store->set_before_put(*this);
        }
    };
    Rearm rearm;
    rearm.store = &store;
    rearm.path = path;
    rearm.n = 1;
    store.set_before_put(rearm);

    DocumentUpdatePolicy policy = TestPolicy();
    const RepositoryResult<core::Optional<std::string> > updated =
        UpdateDocument(&store, VolumeKey("vol_c"), PutAlways("mine", &calls), policy);

    MT_EXPECT_TRUE(!updated.ok());
    MT_EXPECT_TRUE(!updated.ok() && updated.error().kind == RepositoryErrorKind::InvalidRequest);
    // Exactly the cap: one decision and one refused write per attempt. A loop
    // that gave up early would silently fail updates a retry would have
    // landed; one that never gave up would hang the caller.
    MT_EXPECT_EQ(calls, static_cast<int>(kMaxUpdateAttempts));
    MT_EXPECT_EQ(store.refused_puts(), static_cast<int>(kMaxUpdateAttempts));
    // The loser never wrote.
    MT_EXPECT_TRUE(store.Get(path) != "mine");
}

// ---- `Changed`: the state that must not be read as `NotFound` -------------

// A read that lands between another writer's stat and bytes yields `Changed`.
// There is nothing consistent to decide from, so the decision function must
// not be called for that attempt -- and the retry must still see the document
// as existing. Treating `Changed` as `NotFound` would hand the update an
// absent document and then write it create-only, which can never succeed
// while the object exists.
MT_TEST(a_changed_read_retries_without_deciding_and_does_not_become_a_creation) {
    FakeObjectStore store;
    const std::string path = VolumeKey("vol_d").Path();
    store.Seed(path, "v1");

    int calls = 0;
    std::vector<std::string> seen;
    store.ForceChangedReads(2);

    const RepositoryResult<core::Optional<std::string> > updated = UpdateDocument(
        &store, VolumeKey("vol_d"), PutAlways("v2", &calls, &seen), TestPolicy());

    MT_EXPECT_TRUE(updated.ok());
    // Three reads: two inconsistent, one good. Only the last one decided.
    MT_EXPECT_EQ(store.reads(), 3);
    MT_EXPECT_EQ(calls, 1);
    MT_EXPECT_TRUE(seen.size() == 1 && seen[0] == "v1");
    // One conditional write, and it was an If-Match rather than a create.
    MT_EXPECT_EQ(store.conditional_puts(), 1);
    MT_EXPECT_EQ(store.refused_puts(), 0);
    MT_EXPECT_TRUE(store.Get(path) == "v2");
}

// ---- Keep and Delete ------------------------------------------------------

// `Keep` writes nothing at all. A CAS that wrote the unchanged bytes back
// would invalidate every other node's token for no reason, turning a read
// into a source of contention.
MT_TEST(keep_writes_nothing) {
    FakeObjectStore store;
    const std::string path = VolumeKey("vol_e").Path();
    store.Seed(path, "v1");

    const RepositoryResult<core::Optional<std::string> > updated = UpdateDocument(
        &store, VolumeKey("vol_e"),
        [](const core::Optional<std::string>&) -> RepositoryResult<DocumentChange> {
            return DocumentChange::Keep();
        },
        TestPolicy());

    MT_EXPECT_TRUE(updated.ok());
    MT_EXPECT_TRUE(updated.ok() && updated.value().has_value() && *updated.value() == "v1");
    MT_EXPECT_EQ(store.conditional_puts(), 0);
    MT_EXPECT_EQ(store.deletes(), 0);
}

// Deleting an already-absent document is success with nothing done -- the
// caller's intent is already true, and issuing the delete would be a request
// per no-op.
MT_TEST(deleting_an_absent_document_does_nothing) {
    FakeObjectStore store;
    const RepositoryResult<core::Optional<std::string> > updated = UpdateDocument(
        &store, VolumeKey("vol_f"),
        [](const core::Optional<std::string>&) -> RepositoryResult<DocumentChange> {
            return DocumentChange::Delete();
        },
        TestPolicy());

    MT_EXPECT_TRUE(updated.ok());
    MT_EXPECT_TRUE(updated.ok() && !updated.value().has_value());
    MT_EXPECT_EQ(store.deletes(), 0);
}

MT_TEST(deleting_an_existing_document_removes_it) {
    FakeObjectStore store;
    const std::string path = VolumeKey("vol_g").Path();
    store.Seed(path, "v1");

    const RepositoryResult<core::Optional<std::string> > updated = UpdateDocument(
        &store, VolumeKey("vol_g"),
        [](const core::Optional<std::string>&) -> RepositoryResult<DocumentChange> {
            return DocumentChange::Delete();
        },
        TestPolicy());

    MT_EXPECT_TRUE(updated.ok());
    MT_EXPECT_TRUE(updated.ok() && updated.value().has_value() && *updated.value() == "v1");
    MT_EXPECT_EQ(store.deletes(), 1);
    MT_EXPECT_TRUE(!store.Has(path));
}

// ---- stores that cannot support a CAS -------------------------------------

// An append-only store is Alibaba's native OSS, which ignores a write
// precondition. Upstream takes a lease there; this build has none, so the
// write is refused. Falling back to the CAS would report success while
// overwriting a concurrent update -- the one failure mode with no later
// symptom.
MT_TEST(an_append_only_store_is_refused_rather_than_written_unconditionally) {
    FakeObjectStore store;
    store.set_can_append(true);

    const RepositoryResult<core::Optional<std::string> > updated =
        UpdateDocument(&store, VolumeKey("vol_h"), PutAlways("x"), TestPolicy());
    MT_EXPECT_TRUE(!updated.ok());
    MT_EXPECT_TRUE(!updated.ok() && updated.error().kind == RepositoryErrorKind::Unsupported);

    MT_EXPECT_TRUE(!PutDocument(&store, VolumeKey("vol_h"), "x").ok());
    MT_EXPECT_TRUE(!DeleteDocument(&store, VolumeKey("vol_h")).ok());
    // Nothing was written by any of them.
    MT_EXPECT_TRUE(!store.Has(VolumeKey("vol_h").Path()));
    MT_EXPECT_EQ(store.conditional_puts(), 0);

    // A read needs no precondition, so it still works.
    MT_EXPECT_TRUE(GetDocument(store, VolumeKey("vol_h")).ok());
}

// A store that serves no ETag cannot be updated safely either, and says so
// rather than writing unconditionally.
MT_TEST(a_store_without_etags_refuses_to_update_an_existing_document) {
    FakeObjectStore store;
    store.set_supply_etags(false);
    store.Seed(VolumeKey("vol_i").Path(), "v1");

    const RepositoryResult<core::Optional<std::string> > updated =
        UpdateDocument(&store, VolumeKey("vol_i"), PutAlways("v2"), TestPolicy());
    MT_EXPECT_TRUE(!updated.ok());
    MT_EXPECT_TRUE(!updated.ok() && updated.error().kind == RepositoryErrorKind::InvalidRequest);
    MT_EXPECT_EQ(store.conditional_puts(), 0);
    MT_EXPECT_TRUE(store.Get(VolumeKey("vol_i").Path()) == "v1");
}

// A transport failure is not a lost race: it must propagate instead of being
// retried as contention, or a store that is down would burn 32 attempts and
// then report the wrong cause.
MT_TEST(a_read_failure_propagates_instead_of_retrying) {
    FakeObjectStore store;
    store.set_read_fails(true);

    const RepositoryResult<core::Optional<std::string> > updated =
        UpdateDocument(&store, VolumeKey("vol_j"), PutAlways("x"), TestPolicy());
    MT_EXPECT_TRUE(!updated.ok());
    MT_EXPECT_TRUE(!updated.ok() && updated.error().kind == RepositoryErrorKind::Backend);
    MT_EXPECT_EQ(store.reads(), 1);
}

// ---- plain get / put / delete --------------------------------------------

MT_TEST(get_returns_absent_for_a_missing_document) {
    FakeObjectStore store;
    const RepositoryResult<core::Optional<std::string> > got =
        GetDocument(store, VolumeKey("nope"));
    MT_EXPECT_TRUE(got.ok());
    MT_EXPECT_TRUE(got.ok() && !got.value().has_value());
}

MT_TEST(put_then_get_round_trips_through_the_documents_own_path) {
    FakeObjectStore store;
    MT_EXPECT_TRUE(PutDocument(&store, VolumeKey("vol_k"), "body").ok());
    // Stored under `DocumentKey::Path`, the layout every node agrees on --
    // not the POSIX path, which differs for aliases.
    MT_EXPECT_TRUE(store.Has(VolumeKey("vol_k").Path()));

    const RepositoryResult<core::Optional<std::string> > got =
        GetDocument(store, VolumeKey("vol_k"));
    MT_EXPECT_TRUE(got.ok() && got.value().has_value() && *got.value() == "body");
}

// ---- listing --------------------------------------------------------------

MT_TEST(list_returns_ids_in_key_order_and_honours_the_cursor_and_limit) {
    FakeObjectStore store;
    MT_EXPECT_TRUE(PutDocument(&store, VolumeKey("vol_a"), "a").ok());
    MT_EXPECT_TRUE(PutDocument(&store, VolumeKey("vol_b"), "b").ok());
    MT_EXPECT_TRUE(PutDocument(&store, VolumeKey("vol_c"), "c").ok());

    const RepositoryResult<std::vector<std::string> > all = ListDocuments(
        store, DocumentCollection::VolumeRecords, core::Optional<std::string>(), 10);
    MT_EXPECT_TRUE(all.ok());
    MT_EXPECT_TRUE(all.ok() && all.value().size() == 3);
    MT_EXPECT_TRUE(all.ok() && all.value()[0] == "vol_a");
    MT_EXPECT_TRUE(all.ok() && all.value()[2] == "vol_c");

    // The cursor is an id, resolved through the collection's key so it is
    // compared in object-key space.
    const RepositoryResult<std::vector<std::string> > after = ListDocuments(
        store, DocumentCollection::VolumeRecords, core::Optional<std::string>("vol_a"), 10);
    MT_EXPECT_TRUE(after.ok() && after.value().size() == 2);
    MT_EXPECT_TRUE(after.ok() && after.value()[0] == "vol_b");

    const RepositoryResult<std::vector<std::string> > limited = ListDocuments(
        store, DocumentCollection::VolumeRecords, core::Optional<std::string>(), 2);
    MT_EXPECT_TRUE(limited.ok() && limited.value().size() == 2);
}

// A key nested deeper under the prefix is not a document. Taking its leading
// component as an id would invent one from a path, and the caller would then
// look up a record that does not exist.
MT_TEST(a_nested_key_is_not_a_document_id) {
    const std::string prefix = DocumentCollectionPrefix(DocumentCollection::VolumeRecords);
    MT_EXPECT_TRUE(!DocumentIdFromKey(DocumentCollection::VolumeRecords,
                                      prefix + "sub/vol_a.json")
                        .ok());
    // The prefix alone carries no id either.
    MT_EXPECT_TRUE(!DocumentIdFromKey(DocumentCollection::VolumeRecords, prefix).ok());
    // A key outside the collection is refused rather than mapped.
    MT_EXPECT_TRUE(
        !DocumentIdFromKey(DocumentCollection::VolumeRecords, "elsewhere/vol_a.json").ok());
}

// ---- the backoff schedule -------------------------------------------------

// Doubling with a cap, drawn from `[next/2, next]`. The growth has to happen
// even when the sleeper is a no-op, or an injected one would leave the
// schedule pinned at its first interval.
MT_TEST(backoff_doubles_each_interval_up_to_the_cap) {
    std::vector<int64_t> windows;
    std::vector<int64_t> slept;
    Backoff backoff(
        [&windows](int64_t lo, int64_t hi) {
            windows.push_back(lo);
            windows.push_back(hi);
            return lo;
        },
        [&slept](int64_t ms) { slept.push_back(ms); });

    MT_EXPECT_EQ(static_cast<int>(backoff.NextDelayMs()), static_cast<int>(kBackoffFirstMs));
    for (int i = 0; i < 8; ++i) backoff.Wait();

    // First window is [10, 20] for a 20 ms start: the low end is half.
    MT_EXPECT_EQ(static_cast<int>(windows[0]), static_cast<int>(kBackoffFirstMs / 2));
    MT_EXPECT_EQ(static_cast<int>(windows[1]), static_cast<int>(kBackoffFirstMs));
    // Second is [20, 40].
    MT_EXPECT_EQ(static_cast<int>(windows[2]), static_cast<int>(kBackoffFirstMs));
    MT_EXPECT_EQ(static_cast<int>(windows[3]), static_cast<int>(kBackoffFirstMs * 2));
    // And it stops growing at the cap rather than running away.
    MT_EXPECT_EQ(static_cast<int>(backoff.NextDelayMs()), static_cast<int>(kBackoffMaxMs));
    MT_EXPECT_EQ(static_cast<int>(slept.size()), 8);
}

MT_MAIN
