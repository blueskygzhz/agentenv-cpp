// SPDX-License-Identifier: MIT
// Rust: storage/util/src/always_send.rs — AlwaysSend<T> newtype.
//
// In Rust this force-marks a type as `Send` for cross-thread moves the compiler
// can't verify. C++ has no Send/Sync, so this is a thin passthrough wrapper kept
// only for 1:1 structural parity + documentation.
#ifndef AGENTENV_STORAGE_UTIL_ALWAYS_SEND_H_
#define AGENTENV_STORAGE_UTIL_ALWAYS_SEND_H_

#include <utility>

namespace agentenv {
namespace storage {
namespace util {

template <typename T>
class AlwaysSend {
 public:
    explicit AlwaysSend(T value) : value_(std::move(value)) {}
    T&       Get()       { return value_; }
    const T& Get() const { return value_; }
    T        Into()      { return std::move(value_); }
 private:
    T value_;
};

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_ALWAYS_SEND_H_
