// SPDX-License-Identifier: MIT
// Rust: `tokio::sync::CancellationToken`.
// C++11: shared atomic bool + condvar for wait.
#ifndef AGENTENV_CORE_CANCEL_TOKEN_H_
#define AGENTENV_CORE_CANCEL_TOKEN_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace agentenv {
namespace core {

class CancelToken {
 public:
    CancelToken() : state_(std::make_shared<State>()) {}

    void Cancel() {
        std::lock_guard<std::mutex> lk(state_->mu);
        state_->cancelled = true;
        state_->cv.notify_all();
    }

    bool IsCancelled() const { return state_->cancelled.load(); }

    /// Block up to `timeout_ms`. Return true if got cancelled, false on timeout.
    bool WaitCancelled(int timeout_ms) const {
        std::unique_lock<std::mutex> lk(state_->mu);
        if (state_->cancelled.load()) return true;
        return state_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                   [&]() { return state_->cancelled.load(); });
    }

 private:
    struct State {
        mutable std::mutex mu;
        std::condition_variable cv;
        std::atomic<bool> cancelled{false};
    };
    std::shared_ptr<State> state_;
};

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_CANCEL_TOKEN_H_
