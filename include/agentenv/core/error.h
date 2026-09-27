// SPDX-License-Identifier: MIT
// Rust: `anyhow::Error` — an erased error with source chain.
// C++11: minimal pimpl-free version.
#ifndef AGENTENV_CORE_ERROR_H_
#define AGENTENV_CORE_ERROR_H_

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace agentenv {
namespace core {

/// Erased error with a message chain. Move-only-ish: shared_ptr internals let
/// it be freely copied (like `anyhow::Error` cloning by Arc).
class AnyError {
 public:
    AnyError() = default;
    explicit AnyError(std::string what) : node_(std::make_shared<Node>(std::move(what))) {}

    /// Chain a message on top of the current error.
    AnyError context(std::string msg) const {
        AnyError copy;
        copy.node_ = std::make_shared<Node>(std::move(msg));
        copy.node_->source = node_;
        return copy;
    }

    const std::string& what() const {
        static const std::string kEmpty;
        return node_ ? node_->what : kEmpty;
    }

    /// Render as "top: mid: bottom" the way `anyhow::Error` does.
    std::string chain() const {
        std::ostringstream oss;
        const Node* cur = node_.get();
        bool first = true;
        while (cur) {
            if (!first) oss << ": ";
            oss << cur->what;
            first = false;
            cur = cur->source.get();
        }
        return oss.str();
    }

    bool ok() const { return !node_; }
    explicit operator bool() const { return static_cast<bool>(node_); }

 private:
    struct Node {
        std::string what;
        std::shared_ptr<Node> source;
        explicit Node(std::string w) : what(std::move(w)) {}
    };
    std::shared_ptr<Node> node_;
};

/// Convenience factory: `err("foo failed: rc=%d", rc)`-style helpers would go
/// here; we keep the skeleton dependency-free.
inline AnyError err(std::string s) { return AnyError(std::move(s)); }

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_ERROR_H_
