// SPDX-License-Identifier: MIT
// Rust: src/template/errors.rs
#include "agentenv/template/errors.h"

#include <sstream>

namespace agentenv {
namespace tpl {
namespace {

/// Rust `str::trim` — both ends, ASCII whitespace.
std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

}  // namespace

TemplateBuildFailure TemplateBuildFailure::New(const std::string& message) {
    TemplateBuildFailure failure;
    failure.reason = snapshot::TemplateBuildErrorReason::New(message);
    return failure;
}

TemplateBuildFailure TemplateBuildFailure::WithStep(const std::string& message,
                                                    const std::string& step) {
    TemplateBuildFailure failure;
    failure.reason = snapshot::TemplateBuildErrorReason::WithStep(message, step);
    return failure;
}

std::string CommandOutputSuffix(const std::string& stdout_text, const std::string& stderr_text) {
    // stderr wins: a failing command almost always explains itself there.
    const std::string err = Trim(stderr_text);
    if (!err.empty()) return "; stderr: " + err;

    const std::string out = Trim(stdout_text);
    if (!out.empty()) return "; stdout: " + out;

    return std::string();
}

TemplateBuildError TemplateBuildError::InvalidInput(const std::string& reason) {
    TemplateBuildError error;
    error.kind = TemplateBuildErrorKind::InvalidInput;
    error.invalid_input_reason = reason;
    return error;
}

TemplateBuildError TemplateBuildError::System(const std::string& message) {
    TemplateBuildError error;
    error.kind = TemplateBuildErrorKind::System;
    error.system_reason = snapshot::TemplateBuildErrorReason::New(message);
    return error;
}

TemplateBuildError TemplateBuildError::WithSource(const std::string& message,
                                                  const std::string& source) {
    TemplateBuildError error = System(message);
    error.source = source;
    return error;
}

TemplateBuildError TemplateBuildError::WithReasonSource(
    const snapshot::TemplateBuildErrorReason& reason, const std::string& source) {
    TemplateBuildError error;
    error.kind = TemplateBuildErrorKind::System;
    error.system_reason = reason;
    error.source = source;
    return error;
}

TemplateBuildError TemplateBuildError::FromFailure(const TemplateBuildFailure& failure) {
    TemplateBuildError error;
    error.kind = TemplateBuildErrorKind::System;
    error.system_reason = failure.reason;
    return error;
}

std::string TemplateBuildError::ToString() const {
    std::ostringstream oss;
    switch (kind) {
        case TemplateBuildErrorKind::InvalidInput:
            oss << "invalid template build input: " << invalid_input_reason;
            return oss.str();
        case TemplateBuildErrorKind::System:
            oss << "template build failed: " << system_reason.ToString();
            return oss.str();
    }
    return "template build failed";
}

bool TemplateBuildError::operator==(const TemplateBuildError& o) const {
    if (kind != o.kind) return false;
    if (kind == TemplateBuildErrorKind::InvalidInput) {
        return invalid_input_reason == o.invalid_input_reason;
    }
    if (system_reason != o.system_reason) return false;
    if (source.has_value() != o.source.has_value()) return false;
    return !source.has_value() || *source == *o.source;
}

std::ostream& operator<<(std::ostream& os, TemplateBuildErrorKind kind) {
    switch (kind) {
        case TemplateBuildErrorKind::InvalidInput:
            return os << "InvalidInput";
        case TemplateBuildErrorKind::System:
            return os << "System";
    }
    return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, const TemplateBuildError& error) {
    os << error.ToString();
    if (error.step().has_value()) os << " (step: " << *error.step() << ")";
    if (error.source.has_value()) os << " <- " << *error.source;
    return os;
}

TemplatePipelineError TemplatePipelineError::FromBuild(const TemplateBuildError& error) {
    TemplatePipelineError out;
    out.kind = TemplatePipelineErrorKind::Build;
    out.build = error;
    return out;
}

TemplatePipelineError TemplatePipelineError::FromRepository(
    const snapshot::repository::RepositoryError& error) {
    TemplatePipelineError out;
    out.kind = TemplatePipelineErrorKind::Repository;
    out.repository = error;
    return out;
}

std::string TemplatePipelineError::ToString() const {
    // `#[error(transparent)]`: no prefix of our own.
    switch (kind) {
        case TemplatePipelineErrorKind::Build:
            return build.ToString();
        case TemplatePipelineErrorKind::Repository:
            return repository.ToString();
    }
    return "template pipeline failed";
}

std::ostream& operator<<(std::ostream& os, const TemplatePipelineError& error) {
    return os << error.ToString();
}

}  // namespace tpl
}  // namespace agentenv
