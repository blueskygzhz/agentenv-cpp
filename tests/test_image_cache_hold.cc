// SPDX-License-Identifier: MIT
// Rust: src/image/cache/service.rs — operation-hold lifecycle.
#include "agentenv/image/cache_hold.h"

#include <set>
#include <string>

#include "agentenv/core/fs.h"
#include "microtest.h"

using namespace agentenv;              // NOLINT
using namespace agentenv::image;       // NOLINT
using namespace agentenv::image::cache;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        const core::Expected<std::string, std::string> dir =
            core::fs::CreateTempDir("agentenv-image-cache-hold-");
        path = dir.ok() ? dir.value() : std::string("/tmp/agentenv-hold-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

std::shared_ptr<ImageCacheMetadataStore> TestStore(const std::string& dir) {
    // Memory durability: these tests never restart the store, so the WAL
    // would only add fsync cost.
    const core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        ImageCacheMetadataStore::Open(dir + "/meta.db", local_store::Durability::Memory);
    MT_EXPECT_TRUE(store.ok());
    return store.value();
}

HardCommitId Hard(const std::string& digest) {
    const core::Expected<HardCommitId, std::string> id = HardCommitId::New(digest);
    MT_EXPECT_TRUE(id.ok());
    return id.value();
}

const char* const kDigestA = "sha256:aaaa";
const char* const kDigestB = "sha256:bbbb";

std::set<std::string> Digests(const std::string& a) {
    std::set<std::string> out;
    out.insert(a);
    return out;
}

}  // namespace

// ---- owner allocation ------------------------------------------------------

MT_TEST(cache_hold_owner_is_namespaced_and_unique) {
    ResetOperationHoldCounterForTesting();

    const core::Expected<ImageCacheHoldOwner, std::string> first =
        NextOperationHoldOwner("resolve");
    const core::Expected<ImageCacheHoldOwner, std::string> second =
        NextOperationHoldOwner("resolve");
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(second.ok());

    MT_EXPECT_EQ(first.value().Namespace(), std::string(kOperationHoldNamespaceName));
    // Two concurrent operations with the *same name* must not share an owner:
    // the first to finish would otherwise release the other's protection
    // while it is still running.
    MT_EXPECT_TRUE(first.value() != second.value());
    MT_EXPECT_EQ(first.value().Key(), std::string("resolve/1"));
    MT_EXPECT_EQ(second.value().Key(), std::string("resolve/2"));
}

MT_TEST(cache_hold_owner_rejects_a_blank_operation) {
    MT_EXPECT_TRUE(!NextOperationHoldOwner("").ok());
    MT_EXPECT_TRUE(!NextOperationHoldOwner("   ").ok());
}

// ---- eager acquisition -----------------------------------------------------

MT_TEST(cache_hold_eager_acquisition_writes_the_hold) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::AcquireForHardCommits(store, "gc", Digests(kDigestA));
    MT_EXPECT_TRUE(hold.ok());
    MT_EXPECT_TRUE(hold.value()->materialized());
    MT_EXPECT_EQ(hold.value()->digests().size(), static_cast<std::size_t>(1));

    // The hold is visible to GC immediately, which is the whole point of the
    // eager form.
    const core::Expected<std::vector<ImageCacheHoldOwner>, std::string> referrers =
        store->HardCommitHoldReferrers(Hard(kDigestA));
    MT_EXPECT_TRUE(referrers.ok());
    MT_EXPECT_EQ(referrers.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(referrers.value()[0] == hold.value()->owner());
}

MT_TEST(cache_hold_eager_acquisition_rejects_a_bad_digest) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);
    MT_EXPECT_TRUE(
        !ImageCacheOperationHold::AcquireForHardCommits(store, "gc", Digests("")).ok());
}

// ---- lazy acquisition ------------------------------------------------------

MT_TEST(cache_hold_lazy_begin_writes_nothing_until_protected) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::Begin(store, "resolve");
    MT_EXPECT_TRUE(hold.ok());
    // Most resolves hit a cached config and protect nothing; writing a hold
    // per resolve would churn the store for no benefit.
    MT_EXPECT_TRUE(!hold.value()->materialized());
    MT_EXPECT_TRUE(hold.value()->digests().empty());

    const core::Expected<std::vector<ImageCacheHoldOwner>, std::string> before =
        store->HardCommitHoldReferrers(Hard(kDigestA));
    MT_EXPECT_TRUE(before.ok());
    MT_EXPECT_TRUE(before.value().empty());
}

MT_TEST(cache_hold_protect_materializes_and_accumulates) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::Begin(store, "resolve");
    MT_EXPECT_TRUE(hold.ok());

    MT_EXPECT_TRUE(hold.value()->Protect(kDigestA).ok());
    MT_EXPECT_TRUE(hold.value()->materialized());
    MT_EXPECT_TRUE(hold.value()->Protect(kDigestB).ok());
    MT_EXPECT_EQ(hold.value()->digests().size(), static_cast<std::size_t>(2));

    // Both commits are pinned under the one owner.
    MT_EXPECT_EQ(store->HardCommitHoldReferrers(Hard(kDigestA)).value().size(),
                 static_cast<std::size_t>(1));
    MT_EXPECT_EQ(store->HardCommitHoldReferrers(Hard(kDigestB)).value().size(),
                 static_cast<std::size_t>(1));
}

MT_TEST(cache_hold_protect_is_idempotent) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::Begin(store, "resolve");
    MT_EXPECT_TRUE(hold.ok());

    MT_EXPECT_TRUE(hold.value()->Protect(kDigestA).ok());
    // Revisiting the same layer must not rewrite the entry.
    MT_EXPECT_TRUE(hold.value()->Protect(kDigestA).ok());
    MT_EXPECT_EQ(hold.value()->digests().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(store->HardCommitHoldReferrers(Hard(kDigestA)).value().size(),
                 static_cast<std::size_t>(1));
}

MT_TEST(cache_hold_protect_rejects_a_bad_digest) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);
    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::Begin(store, "resolve");
    MT_EXPECT_TRUE(hold.ok());
    MT_EXPECT_TRUE(!hold.value()->Protect("").ok());
    // A rejected digest must not half-materialize the hold.
    MT_EXPECT_TRUE(!hold.value()->materialized());
}

// ---- release ---------------------------------------------------------------

MT_TEST(cache_hold_release_drops_the_protection) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::AcquireForHardCommits(store, "gc", Digests(kDigestA));
    MT_EXPECT_TRUE(hold.ok());

    MT_EXPECT_TRUE(hold.value()->Release("done").ok());
    MT_EXPECT_TRUE(hold.value()->released());
    MT_EXPECT_TRUE(store->HardCommitHoldReferrers(Hard(kDigestA)).value().empty());
}

MT_TEST(cache_hold_release_is_idempotent) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::AcquireForHardCommits(store, "gc", Digests(kDigestA));
    MT_EXPECT_TRUE(hold.ok());
    MT_EXPECT_TRUE(hold.value()->Release("done").ok());
    // Idempotent so the destructor can call it unconditionally.
    MT_EXPECT_TRUE(hold.value()->Release("again").ok());
}

MT_TEST(cache_hold_release_of_an_unmaterialized_hold_is_a_noop) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
        ImageCacheOperationHold::Begin(store, "resolve");
    MT_EXPECT_TRUE(hold.ok());
    // Nothing was written, so there is nothing to release.
    MT_EXPECT_TRUE(hold.value()->Release("done").ok());
    MT_EXPECT_TRUE(hold.value()->released());
}

MT_TEST(cache_hold_destructor_releases_the_hold) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    ImageCacheHoldOwner owner = NextOperationHoldOwner("scope").value();
    {
        const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> hold =
            ImageCacheOperationHold::AcquireForHardCommits(store, "scope", Digests(kDigestA));
        MT_EXPECT_TRUE(hold.ok());
        owner = hold.value()->owner();
        MT_EXPECT_EQ(store->HardCommitHoldReferrers(Hard(kDigestA)).value().size(),
                     static_cast<std::size_t>(1));
    }

    // RAII stands in for Rust's `Drop`: leaving scope releases the lease, so
    // an operation that returns early cannot pin commits forever.
    MT_EXPECT_TRUE(store->HardCommitHoldReferrers(Hard(kDigestA)).value().empty());
}

MT_TEST(cache_hold_destructor_does_not_re_release_after_explicit_release) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    // A second hold on the same digest, taken after the first released, must
    // survive the first hold's destructor.
    core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> second =
        ImageCacheOperationHold::Begin(store, "later");
    {
        const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> first =
            ImageCacheOperationHold::AcquireForHardCommits(store, "early", Digests(kDigestA));
        MT_EXPECT_TRUE(first.ok());
        MT_EXPECT_TRUE(first.value()->Release("early done").ok());

        MT_EXPECT_TRUE(second.ok());
        MT_EXPECT_TRUE(second.value()->Protect(kDigestA).ok());
    }

    // The first hold's destructor released its *own* owner, which no longer
    // has an entry, and must not have disturbed the second's.
    const core::Expected<std::vector<ImageCacheHoldOwner>, std::string> referrers =
        store->HardCommitHoldReferrers(Hard(kDigestA));
    MT_EXPECT_TRUE(referrers.ok());
    MT_EXPECT_EQ(referrers.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(referrers.value()[0] == second.value()->owner());
}

MT_TEST(cache_hold_two_holds_on_one_digest_are_independent) {
    TempRoot root;
    std::shared_ptr<ImageCacheMetadataStore> store = TestStore(root.path);

    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> first =
        ImageCacheOperationHold::AcquireForHardCommits(store, "resolve", Digests(kDigestA));
    const core::Expected<std::shared_ptr<ImageCacheOperationHold>, std::string> second =
        ImageCacheOperationHold::AcquireForHardCommits(store, "resolve", Digests(kDigestA));
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_EQ(store->HardCommitHoldReferrers(Hard(kDigestA)).value().size(),
                 static_cast<std::size_t>(2));

    // Releasing one must leave the other's protection in place — this is what
    // the unique-owner rule buys.
    MT_EXPECT_TRUE(first.value()->Release("done").ok());
    const core::Expected<std::vector<ImageCacheHoldOwner>, std::string> remaining =
        store->HardCommitHoldReferrers(Hard(kDigestA));
    MT_EXPECT_TRUE(remaining.ok());
    MT_EXPECT_EQ(remaining.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(remaining.value()[0] == second.value()->owner());
}

int main() { return microtest::RunAll(); }
