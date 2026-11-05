// SPDX-License-Identifier: MIT
// An in-memory `ObjectStore` with real ETag semantics, plus the hooks a test
// needs to produce a race deterministically.
//
// Rust tests the OSS document layer against a MinIO container. There is none
// here, and a mock that only records requests would be worse than no test:
// the entire purpose of the compare-and-swap is what happens when two writers
// interleave, and that cannot be observed by inspecting one request.
//
// So this fake implements the semantics the CAS depends on — a version token
// that changes on every write, a conditional write that refuses a stale one,
// and a create-only write that refuses an existing object — and exposes a
// `before_put` hook, which is what lets a test inject another writer at the
// exact point between the read and the write.
#ifndef AGENTENV_TESTS_OSS_OBJECT_STORE_FAKE_H_
#define AGENTENV_TESTS_OSS_OBJECT_STORE_FAKE_H_

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "agentenv/snapshot/repository/oss/object_store.h"

namespace agentenv {
namespace testing {

/// An in-memory object store.
class FakeObjectStore : public snapshot::repository::oss::ObjectStore {
 public:
    typedef snapshot::repository::oss::ObjectRead ObjectRead;
    template <class T>
    struct Result {
        typedef snapshot::repository::RepositoryResult<T> Type;
    };

    FakeObjectStore() : can_append_(false), supply_etags_(true), next_version_(1) {}

    // ---- controls -------------------------------------------------------

    /// Makes this an Alibaba-native-OSS-like store, which cannot write
    /// conditionally and must therefore be refused by the document layer.
    void set_can_append(bool v) { can_append_ = v; }
    /// Drops the version token from reads, as a store without ETag support.
    void set_supply_etags(bool v) { supply_etags_ = v; }

    /// Forces the next `n` reads to report `Changed` — the object moved
    /// between its stat and its read.
    void ForceChangedReads(int n) { forced_changed_reads_ = n; }

    /// Runs just before a conditional write is evaluated. This is the seam a
    /// competing writer is injected through: whatever it does to the store is
    /// what the pending write then collides with.
    void set_before_put(const std::function<void()>& hook) { before_put_ = hook; }

    /// Fails every read with a backend error.
    void set_read_fails(bool v) { read_fails_ = v; }

    // ---- direct access, for arranging and asserting ---------------------

    /// Writes without going through any precondition.
    void Seed(const std::string& key, const std::string& bytes) {
        Object& o = objects_[key];
        o.bytes = bytes;
        o.version = next_version_++;
    }
    bool Has(const std::string& key) const { return objects_.count(key) != 0; }
    std::string Get(const std::string& key) const {
        const std::map<std::string, Object>::const_iterator it = objects_.find(key);
        return it == objects_.end() ? std::string() : it->second.bytes;
    }

    int reads() const { return reads_; }
    int conditional_puts() const { return conditional_puts_; }
    int refused_puts() const { return refused_puts_; }
    int deletes() const { return deletes_; }

    // ---- ObjectStore ----------------------------------------------------

    bool CanAppend() const override { return can_append_; }

    Result<ObjectRead>::Type GetWithEtag(const std::string& key) const override {
        ++reads_;
        if (read_fails_) {
            return core::make_unexpected(
                snapshot::repository::RepositoryError::Backend("fake read failure"));
        }
        if (forced_changed_reads_ > 0) {
            --forced_changed_reads_;
            return ObjectRead::Changed();
        }
        const std::map<std::string, Object>::const_iterator it = objects_.find(key);
        if (it == objects_.end()) return ObjectRead::NotFound();
        core::Optional<std::string> etag;
        if (supply_etags_) etag = core::Optional<std::string>(Etag(it->second.version));
        return ObjectRead::Found(it->second.bytes, etag);
    }

    Result<bool>::Type PutConditionally(const std::string& key, const std::string& bytes,
                                        const core::Optional<std::string>& etag) override {
        if (before_put_) {
            // Copied out and cleared first: the hook usually writes through
            // this same store, and leaving it installed would make that write
            // re-enter the hook forever.
            const std::function<void()> hook = before_put_;
            before_put_ = std::function<void()>();
            hook();
        }
        ++conditional_puts_;
        const std::map<std::string, Object>::iterator it = objects_.find(key);
        if (!etag.has_value()) {
            // Create-only: refuse an object that already exists.
            if (it != objects_.end()) {
                ++refused_puts_;
                return false;
            }
            Object& o = objects_[key];
            o.bytes = bytes;
            o.version = next_version_++;
            return true;
        }
        // If-Match: refuse a token that no longer describes the object.
        if (it == objects_.end() || Etag(it->second.version) != *etag) {
            ++refused_puts_;
            return false;
        }
        it->second.bytes = bytes;
        it->second.version = next_version_++;
        return true;
    }

    Result<core::Unit>::Type Put(const std::string& key, const std::string& bytes) override {
        Seed(key, bytes);
        return core::Unit();
    }

    Result<core::Unit>::Type Delete(const std::string& key) override {
        ++deletes_;
        objects_.erase(key);
        return core::Unit();
    }

    Result<std::vector<std::string> >::Type ListKeysPage(
        const std::string& prefix, const core::Optional<std::string>& start_after,
        std::size_t limit) const override {
        std::vector<std::string> keys;
        // `std::map` is ordered, which is the object-key order the contract
        // specifies.
        for (std::map<std::string, Object>::const_iterator it = objects_.begin();
             it != objects_.end(); ++it) {
            if (it->first.compare(0, prefix.size(), prefix) != 0) continue;
            if (start_after.has_value() && it->first <= *start_after) continue;
            if (keys.size() >= limit) break;
            keys.push_back(it->first);
        }
        return keys;
    }

 private:
    struct Object {
        std::string bytes;
        uint64_t version;
        Object() : version(0) {}
    };

    /// A quoted token, as a server sends it.
    static std::string Etag(uint64_t version) {
        char buf[32];
        ::snprintf(buf, sizeof(buf), "\"v%llu\"", static_cast<unsigned long long>(version));
        return std::string(buf);
    }

    std::map<std::string, Object> objects_;
    bool can_append_;
    bool supply_etags_;
    uint64_t next_version_;

    // Mutable: the read path is `const` but still counts and can be scripted.
    mutable int reads_ = 0;
    mutable int forced_changed_reads_ = 0;
    mutable bool read_fails_ = false;
    int conditional_puts_ = 0;
    int refused_puts_ = 0;
    int deletes_ = 0;
    std::function<void()> before_put_;
};

}  // namespace testing
}  // namespace agentenv
#endif  // AGENTENV_TESTS_OSS_OBJECT_STORE_FAKE_H_
