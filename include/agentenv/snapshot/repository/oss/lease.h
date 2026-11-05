// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/lease.rs
//
// Leases that let one node at a time write a shared document on Alibaba OSS,
// which cannot replace an object only if it is unchanged.
//
// Alibaba OSS appends to an object only at its current length, so of two
// appends at one length exactly one lands. A document's lock is an appendable
// object of JSON lines under `locks/`, a prefix older nodes never list. Each
// line is a `Frame` that claims the lock for one attempt, naming the offset it
// expects to land at and the holder it replaces. A frame counts only if it
// landed there while that holder held the lock, and the last frame that counts
// holds it, so appends made from an outdated view never take the lock.
// Releasing deletes the lock object.
//
// A holder that dies never releases, so a contender that has seen the same
// holder for `kLeaseMs` takes the lock over. Nothing can stop a holder that is
// merely slow, so holders bound themselves: every write a holder makes starts
// before `Lease::deadline_ms`, `kMarginMs` before any contender may take over,
// and ends within it. Each node measures time on its own monotonic clock.
//
// Porting note. Upstream wraps the whole acquire in `tokio::time::timeout`,
// which *cancels* the request in flight. Nothing here can cancel a blocking
// call, so the budget is checked between attempts instead and the bound
// becomes `kAcquireTimeoutMs` plus the one request already in flight. That is
// only a bound, not a behaviour change, because every request a lease makes is
// itself bounded by `kWriteAttemptTimeoutSecs` -- which is also why
// `kMarginMs` must stay larger than two of those.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_OSS_LEASE_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_OSS_LEASE_H_

#include <cstdint>
#include <functional>
#include <string>

#include "agentenv/core/optional.h"
#include "agentenv/snapshot/repository/errors.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {
namespace lease {

/// Rust `LEASE` — how long a contender waits on one holder before taking its
/// lock over.
extern const int64_t kLeaseMs;
/// Rust `MARGIN` — how long before a contender may take over a holder's
/// writes must start.
///
/// Must exceed two write attempts, because a write may take a second one
/// after refreshing credentials; `lease.cc` asserts that against the client's
/// per-attempt timeout.
extern const int64_t kMarginMs;
/// Rust `ACQUIRE_TIMEOUT` — long enough to outwait one holder that died, and
/// contention after it.
extern const int64_t kAcquireTimeoutMs;
/// Rust `client::WRITE_ATTEMPT_TIMEOUT` — the bound every write made under a
/// lease must respect, so that a holder cannot still be writing once a
/// contender may take over.
///
/// Lives here rather than with the client because it is the lease that
/// depends on it: `lease.cc` asserts `kMarginMs` outlasts two such attempts.
/// The client does not issue bounded writes yet (see `LockStore::AppendLock`),
/// and whatever eventually does must use this value.
extern const int kWriteAttemptTimeoutSecs;
/// Rust `LOCKS_PREFIX`.
extern const char* const kLocksPrefix;

/// Rust `trait LockStore` — the lock object operations a lease needs.
///
/// A seam rather than a direct dependency on the client, for the same reason
/// it is one upstream: every rule in this file is about what happens when two
/// nodes append at the same offset, and that is only observable against a
/// store a test can make race.
class LockStore {
 public:
    virtual ~LockStore() {}

    /// The object at `key`; absent when there is no such object.
    virtual RepositoryResult<core::Optional<std::string> > ReadLock(
        const std::string& key) = 0;

    /// Rust `OssClient::append` — appends `data` at the object's current
    /// length, creating it when absent.
    ///
    /// Returns the offset `data` landed at, or **absent** when the object grew
    /// while the append was in flight. Those two answers are not
    /// interchangeable: an append that reports no offset may still have
    /// landed, which is why a claim is confirmed by reading the object back
    /// rather than by this return value alone.
    virtual RepositoryResult<core::Optional<uint64_t> > AppendLock(
        const std::string& key, const std::string& data) = 0;

    /// Rust `OssClient::delete_before` — deletes `key`, in attempts that each
    /// start before `deadline_ms` on this node's monotonic clock.
    virtual RepositoryResult<core::Unit> DeleteLockBefore(const std::string& key,
                                                          int64_t deadline_ms) = 0;
};

/// How a lease measures time and waits.
///
/// Injectable because every rule here is a duration: a test that had to wait
/// out a 30-second lease for real could not assert the take-over at all.
struct LeaseClock {
    /// Monotonic milliseconds. Unset means the real clock.
    std::function<int64_t()> now_ms;
    /// Waits `ms`. Unset means a real sleep. A virtual clock advances its own
    /// counter here, which is what makes the waiting observable.
    std::function<void(int64_t ms)> sleep_ms;
};

/// Rust `struct Frame` — one line of a lock object.
struct Frame {
    /// The offset the frame must land at to count.
    uint64_t at;
    /// The holder the frame replaces; absent when the lock is free.
    core::Optional<std::string> prev;
    /// The attempt that appended the frame.
    std::string token;

    Frame() : at(0) {}

    /// The JSON line, newline included. Field names match upstream's `serde`
    /// derive, which is what lets a Rust node and this one share a lock.
    std::string Encode() const;

    /// Absent when the line is not a frame. A line that does not parse is
    /// *skipped* by the state machine, not an error: the object is appended to
    /// by other versions, and a line this build cannot read must still count
    /// towards the offsets.
    static core::Optional<Frame> Decode(const std::string& line);
};

/// Rust `struct LockState` — what a lock object shows.
struct LockState {
    /// The object's length; 0 when absent.
    uint64_t len;
    core::Optional<std::string> holder;

    LockState() : len(0) {}

    /// Rust `LockState::parse`.
    static LockState Parse(const std::string& object);

    /// Rust `LockState::claim` — the frame by which `token` takes the lock
    /// from this state.
    std::string Claim(const std::string& token) const;
};

/// Rust `struct Lease` — a held lock. Dropping it without `Release` leaves it
/// to run out.
class Lease {
 public:
    Lease() : deadline_ms_(0) {}
    /// Rust `Lease::new` — `started_ms` is when the append that took the lock
    /// was sent, not when it was confirmed. Dating the lease from the send is
    /// what keeps a slow confirmation from extending it past the point a
    /// contender may take over.
    Lease(const std::string& key, int64_t started_ms)
        : key_(key), deadline_ms_(started_ms + kLeaseMs - kMarginMs) {}

    const std::string& key() const { return key_; }
    /// Writes made under this lease must start before this.
    int64_t deadline_ms() const { return deadline_ms_; }

 private:
    std::string key_;
    int64_t deadline_ms_;
};

/// Rust `acquire` — takes the lock of the document at `document`, giving up
/// after `kAcquireTimeoutMs`. A claim that lands after that runs out.
RepositoryResult<Lease> Acquire(LockStore& store, const std::string& document,
                                const LeaseClock& clock);
/// `Acquire` on the real clock.
RepositoryResult<Lease> Acquire(LockStore& store, const std::string& document);

/// Rust `release` — releases `lease`, unless a contender may take it over
/// before the release lands; then the lease runs out instead.
///
/// Returns nothing, as upstream: a release that could not be made durable is
/// logged and the lock left to expire, because reporting it would make a
/// write that *did* land look like it failed.
void Release(LockStore& store, const Lease& lease, const LeaseClock& clock);
void Release(LockStore& store, const Lease& lease);

}  // namespace lease
}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_OSS_LEASE_H_
