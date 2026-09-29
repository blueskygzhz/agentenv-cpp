// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/resolver.rs
#include "agentenv/sandbox/network/resolver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace agentenv {
namespace sandbox {
namespace network {

int HostNsFd() {
    // Rust `OnceLock<OwnedFd>` — opened once, then reused forever. The fd must
    // be captured before any thread unshares, otherwise we would cache a
    // sandbox namespace and every lookup would resolve in the wrong place.
    static int fd = -1;
    static std::once_flag once;
    std::call_once(once, []() {
        fd = ::open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
    });
    return fd;
}

// One queued lookup. `done`/`cv` let the waiter be woken as soon as the worker
// finishes, while still polling for cancellation on its own cadence.
struct HostNetResolver::Request {
    std::string hostname;
    uint16_t    port = 0;
    // Rust `SyncSender<Option<Vec<SocketAddr>>>` — `resolved` is only
    // meaningful when `ok` is true.
    bool                      ok = false;
    std::vector<ResolvedAddr> resolved;
    bool                      done = false;
};

struct HostNetResolver::State {
    std::mutex              mu;
    std::condition_variable queue_cv;     // worker waits for work / shutdown
    std::condition_variable done_cv;      // waiters wait for their result
    std::deque<std::shared_ptr<Request> > queue;
    bool                    shutdown = false;
    bool                    worker_ready = false;
    size_t                  in_flight = 0;
};

HostNetResolver::HostNetResolver()
    : state_(new State()), stopped_(false), worker_spawned_(false) {
    // Rust: a failed spawn only disables resolution (warn + empty lifecycle).
    try {
        worker_ = std::thread(&HostNetResolver::RunWorker, this);
        worker_spawned_ = true;
    } catch (...) {
        worker_spawned_ = false;
    }
}

HostNetResolver::~HostNetResolver() { Shutdown(); }

bool HostNetResolver::WorkerReady() const {
    std::lock_guard<std::mutex> g(state_->mu);
    return state_->worker_ready;
}

void HostNetResolver::RunWorker() {
    // Rust: `setns(host_ns_fd(), CLONE_NEWNET)`; on failure it warns and
    // returns, leaving resolution permanently unavailable rather than
    // resolving against the sandbox's DNS.
    const int ns = HostNsFd();
    if (ns < 0 || ::setns(ns, CLONE_NEWNET) != 0) {
        std::lock_guard<std::mutex> g(state_->mu);
        state_->shutdown = true;
        state_->done_cv.notify_all();
        return;
    }
    {
        std::lock_guard<std::mutex> g(state_->mu);
        state_->worker_ready = true;
    }

    std::shared_ptr<State> state = state_;
    while (true) {
        std::shared_ptr<Request> req;
        {
            std::unique_lock<std::mutex> lk(state->mu);
            // Rust `tokio::select!` with `biased;` checks shutdown first.
            state->queue_cv.wait(lk, [&state]() {
                return state->shutdown || !state->queue.empty();
            });
            if (state->shutdown) break;
            req = state->queue.front();
            state->queue.pop_front();
            ++state->in_flight;
        }

        // The actual lookup, outside the lock. IPv4 only, mirroring
        // `LookupIpStrategy::Ipv4Only`.
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = NULL;
        const int rc = ::getaddrinfo(req->hostname.c_str(), NULL, &hints, &res);

        std::vector<ResolvedAddr> resolved;
        if (rc == 0) {
            for (struct addrinfo* it = res; it != NULL; it = it->ai_next) {
                if (it->ai_family != AF_INET) continue;  // filter_map(V4)
                const struct sockaddr_in* sin =
                    reinterpret_cast<const struct sockaddr_in*>(it->ai_addr);
                ResolvedAddr addr;
                addr.ipv4 = ntohl(sin->sin_addr.s_addr);
                addr.port = req->port;
                resolved.push_back(addr);
            }
        }
        if (res) ::freeaddrinfo(res);

        {
            std::lock_guard<std::mutex> g(state->mu);
            // Rust maps a lookup error to None; a successful lookup yields the
            // (possibly empty) V4 list.
            req->ok = (rc == 0);
            req->resolved = resolved;
            req->done = true;
            --state->in_flight;
            state->done_cv.notify_all();
        }
    }

    // Rust `tasks.abort_all()` then drains: release anyone still waiting.
    std::lock_guard<std::mutex> g(state->mu);
    for (size_t i = 0; i < state->queue.size(); ++i) {
        state->queue[i]->done = true;
        state->queue[i]->ok = false;
    }
    state->queue.clear();
    state->done_cv.notify_all();
}

bool HostNetResolver::Resolve(const std::string& hostname, uint16_t port,
                              const std::atomic<bool>* cancel,
                              std::vector<ResolvedAddr>* out) {
    // Rust: `if self.stopped.load(Acquire) { return None; }`
    if (stopped_.load(std::memory_order_acquire)) return false;
    if (!worker_spawned_) return false;  // Rust: `requests.as_ref()?` on None

    std::shared_ptr<Request> req(new Request());
    req->hostname = hostname;
    req->port = port;

    {
        std::lock_guard<std::mutex> g(state_->mu);
        if (state_->shutdown) return false;
        // Rust `try_send(..).ok()?` — a full bounded queue is an immediate
        // failure, never backpressure on the proxy's accept loop.
        if (state_->queue.size() >= kResolverQueueCapacity) return false;
        // Rust caps concurrency with a semaphore; queued-but-not-started work
        // counts against the same budget here.
        if (state_->in_flight >= kResolverMaxInFlight &&
            state_->queue.size() + state_->in_flight >= kResolverQueueCapacity) {
            return false;
        }
        state_->queue.push_back(req);
        state_->queue_cv.notify_one();
    }

    // Rust: deadline = now + RESPONSE_TIMEOUT, polled at STOP_POLL granularity
    // so stop/cancel are observed promptly.
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kResolverResponseTimeoutMs);

    std::unique_lock<std::mutex> lk(state_->mu);
    while (true) {
        if (req->done) {
            if (!req->ok) return false;
            if (out) *out = req->resolved;
            return true;
        }
        if (stopped_.load(std::memory_order_acquire) || state_->shutdown) return false;
        if (cancel && cancel->load(std::memory_order_acquire)) return false;

        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;  // Rust: remaining.is_zero()
        // Rust: `remaining.min(RESOLVER_STOP_POLL)`
        std::chrono::milliseconds slice(kResolverStopPollMs);
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining < slice) slice = remaining;
        state_->done_cv.wait_for(lk, slice);
    }
}

void HostNetResolver::Shutdown() {
    // Rust: `if self.stopped.swap(true, AcqRel) { return; }` — idempotent.
    if (stopped_.exchange(true, std::memory_order_acq_rel)) return;

    {
        std::lock_guard<std::mutex> g(state_->mu);
        state_->shutdown = true;
        // Drop the queue so waiters stop instead of riding out the deadline.
        for (size_t i = 0; i < state_->queue.size(); ++i) {
            state_->queue[i]->done = true;
            state_->queue[i]->ok = false;
        }
        state_->queue.clear();
        state_->queue_cv.notify_all();
        state_->done_cv.notify_all();
    }

    if (worker_spawned_ && worker_.joinable()) worker_.join();
}

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
