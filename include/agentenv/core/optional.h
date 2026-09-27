// SPDX-License-Identifier: MIT
// Rust: `Option<T>`
// C++11: hand-written; C++17 std::optional supersedes this.
#ifndef AGENTENV_CORE_OPTIONAL_H_
#define AGENTENV_CORE_OPTIONAL_H_

#include <cassert>
#include <new>
#include <type_traits>
#include <utility>

namespace agentenv {
namespace core {

struct nullopt_t { struct init{}; explicit nullopt_t(init) {} };
static const nullopt_t nullopt{ nullopt_t::init{} };

template <typename T>
class Optional {
 public:
    Optional() : has_(false) {}
    Optional(nullopt_t) : has_(false) {}
    Optional(const T& v) : has_(true) { new (&s_) T(v); }
    Optional(T&& v)      : has_(true) { new (&s_) T(std::move(v)); }

    Optional(const Optional& o) : has_(o.has_) {
        if (has_) new (&s_) T(*o); }
    Optional(Optional&& o) noexcept : has_(o.has_) {
        if (has_) new (&s_) T(std::move(*o)); }
    ~Optional() { reset(); }

    Optional& operator=(const Optional& o) {
        if (this == &o) return *this;
        reset(); has_ = o.has_;
        if (has_) new (&s_) T(*o);
        return *this;
    }
    Optional& operator=(Optional&& o) noexcept {
        reset(); has_ = o.has_;
        if (has_) new (&s_) T(std::move(*o));
        return *this;
    }
    Optional& operator=(nullopt_t) { reset(); return *this; }

    void reset() { if (has_) { get().~T(); has_ = false; } }
    bool has_value() const { return has_; }
    explicit operator bool() const { return has_; }

    T& operator*()             { assert(has_); return get(); }
    const T& operator*() const { assert(has_); return get(); }
    T* operator->()             { assert(has_); return &get(); }
    const T* operator->() const { assert(has_); return &get(); }

    T value_or(T fallback) const { return has_ ? get() : fallback; }

 private:
    T&       get()       { return *reinterpret_cast<T*>(&s_); }
    const T& get() const { return *reinterpret_cast<const T*>(&s_); }

    typename std::aligned_storage<sizeof(T), alignof(T)>::type s_;
    bool has_;
};

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_OPTIONAL_H_
