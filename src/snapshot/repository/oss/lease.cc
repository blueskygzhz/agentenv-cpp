// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/lease.rs
#include "agentenv/snapshot/repository/oss/lease.h"

#include <ctime>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/identity.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/snapshot/repository/oss/documents.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {
namespace lease {

namespace {
/// The same values as the exported constants, but as constant expressions:
/// the margin/attempt relation has to hold at compile time and an
/// `extern const` is not one.
enum {
    kLeaseMsValue = 30000,
    kMarginMsValue = 10000,
    kAcquireTimeoutMsValue = 75000,
    kWriteAttemptTimeoutSecsValue = 4
};
}  // namespace

const int64_t kLeaseMs = kLeaseMsValue;
const int64_t kMarginMs = kMarginMsValue;
const int64_t kAcquireTimeoutMs = kAcquireTimeoutMsValue;
const int kWriteAttemptTimeoutSecs = kWriteAttemptTimeoutSecsValue;
const char* const kLocksPrefix = "locks/";

// Rust's `const _: () = assert!(MARGIN.as_secs() > 2 * WRITE_ATTEMPT_TIMEOUT)`,
// checked here for the same reason: a write may take a second attempt after
// refreshing credentials, and both must finish inside the margin or a holder
// could still be writing when a contender takes over. Raising the attempt
// timeout without raising the margin breaks the build rather than the lease.
static_assert(kMarginMsValue > 2 * 1000 * kWriteAttemptTimeoutSecsValue,
              "the lease margin must outlast two write attempts");

namespace {

int64_t RealNowMs() {
    timespec now;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &now);
#else
    clock_gettime(CLOCK_REALTIME, &now);
#endif
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

int64_t NowMs(const LeaseClock& clock) { return clock.now_ms ? clock.now_ms() : RealNowMs(); }

/// `75s`, as Rust's `{Duration:?}` prints it.
std::string Seconds(int64_t ms) {
    std::ostringstream text;
    text << (ms / 1000) << "s";
    return text.str();
}

bool SameHolder(const core::Optional<std::string>& left,
                const core::Optional<std::string>& right) {
    if (left.has_value() != right.has_value()) return false;
    return !left.has_value() || *left == *right;
}

}  // namespace

std::string Frame::Encode() const {
    core::JsonObject object;
    object["at"] = core::Json(static_cast<int64_t>(at));
    // Written even when absent: a free lock is `"prev":null`, which is what
    // upstream's `Option<String>` serialises to, and a lock shared with a Rust
    // node has to read the same either way.
    object["prev"] = prev.has_value() ? core::Json(*prev) : core::Json();
    object["token"] = core::Json(token);
    return core::Json(object).ToString() + "\n";
}

core::Optional<Frame> Frame::Decode(const std::string& line) {
    const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(line);
    if (!json.ok() || json.value().kind() != core::Json::Kind::Object) {
        return core::Optional<Frame>();
    }
    const core::JsonObject& object = json.value().as_object();

    const core::JsonObject::const_iterator at = object.find("at");
    if (at == object.end() || at->second.kind() != core::Json::Kind::Int) {
        return core::Optional<Frame>();
    }
    // A negative offset is not an offset. Upstream's `u64` rejects it in the
    // deserializer; casting it would wrap into an enormous one and make the
    // frame count against a length nothing can reach.
    if (at->second.as_int() < 0) return core::Optional<Frame>();

    const core::JsonObject::const_iterator token = object.find("token");
    if (token == object.end() || token->second.kind() != core::Json::Kind::String) {
        return core::Optional<Frame>();
    }

    Frame frame;
    frame.at = static_cast<uint64_t>(at->second.as_int());
    frame.token = token->second.as_string();

    // Missing and null both mean a free lock, matching serde's treatment of
    // `Option<String>`. Anything else is not a holder name, so the line is
    // not a frame.
    const core::JsonObject::const_iterator prev = object.find("prev");
    if (prev != object.end() && prev->second.kind() != core::Json::Kind::Null) {
        if (prev->second.kind() != core::Json::Kind::String) return core::Optional<Frame>();
        frame.prev = core::Optional<std::string>(prev->second.as_string());
    }
    return core::Optional<Frame>(frame);
}

LockState LockState::Parse(const std::string& object) {
    LockState state;
    // Rust `split_inclusive(b'\n')`: the newline belongs to the line before
    // it, and a trailing newline does not start an empty last line. The
    // lengths have to come out exactly right -- they are the offsets frames
    // claim -- so this cannot be a plain tokenising split.
    std::size_t pos = 0;
    while (pos < object.size()) {
        const std::size_t newline = object.find('\n', pos);
        const std::size_t end = newline == std::string::npos ? object.size() : newline + 1;
        const std::string line = object.substr(pos, end - pos);

        const core::Optional<Frame> frame = Frame::Decode(line);
        // A frame counts only where it landed and only against the holder it
        // named, which is what makes an append from an outdated view inert.
        if (frame.has_value() && frame->at == state.len &&
            SameHolder(frame->prev, state.holder)) {
            state.holder = core::Optional<std::string>(frame->token);
        }
        // Counted even when the line was not a frame: another version may
        // write lines this one cannot read, and they still occupy offsets.
        state.len += static_cast<uint64_t>(line.size());
        pos = end;
    }
    return state;
}

std::string LockState::Claim(const std::string& token) const {
    Frame frame;
    frame.at = len;
    frame.prev = holder;
    frame.token = token;
    return frame.Encode();
}

namespace {

/// Rust `take`.
RepositoryResult<Lease> Take(LockStore& store, const std::string& key,
                             const LeaseClock& clock) {
    const int64_t give_up_at = NowMs(clock) + kAcquireTimeoutMs;
    const RepositoryError timed_out = RepositoryError::InvalidRequest(
        "'" + key + "' stayed locked for " + Seconds(kAcquireTimeoutMs));

    Backoff backoff(Backoff::Jitter(), clock.sleep_ms);
    // Every append is a separate attempt, so a holder knows which of its
    // appends took the lock, and when it was sent.
    std::vector<std::pair<std::string, int64_t> > attempts;
    // The holder this node has seen without a break, and since when.
    core::Optional<std::pair<std::string, int64_t> > waiting_on;
    // Optimistically, the lock is free and its object absent.
    LockState state;

    while (true) {
        // Upstream cancels the request in flight here; this can only refuse
        // to start another one, so the bound includes one request more.
        if (NowMs(clock) >= give_up_at) return core::make_unexpected(timed_out);

        const std::string token = core::Uuid::GenV7().ToString();
        const int64_t sent = NowMs(clock);
        attempts.push_back(std::make_pair(token, sent));

        const RepositoryResult<core::Optional<uint64_t> > landed =
            store.AppendLock(key, state.Claim(token));
        if (!landed.ok()) {
            // An append the store cannot do at all is reported as it is: the
            // reason is the store's, and wrapping it as a backend failure
            // would read as a transient one worth retrying.
            if (landed.error().kind == RepositoryErrorKind::Unsupported) {
                return core::make_unexpected(landed.error());
            }
            return core::make_unexpected(
                RepositoryError::Backend("append to '" + key + "'", landed.error().message));
        }
        // Landing at the start of an absent object leaves this frame alone in
        // it. Anywhere else, the object may have been deleted and grown back
        // since it was read, so only a new read tells.
        if (state.len == 0 && landed.value().has_value() && *landed.value() == 0) {
            return Lease(key, sent);
        }

        while (true) {
            const RepositoryResult<core::Optional<std::string> > object = store.ReadLock(key);
            if (!object.ok()) {
                return core::make_unexpected(
                    RepositoryError::Backend("read '" + key + "'", object.error().message));
            }
            state = LockState::Parse(object.value().has_value() ? *object.value()
                                                               : std::string());
            if (!state.holder.has_value()) break;
            const std::string& holder = *state.holder;

            bool ours = false;
            int64_t our_send = 0;
            for (std::size_t i = 0; i < attempts.size(); ++i) {
                if (attempts[i].first == holder) {
                    ours = true;
                    our_send = attempts[i].second;
                    break;
                }
            }
            // Dated from the send, not from now: a reply this slow has
            // already eaten into the lease.
            if (ours) return Lease(key, our_send);

            if (!waiting_on.has_value() || waiting_on->first != holder) {
                waiting_on = core::Optional<std::pair<std::string, int64_t> >(
                    std::make_pair(holder, NowMs(clock)));
            }
            const int64_t since = waiting_on->second;
            if (NowMs(clock) - since >= kLeaseMs) {
                AGENTENV_WARN("taking over a lock its holder kept too long: key=%s holder=%s",
                              key.c_str(), holder.c_str());
                break;
            }
            if (NowMs(clock) >= give_up_at) return core::make_unexpected(timed_out);
            backoff.Wait();
        }
    }
}

}  // namespace

RepositoryResult<Lease> Acquire(LockStore& store, const std::string& document,
                                const LeaseClock& clock) {
    return Take(store, std::string(kLocksPrefix) + document, clock);
}

RepositoryResult<Lease> Acquire(LockStore& store, const std::string& document) {
    return Acquire(store, document, LeaseClock());
}

void Release(LockStore& store, const Lease& lease, const LeaseClock& clock) {
    (void)clock;
    const RepositoryResult<core::Unit> deleted =
        store.DeleteLockBefore(lease.key(), lease.deadline_ms());
    if (!deleted.ok()) {
        // Left to run out rather than reported: the write this lease guarded
        // has already landed, and failing here would make it look like it had
        // not.
        AGENTENV_WARN("left a lease to run out: key=%s error=%s", lease.key().c_str(),
                      deleted.error().message.c_str());
    }
}

void Release(LockStore& store, const Lease& lease) { Release(store, lease, LeaseClock()); }

}  // namespace lease
}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
