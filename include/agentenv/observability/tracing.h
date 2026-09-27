// SPDX-License-Identifier: MIT
// Rust: src/observability/ + crates/observability/ — tracing + metrics facade.
#ifndef AGENTENV_OBSERVABILITY_TRACING_H_
#define AGENTENV_OBSERVABILITY_TRACING_H_

#include <string>

namespace agentenv {
namespace observability {

/// Rust `init_tracing`: attach a subscriber that writes JSON logs to stderr.
void InitTracing(const std::string& service_name, int level);

/// Rust `Span` — a scoped span. RAII on stack.
class Span {
 public:
    Span(const std::string& name);
    ~Span();
    void SetAttribute(const std::string& key, const std::string& value);
 private:
    std::string name_;
};

}  // namespace observability
}  // namespace agentenv
#endif  // AGENTENV_OBSERVABILITY_TRACING_H_
