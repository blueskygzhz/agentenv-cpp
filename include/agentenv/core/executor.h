// SPDX-License-Identifier: MIT
// Rust: `tokio::spawn` on a runtime.
// C++11: a fixed-size thread pool that runs `std::function<void()>` tasks and
// returns `std::future<T>` for typed submissions.
#ifndef AGENTENV_CORE_EXECUTOR_H_
#define AGENTENV_CORE_EXECUTOR_H_

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace agentenv {
namespace core {

class Executor {
 public:
    explicit Executor(std::size_t nthreads = 0);  // 0 -> hardware_concurrency
    ~Executor();

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    /// Fire-and-forget submission.
    void Post(std::function<void()> fn);

    /// Typed submission — returns a future.
    template <typename Fn>
    auto Submit(Fn fn) -> std::future<decltype(fn())> {
        using R = decltype(fn());
        auto pkg = std::make_shared<std::packaged_task<R()>>(std::move(fn));
        auto fut = pkg->get_future();
        Post([pkg]() { (*pkg)(); });
        return fut;
    }

    /// Cooperative shutdown; joins all workers.
    void Shutdown();

    std::size_t nthreads() const { return workers_.size(); }

 private:
    void WorkerLoop();

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool shutdown_;
};

/// Global default executor. Lazily constructed.
Executor& GlobalExecutor();

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_EXECUTOR_H_
