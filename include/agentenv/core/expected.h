// SPDX-License-Identifier: MIT
// Rust: `Result<T, E>` — used everywhere in AgentENV
// C++11: hand-written value-type Expected. std::expected is C++23; std::variant
//        is C++17; so we implement a minimal aligned-storage tagged union.
#ifndef AGENTENV_CORE_EXPECTED_H_
#define AGENTENV_CORE_EXPECTED_H_

#include <cassert>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace agentenv {
namespace core {

/// Unit type, used as the T in `Expected<Unit, E>` for functions that only signal
/// success or failure — equivalent to Rust `Result<(), E>`.
struct Unit {};

/// Tag types.
template <typename E> struct Unexpected {
    E value;
    explicit Unexpected(E v) : value(std::move(v)) {}
};

/// Convenience constructor: `make_unexpected(err)`.
template <typename E>
Unexpected<typename std::decay<E>::type> make_unexpected(E&& e) {
    return Unexpected<typename std::decay<E>::type>(std::forward<E>(e));
}

/// Minimal Result<T, E> for C++11.
///
/// Contract:
/// - Holds either a `T` or an `E`, never both, never neither.
/// - `ok()` returns whether it holds `T`.
/// - `value()` / `error()` must only be called after checking `ok()`.
/// - Move-only if `T` or `E` is move-only.
template <typename T, typename E>
class Expected {
 public:
    static_assert(!std::is_reference<T>::value, "T must not be a reference");
    static_assert(!std::is_reference<E>::value, "E must not be a reference");

    // Construct with a value.
    Expected(const T& v) : has_value_(true) { new (&storage_) T(v); }
    Expected(T&& v)      : has_value_(true) { new (&storage_) T(std::move(v)); }

    // Construct with an error (via Unexpected tag).
    Expected(const Unexpected<E>& u) : has_value_(false) { new (&err_storage_) E(u.value); }
    Expected(Unexpected<E>&& u)      : has_value_(false) { new (&err_storage_) E(std::move(u.value)); }

    Expected(const Expected& other) : has_value_(other.has_value_) {
        if (has_value_) new (&storage_) T(other.value_ref());
        else            new (&err_storage_) E(other.error_ref());
    }
    Expected(Expected&& other) noexcept : has_value_(other.has_value_) {
        if (has_value_) new (&storage_) T(std::move(other.value_ref()));
        else            new (&err_storage_) E(std::move(other.error_ref()));
    }
    ~Expected() { destroy(); }

    Expected& operator=(const Expected& other) {
        if (this == &other) return *this;
        destroy();
        has_value_ = other.has_value_;
        if (has_value_) new (&storage_) T(other.value_ref());
        else            new (&err_storage_) E(other.error_ref());
        return *this;
    }
    Expected& operator=(Expected&& other) noexcept {
        destroy();
        has_value_ = other.has_value_;
        if (has_value_) new (&storage_) T(std::move(other.value_ref()));
        else            new (&err_storage_) E(std::move(other.error_ref()));
        return *this;
    }

    bool ok() const { return has_value_; }
    /// Alias of `ok()`, matching `Optional<T>::has_value` and the naming used by
    /// `std::expected`/`std::optional`. Having both lets call sites stay uniform
    /// when a function returns `Expected` in one place and `Optional` in another.
    bool has_value() const { return has_value_; }
    explicit operator bool() const { return has_value_; }

    T& value()             { assert(has_value_); return value_ref(); }
    const T& value() const { assert(has_value_); return value_ref(); }
    E& error()             { assert(!has_value_); return error_ref(); }
    const E& error() const { assert(!has_value_); return error_ref(); }

    // Take by move (destroys the Expected's payload).
    T take_value() { assert(has_value_); T v = std::move(value_ref()); return v; }
    E take_error() { assert(!has_value_); E e = std::move(error_ref()); return e; }

 private:
    T&       value_ref()       { return *reinterpret_cast<T*>(&storage_); }
    const T& value_ref() const { return *reinterpret_cast<const T*>(&storage_); }
    E&       error_ref()       { return *reinterpret_cast<E*>(&err_storage_); }
    const E& error_ref() const { return *reinterpret_cast<const E*>(&err_storage_); }

    void destroy() {
        if (has_value_) value_ref().~T();
        else            error_ref().~E();
    }

    // We store T and E in separate raw buffers to keep the class trivially
    // constructible; C++11 does not have union-of-non-trivial in a clean way.
    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
    typename std::aligned_storage<sizeof(E), alignof(E)>::type err_storage_;
    bool has_value_;
};

}  // namespace core
}  // namespace agentenv

/// Rust `?` operator. Usage in a function returning Expected<X, E>:
///   auto y = AGENTENV_TRY(compute());
/// If `compute()` returns error, we short-circuit with `return make_unexpected(err)`.
#define AGENTENV_TRY(expr) \
    ({ auto _r = (expr); \
       if (!_r.ok()) return ::agentenv::core::make_unexpected(std::move(_r.error())); \
       std::move(_r.value()); })

#endif  // AGENTENV_CORE_EXPECTED_H_
