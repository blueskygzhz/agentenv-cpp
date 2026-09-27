// SPDX-License-Identifier: MIT
// Rust: src/observability/ + crates/observability/
#include "agentenv/observability/tracing.h"

#include <cstdio>

namespace agentenv {
namespace observability {

void InitTracing(const std::string& service_name, int level) {
    std::fprintf(stderr,
        "{\"event\":\"tracing_init\",\"service\":\"%s\",\"level\":%d}\n",
        service_name.c_str(), level);
}

Span::Span(const std::string& name) : name_(name) {}
Span::~Span() {}
void Span::SetAttribute(const std::string&, const std::string&) {}

}}
