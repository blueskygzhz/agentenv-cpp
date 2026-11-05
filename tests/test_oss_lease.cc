// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/lease.rs test module.
//
// Upstream drives these on tokio's paused clock, so a 30-second lease expires
// instantly and sixteen writers interleave deterministically. This port is
// synchronous, so the two halves are tested differently: the timing rules run
// on an injected virtual clock in one thread, and mutual exclusion runs on
// real threads, which is the only way to make two appends actually race here.
#include <ctime>
#include <map>
#include <sstream>
#include <pthread.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "agentenv/snapshot/repository/oss/lease.h"
#include "microtest.h"

namespace {

namespace core = agentenv::core;
namespace lease = agentenv::snapshot::repository::oss::lease;
using agentenv::snapshot::repository::RepositoryErrorKind;
using agentenv::snapshot::repository::RepositoryResult;

/// How long each request to `FakeStore` takes, so requests overlap.
const int64_t kLatencyMs = 5;

/// Rust's paused clock. Every rule in the lease is a duration, so a test that
/// had to wait them out could not assert the take-over at all.
struct VirtualClock {
    int64_t now;
    VirtualClock() : now(0) {}

    lease::LeaseClock Handle() {
        VirtualClock* self = this;
        lease::LeaseClock clock;
        clock.now_ms = [self]() { return self->now; };
        // A virtual clock "waits" by moving, which is what makes the waiting
        // observable at all.
        clock.sleep_ms = [self](int64_t ms) { self->now += ms; };
        return clock;
    }
};

struct Mutex {
    pthread_mutex_t mu;
    Mutex() { pthread_mutex_init(&mu, 0); }
    ~Mutex() { pthread_mutex_destroy(&mu); }
    void Lock() { pthread_mutex_lock(&mu); }
    void Unlock() { pthread_mutex_unlock(&mu); }
};

struct Guard {
    Mutex& mu;
    explicit Guard(Mutex& mu) : mu(mu) { mu.Lock(); }
    ~Guard() { mu.Unlock(); }
};

/// Rust `FakeStore` — lock objects in memory, appended to the way Alibaba OSS
/// does: the length is read, then written at, and the append is refused if the
/// object grew in between.
class FakeStore : public lease::LockStore {
 public:
    /// `clock` may be null, in which case requests cost no virtual time and
    /// the caller is relying on the real clock.
    explicit FakeStore(VirtualClock* clock)
        : clock_(clock), lose_next_reply_(false), appends_(0) {}

    RepositoryResult<core::Optional<std::string> > ReadLock(const std::string& key) {
        Elapse(kLatencyMs);
        Guard guard(mu_);
        const std::map<std::string, std::string>::const_iterator found = objects_.find(key);
        if (found == objects_.end()) return core::Optional<std::string>();
        return core::Optional<std::string>(found->second);
    }

    RepositoryResult<core::Optional<uint64_t> > AppendLock(const std::string& key,
                                                           const std::string& data) {
        // Reads the length, then appends there unless the object grew. The
        // gap between the two is the race the lease exists to survive, so it
        // is modelled rather than collapsed into one locked step.
        Elapse(kLatencyMs);
        const uint64_t offset = Len(key);
        Elapse(kLatencyMs);
        Guard guard(mu_);
        ++appends_;
        if (LenUnlocked(key) != offset) return core::Optional<uint64_t>();
        objects_[key] += data;
        if (lose_next_reply_) {
            lose_next_reply_ = false;
            // Landed, but reported as if it had not.
            return core::Optional<uint64_t>();
        }
        return core::Optional<uint64_t>(offset);
    }

    RepositoryResult<core::Unit> DeleteLockBefore(const std::string& key, int64_t deadline_ms) {
        if (Now() >= deadline_ms) {
            return core::make_unexpected(
                agentenv::snapshot::repository::RepositoryError::Backend("delete", "deadline passed"));
        }
        Guard guard(mu_);
        objects_.erase(key);
        return core::Unit();
    }

    uint64_t Len(const std::string& key) {
        Guard guard(mu_);
        return LenUnlocked(key);
    }
    std::size_t ObjectCount() {
        Guard guard(mu_);
        return objects_.size();
    }
    void LoseNextReply() {
        Guard guard(mu_);
        lose_next_reply_ = true;
    }
    int Appends() {
        Guard guard(mu_);
        return appends_;
    }
    void Seed(const std::string& key, const std::string& bytes) {
        Guard guard(mu_);
        objects_[key] = bytes;
    }

 private:
    uint64_t LenUnlocked(const std::string& key) const {
        const std::map<std::string, std::string>::const_iterator found = objects_.find(key);
        return found == objects_.end() ? 0 : static_cast<uint64_t>(found->second.size());
    }
    void Elapse(int64_t ms) {
        if (clock_ != 0) clock_->now += ms;
    }
    /// The real monotonic clock when no virtual one was given: a lease taken
    /// on the real clock has a real deadline, and comparing it against 0
    /// would make every release look timely.
    int64_t Now() const {
        if (clock_ != 0) return clock_->now;
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    }

    VirtualClock* clock_;
    Mutex mu_;
    std::map<std::string, std::string> objects_;
    bool lose_next_reply_;
    int appends_;
};

}  // namespace

// ---------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------

// Rust `only_frames_landing_where_they_claim_hold_the_lock`.
MT_TEST(only_frames_landing_where_they_claim_hold_the_lock) {
    const lease::LockState free;
    std::string object = free.Claim("a");
    const lease::LockState held = lease::LockState::Parse(object);
    MT_EXPECT_TRUE(held.holder.has_value());
    MT_EXPECT_EQ(*held.holder, std::string("a"));

    // Claimed from an outdated view, so it lands past its offset.
    object += free.Claim("b");
    MT_EXPECT_EQ(*lease::LockState::Parse(object).holder, std::string("a"));

    // Claimed from a lock object that was deleted and grew back to the same
    // length under another holder. The offset matches, so only naming the
    // holder it replaces keeps this claim out.
    const lease::LockState recreated = lease::LockState::Parse(free.Claim("c"));
    MT_EXPECT_EQ(recreated.len, held.len);
    object = free.Claim("a");
    object += recreated.Claim("d");
    const lease::LockState state = lease::LockState::Parse(object);
    MT_EXPECT_EQ(*state.holder, std::string("a"));

    // Claimed from the current view, so it takes the lock.
    object += state.Claim("e");
    MT_EXPECT_EQ(*lease::LockState::Parse(object).holder, std::string("e"));
}

// The field names and the newline are what a Rust node reads, so they are
// asserted literally rather than through a round trip, which would pass just
// as well on names only this build understands.
MT_TEST(a_claim_is_one_json_line_in_the_shape_rust_writes) {
    const lease::LockState free;
    const std::string first = free.Claim("a");
    MT_EXPECT_EQ(first, std::string("{\"at\":0,\"prev\":null,\"token\":\"a\"}\n"));

    // The next claim's offset is the first line's length, so the two agree on
    // where an append lands without either having to be told.
    const lease::LockState held = lease::LockState::Parse(first);
    MT_EXPECT_EQ(held.len, static_cast<uint64_t>(first.size()));
    MT_EXPECT_EQ(held.Claim("b"),
                 std::string("{\"at\":33,\"prev\":\"a\",\"token\":\"b\"}\n"));
}

MT_TEST(a_line_this_version_cannot_read_still_counts_towards_offsets) {
    // Another version may append lines this one cannot parse. They hold no
    // lock, but they do occupy offsets, so a later frame has to claim past
    // them -- counting them is what keeps the two views of the object agreeing
    // on where the next append lands.
    const lease::LockState free;
    const std::string alien = "{\"at\":0,\"unknown\":true}\n";
    std::string object = alien;
    lease::LockState state = lease::LockState::Parse(object);
    MT_EXPECT_TRUE(!state.holder.has_value());
    MT_EXPECT_EQ(state.len, static_cast<uint64_t>(alien.size()));

    // A claim made from that state lands after the unreadable line.
    object += state.Claim("a");
    MT_EXPECT_EQ(*lease::LockState::Parse(object).holder, std::string("a"));

    // Whereas one that ignored it does not.
    object = alien + free.Claim("b");
    MT_EXPECT_TRUE(!lease::LockState::Parse(object).holder.has_value());
}

MT_TEST(a_frame_with_an_unusable_field_is_not_a_frame) {
    // Each of these would hold the lock if the field were merely defaulted,
    // which is why the line is rejected instead.
    const char* const rejected[] = {
        "{\"at\":\"0\",\"prev\":null,\"token\":\"a\"}\n",  // not a number
        "{\"at\":-1,\"prev\":null,\"token\":\"a\"}\n",      // not an offset
        "{\"prev\":null,\"token\":\"a\"}\n",                // no offset at all
        "{\"at\":0,\"prev\":null}\n",                       // no holder named
        "{\"at\":0,\"prev\":7,\"token\":\"a\"}\n",          // not a holder name
        "not json\n",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!lease::LockState::Parse(rejected[i]).holder.has_value());
    }

    // A missing `prev` means a free lock, as serde's `Option<String>` does.
    MT_EXPECT_EQ(*lease::LockState::Parse("{\"at\":0,\"token\":\"a\"}\n").holder,
                 std::string("a"));
}

// ---------------------------------------------------------------------------
// acquiring and releasing
// ---------------------------------------------------------------------------

MT_TEST(acquiring_a_free_lock_takes_it_in_one_append) {
    VirtualClock clock;
    FakeStore store(&clock);

    const RepositoryResult<lease::Lease> taken =
        lease::Acquire(store, "doc.json", clock.Handle());
    MT_EXPECT_TRUE(taken.ok());
    // Landing at the start of an absent object needs no read back.
    MT_EXPECT_EQ(store.Appends(), 1);
    // Dated from when the append was *sent* -- clock zero -- not from when it
    // came back. The two round trips the append cost do not extend the lease:
    // the holder it replaced started counting from the send too.
    MT_EXPECT_EQ(taken.value().deadline_ms(),
                 static_cast<int64_t>(lease::kLeaseMs - lease::kMarginMs));
    MT_EXPECT_TRUE(clock.now == 2 * kLatencyMs);

    // One frame in the object, and it holds the lock.
    const RepositoryResult<core::Optional<std::string> > object =
        store.ReadLock("locks/doc.json");
    MT_EXPECT_TRUE(object.ok() && object.value().has_value());
    const lease::LockState state = lease::LockState::Parse(*object.value());
    MT_EXPECT_TRUE(state.holder.has_value());
    MT_EXPECT_EQ(state.len, static_cast<uint64_t>(object.value()->size()));
}

// Rust `an_append_reported_lost_still_takes_the_lock`.
MT_TEST(an_append_reported_lost_still_takes_the_lock) {
    VirtualClock clock;
    FakeStore store(&clock);
    store.LoseNextReply();

    const RepositoryResult<lease::Lease> taken =
        lease::Acquire(store, "doc.json", clock.Handle());
    MT_EXPECT_TRUE(taken.ok());
    // The append that reported nothing had landed, and the read back is what
    // found it. Trusting the reply instead would have appended a second claim
    // from an outdated view, which could never take the lock.
    MT_EXPECT_EQ(store.Appends(), 1);
    // Still dated from that append's send, even though it took a read back to
    // find out it had landed.
    MT_EXPECT_EQ(taken.value().deadline_ms(),
                 static_cast<int64_t>(lease::kLeaseMs - lease::kMarginMs));
}

MT_TEST(a_released_lock_is_free_and_its_object_gone) {
    VirtualClock clock;
    FakeStore store(&clock);

    const RepositoryResult<lease::Lease> held =
        lease::Acquire(store, "doc.json", clock.Handle());
    MT_EXPECT_TRUE(held.ok());
    lease::Release(store, held.value(), clock.Handle());
    MT_EXPECT_EQ(store.ObjectCount(), static_cast<std::size_t>(0));

    // Free again straight away, rather than after the lease runs out.
    const int64_t before = clock.now;
    const RepositoryResult<lease::Lease> next =
        lease::Acquire(store, "doc.json", clock.Handle());
    MT_EXPECT_TRUE(next.ok());
    MT_EXPECT_TRUE(clock.now - before < lease::kLeaseMs);
}

// Rust `a_lock_whose_holder_died_is_taken_over_after_the_lease`.
MT_TEST(a_lock_whose_holder_died_is_taken_over_after_the_lease) {
    VirtualClock clock;
    FakeStore store(&clock);
    // Acquired and then abandoned without releasing, as a holder that died
    // leaves it.
    MT_EXPECT_TRUE(lease::Acquire(store, "doc.json", clock.Handle()).ok());

    const int64_t started = clock.now;
    const RepositoryResult<lease::Lease> taken =
        lease::Acquire(store, "doc.json", clock.Handle());
    MT_EXPECT_TRUE(taken.ok());
    // Waited the full lease on the dead holder, and not much more: the
    // take-over is what keeps one crash from blocking the document forever.
    MT_EXPECT_TRUE(clock.now - started >= lease::kLeaseMs);
    MT_EXPECT_TRUE(clock.now - started < lease::kLeaseMs + 1000);
    // And the lock it took is usable, not already expired.
    MT_EXPECT_TRUE(taken.value().deadline_ms() > clock.now);

    lease::Release(store, taken.value(), clock.Handle());
    MT_EXPECT_EQ(store.ObjectCount(), static_cast<std::size_t>(0));
}

// An append that lands where the claim said still has to be read back, unless
// it landed at offset 0 of an object that did not exist. Anywhere else the
// object may have been deleted and grown back to the same length under another
// holder since it was read, in which case the offset is free but the holder
// the claim names is gone -- so the frame holds nothing, and only a read
// tells.
MT_TEST(an_append_that_landed_where_it_claimed_is_still_read_back) {
    // Two holders whose tokens are the length of a real one, so the lock
    // object can be swapped for an equally long one and every offset in it
    // stays where it was.
    const std::string first = "11111111-1111-1111-1111-111111111111";
    const std::string usurper = "22222222-2222-2222-2222-222222222222";
    const std::string first_claim = lease::LockState().Claim(first);
    const std::string usurper_claim = lease::LockState().Claim(usurper);
    MT_EXPECT_EQ(first_claim.size(), usurper_claim.size());

    /// Deletes the lock and lets it grow back to the same length under
    /// another holder, in the window between a claim's offset being read and
    /// the claim landing. `after` is how many appends go through untouched
    /// first, which is what puts the swap on the claim that follows a
    /// take-over rather than on the first one.
    class RecreatingStore : public FakeStore {
     public:
        RecreatingStore(VirtualClock* clock, const std::string& replacement, int after)
            : FakeStore(clock), replacement_(replacement), remaining_(after), swapped_(false) {}
        RepositoryResult<core::Optional<uint64_t> > AppendLock(const std::string& key,
                                                               const std::string& data) {
            if (remaining_ > 0) {
                --remaining_;
            } else if (!swapped_) {
                swapped_ = true;
                Seed(key, replacement_);
            }
            return FakeStore::AppendLock(key, data);
        }

     private:
        std::string replacement_;
        int remaining_;
        bool swapped_;
    };

    VirtualClock clock;
    // The replacement is as long as what the claim was computed against, and
    // its second line is a frame that counts for nothing -- so the offset the
    // claim names is still free while the holder it names is gone.
    RecreatingStore store(&clock, usurper_claim + first_claim, 1);
    const std::string key = "locks/doc.json";
    store.Seed(key, first_claim);

    // The holder never releases, so the lock is taken over; the claim that
    // follows lands exactly where it said, into a different object.
    const RepositoryResult<lease::Lease> taken =
        lease::Acquire(store, "doc.json", clock.Handle());
    MT_EXPECT_TRUE(taken.ok());

    const RepositoryResult<core::Optional<std::string> > object = store.ReadLock(key);
    MT_EXPECT_TRUE(object.ok() && object.value().has_value());
    const lease::LockState state = lease::LockState::Parse(*object.value());
    MT_EXPECT_TRUE(state.holder.has_value());
    // The lease that came back is backed by the object, not by a reply about
    // an offset the usurper had already taken.
    MT_EXPECT_TRUE(state.holder.has_value() && *state.holder != usurper);
    MT_EXPECT_TRUE(state.holder.has_value() && *state.holder != first);
}

// Rust `a_lease_past_its_deadline_is_left_to_run_out`.
MT_TEST(a_lease_past_its_deadline_is_left_to_run_out) {
    VirtualClock clock;
    FakeStore store(&clock);
    const RepositoryResult<lease::Lease> held =
        lease::Acquire(store, "doc.json", clock.Handle());
    MT_EXPECT_TRUE(held.ok());

    clock.now = held.value().deadline_ms();
    lease::Release(store, held.value(), clock.Handle());

    // The object stays: deleting it now could remove a lock a contender has
    // since taken, and `Release` reports nothing because the write this lease
    // guarded has already landed.
    MT_EXPECT_EQ(store.ObjectCount(), static_cast<std::size_t>(1));
}

// Upstream's equivalent test hangs the store and relies on the whole acquire
// being cancelled. Nothing here can cancel a blocking call, so the budget is
// checked between attempts -- and this is the shape that reaches it: a lock
// whose holder keeps changing, so no single holder is ever seen long enough
// to be taken over.
MT_TEST(acquiring_gives_up_on_a_lock_whose_holder_keeps_changing) {
    VirtualClock clock;

    /// Rewrites the lock under a fresh holder on every read.
    class ChurningStore : public FakeStore {
     public:
        explicit ChurningStore(VirtualClock* clock) : FakeStore(clock), next_(0) {}
        RepositoryResult<core::Optional<std::string> > ReadLock(const std::string& key) {
            std::ostringstream holder;
            holder << "holder-" << next_++;
            Seed(key, lease::LockState().Claim(holder.str()));
            return FakeStore::ReadLock(key);
        }
     private:
        int next_;
    };

    ChurningStore store(&clock);
    // Held to begin with, so the first append cannot simply take a free lock.
    store.Seed("locks/doc.json", lease::LockState().Claim("holder-0"));
    const int64_t started = clock.now;
    const RepositoryResult<lease::Lease> refused =
        lease::Acquire(store, "doc.json", clock.Handle());

    MT_EXPECT_TRUE(!refused.ok());
    MT_EXPECT_TRUE(!refused.ok() &&
                   refused.error().kind == RepositoryErrorKind::InvalidRequest);
    // Gave up on the budget rather than spinning forever, and named the key.
    MT_EXPECT_TRUE(!refused.ok() &&
                   refused.error().message.find("locks/doc.json") != std::string::npos);
    MT_EXPECT_TRUE(clock.now - started >= lease::kAcquireTimeoutMs);
}

// A store that cannot append at all is reported as such, not as a transient
// backend failure: retrying it would never succeed, and a caller that can
// fall back needs to be able to tell the difference.
MT_TEST(a_store_that_cannot_append_is_refused_as_unsupported) {
    class NoAppend : public lease::LockStore {
     public:
        RepositoryResult<core::Optional<std::string> > ReadLock(const std::string&) {
            return core::Optional<std::string>();
        }
        RepositoryResult<core::Optional<uint64_t> > AppendLock(const std::string&,
                                                               const std::string&) {
            return core::make_unexpected(
                agentenv::snapshot::repository::RepositoryError::Unsupported("appending"));
        }
        RepositoryResult<core::Unit> DeleteLockBefore(const std::string&, int64_t) {
            return core::Unit();
        }
    };

    NoAppend store;
    VirtualClock clock;
    const RepositoryResult<lease::Lease> refused =
        lease::Acquire(store, "doc.json", clock.Handle());
    MT_EXPECT_TRUE(!refused.ok());
    MT_EXPECT_TRUE(!refused.ok() && refused.error().kind == RepositoryErrorKind::Unsupported);
}

// ---------------------------------------------------------------------------
// mutual exclusion
//
// On real threads and the real clock: what the lease is for is two appends at
// one offset, and a virtual clock cannot produce that.
// ---------------------------------------------------------------------------

namespace {

struct WriterArgs {
    FakeStore* store;
    /// The document the writers race on, guarded by nothing: the lease is
    /// what has to keep them apart, so a lost update shows up as a count
    /// short of the number of writers.
    int* document;
    Mutex* seen;
    bool failed;
    bool missed_deadline;
};

void* Writer(void* raw) {
    WriterArgs* args = static_cast<WriterArgs*>(raw);
    // A short injected sleep instead of the real backoff: the schedule is
    // asserted elsewhere, and waiting it out here would make the suite slow
    // without making the race any likelier.
    lease::LeaseClock clock;
    clock.sleep_ms = [](int64_t) { ::usleep(200); };

    const RepositoryResult<lease::Lease> held = lease::Acquire(*args->store, "doc.json", clock);
    if (!held.ok()) {
        args->failed = true;
        return 0;
    }
    int read;
    {
        Guard guard(*args->seen);
        read = *args->document;
    }
    ::usleep(1000);
    {
        Guard guard(*args->seen);
        *args->document = read + 1;
    }
    lease::Release(*args->store, held.value(), clock);
    return 0;
}

}  // namespace

// Rust `concurrent_writers_lose_no_update`.
MT_TEST(concurrent_writers_lose_no_update) {
    const int kWriters = 16;
    FakeStore store(0);
    int document = 0;
    Mutex seen;

    std::vector<WriterArgs> args(kWriters);
    std::vector<pthread_t> threads(kWriters);
    for (int i = 0; i < kWriters; ++i) {
        args[i].store = &store;
        args[i].document = &document;
        args[i].seen = &seen;
        args[i].failed = false;
        args[i].missed_deadline = false;
        MT_EXPECT_EQ(pthread_create(&threads[i], 0, Writer, &args[i]), 0);
    }
    for (int i = 0; i < kWriters; ++i) {
        pthread_join(threads[i], 0);
        MT_EXPECT_TRUE(!args[i].failed);
    }

    // Every writer read a value no other writer was about to overwrite.
    MT_EXPECT_EQ(document, kWriters);
    // And every one of them released, so nothing was left to run out.
    MT_EXPECT_EQ(store.ObjectCount(), static_cast<std::size_t>(0));
}

MT_MAIN
