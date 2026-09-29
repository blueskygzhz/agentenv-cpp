// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/resolver.rs — the egress proxy's DNS resolver.
//
// Why a dedicated thread: entering a network namespace is a *per-thread*
// property. The egress proxy runs inside the sandbox namespace, but domain
// policy has to be resolved against the *host's* DNS. So one thread is pinned
// to the host namespace for its whole lifetime and every lookup is funnelled
// through it.
//
// Deviation from Rust: `resolver.rs` uses hickory's async resolver so that a
// shutdown can cancel an in-flight query (libc's `getaddrinfo` is not
// cancellable). C++11 has no async resolver, so the worker uses
// `getaddrinfo` but the *caller* never blocks on it: `Resolve` waits on a
// bounded queue with a 100 ms stop-poll, so shutdown and per-connection
// cancellation stay responsive even while a lookup is still stuck in libc.
#ifndef AGENTENV_SANDBOX_NETWORK_RESOLVER_H_
#define AGENTENV_SANDBOX_NETWORK_RESOLVER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agentenv {
namespace sandbox {
namespace network {

/// Rust `RESOLVER_QUEUE_CAPACITY` — `mpsc::channel(128)`. A full queue makes
/// `try_send` fail, which the caller reports as "unresolvable" rather than
/// queueing without bound.
const size_t kResolverQueueCapacity = 128;
/// Rust `RESOLVER_MAX_IN_FLIGHT` — semaphore permits / `max_active_requests`.
const size_t kResolverMaxInFlight = 16;
/// Rust `RESOLVER_STOP_POLL` = 100ms — how often a waiter re-checks
/// `stopped`/`cancel`.
const int kResolverStopPollMs = 100;
/// Rust `RESOLVER_DNS_TIMEOUT` = 5s — per-query network timeout.
const int kResolverDnsTimeoutMs = 5000;
/// Rust `RESOLVER_RESPONSE_TIMEOUT` = 30s — overall deadline for one
/// `resolve()` call.
const int kResolverResponseTimeoutMs = 30000;

/// Rust `slot::host_ns_fd()` — a process-wide fd for the host network
/// namespace, captured on first use from `/proc/thread-self/ns/net`.
///
/// Captured once and cached so it still refers to the *host* namespace even
/// when later called from a thread that has already entered a sandbox
/// namespace. Returns -1 if it cannot be opened.
int HostNsFd();

/// One resolved IPv4 endpoint. Rust `SocketAddr` restricted to V4, since the
/// resolver forces `LookupIpStrategy::Ipv4Only`.
struct ResolvedAddr {
    uint32_t ipv4 = 0;  // host byte order
    uint16_t port  = 0;

    bool operator==(const ResolvedAddr& o) const {
        return ipv4 == o.ipv4 && port == o.port;
    }
};

/// Rust `struct HostNetResolver`.
class HostNetResolver {
 public:
    /// Rust `HostNetResolver::new` — spawns the host-namespace worker. If the
    /// spawn fails, Rust logs a warning and leaves the lifecycle empty so that
    /// every `resolve` returns None instead of failing construction; this does
    /// the same.
    HostNetResolver();

    /// Rust `impl Drop` — shuts the worker down.
    ~HostNetResolver();

    /// Rust `resolve(hostname, port, cancel)`.
    ///
    /// Returns false for Rust's `None`: resolver stopped, queue full, deadline
    /// exceeded, cancellation requested, or the lookup failed. `cancel` may be
    /// null (Rust `None`) and is polled on the same 100 ms cadence as
    /// `stopped`.
    bool Resolve(const std::string& hostname, uint16_t port,
                 const std::atomic<bool>* cancel,
                 std::vector<ResolvedAddr>* out);

    /// Rust `shutdown` — idempotent (guarded by an atomic swap) and releases
    /// every waiter rather than leaving them on the 30 s deadline.
    void Shutdown();

    /// True once the worker entered the host namespace and is serving.
    bool WorkerReady() const;

 private:
    HostNetResolver(const HostNetResolver&);
    HostNetResolver& operator=(const HostNetResolver&);

    struct Request;
    struct State;

    void RunWorker();

    std::shared_ptr<State> state_;
    std::thread            worker_;
    std::atomic<bool>      stopped_;
    bool                   worker_spawned_;
};

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_NETWORK_RESOLVER_H_
