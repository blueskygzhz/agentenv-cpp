// SPDX-License-Identifier: MIT
#include "agentenv/core/executor.h"

namespace agentenv {
namespace core {

Executor::Executor(std::size_t nthreads) : shutdown_(false) {
    if (nthreads == 0) {
        nthreads = std::thread::hardware_concurrency();
        if (nthreads == 0) nthreads = 4;
    }
    for (std::size_t i = 0; i < nthreads; ++i) {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
}

Executor::~Executor() { Shutdown(); }

void Executor::Post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lg(mu_);
        if (shutdown_) return;
        queue_.emplace_back(std::move(fn));
    }
    cv_.notify_one();
}

void Executor::Shutdown() {
    {
        std::lock_guard<std::mutex> lg(mu_);
        if (shutdown_) return;
        shutdown_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
}

void Executor::WorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lg(mu_);
            cv_.wait(lg, [&]() { return shutdown_ || !queue_.empty(); });
            if (queue_.empty()) return;  // shutdown & drained
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        try { task(); }
        catch (...) { /* swallow — log with real logging integration */ }
    }
}

Executor& GlobalExecutor() {
    static Executor inst;
    return inst;
}

}  // namespace core
}  // namespace agentenv
