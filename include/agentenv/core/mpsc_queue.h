// SPDX-License-Identifier: MIT
// Rust: `crossbeam::channel::{unbounded, bounded}`.
// C++11: mutex + condvar + std::deque.
#ifndef AGENTENV_CORE_MPSC_QUEUE_H_
#define AGENTENV_CORE_MPSC_QUEUE_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

#include "agentenv/core/optional.h"

namespace agentenv {
namespace core {

enum class RecvError { Closed };

template <typename T>
class MpscQueue {
 public:
    /// capacity == 0 => unbounded.
    explicit MpscQueue(std::size_t capacity = 0)
        : capacity_(capacity), closed_(false) {}

    /// Blocking push. Returns false iff queue is closed.
    bool Push(T value) {
        std::unique_lock<std::mutex> lock(mu_);
        not_full_.wait(lock, [&]() {
            return closed_ || capacity_ == 0 || q_.size() < capacity_;
        });
        if (closed_) return false;
        q_.emplace_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    /// Non-blocking push. Returns false if full or closed.
    bool TryPush(T value) {
        std::unique_lock<std::mutex> lock(mu_);
        if (closed_) return false;
        if (capacity_ != 0 && q_.size() >= capacity_) return false;
        q_.emplace_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    /// Blocking pop. Returns Closed if the queue is closed AND empty.
    /// (Draining semantics: closing does not lose queued items.)
    bool Pop(T* out) {
        std::unique_lock<std::mutex> lock(mu_);
        not_empty_.wait(lock, [&]() { return !q_.empty() || closed_; });
        if (q_.empty()) return false;
        *out = std::move(q_.front());
        q_.pop_front();
        not_full_.notify_one();
        return true;
    }

    Optional<T> TryPop() {
        std::unique_lock<std::mutex> lock(mu_);
        if (q_.empty()) return nullopt;
        T v = std::move(q_.front());
        q_.pop_front();
        not_full_.notify_one();
        return Optional<T>(std::move(v));
    }

    void Close() {
        std::unique_lock<std::mutex> lock(mu_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t Size() const {
        std::unique_lock<std::mutex> lock(mu_);
        return q_.size();
    }

 private:
    mutable std::mutex mu_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> q_;
    std::size_t capacity_;
    bool closed_;
};

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_MPSC_QUEUE_H_
