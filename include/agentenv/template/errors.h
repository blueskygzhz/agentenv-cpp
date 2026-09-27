// SPDX-License-Identifier: MIT
// Rust: src/template/errors.rs
#ifndef AGENTENV_TEMPLATE_ERRORS_H_
#define AGENTENV_TEMPLATE_ERRORS_H_

#include <ostream>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace tpl {

/// Rust struct `TemplateBuildFailure` — a build failure carrying the reason
/// that gets persisted on the template record. `#[error("{reason}")]`.
struct TemplateBuildFailure {
    snapshot::TemplateBuildErrorReason reason;

    /// Rust `TemplateBuildFailure::new`.
    static TemplateBuildFailure New(const std::string& message);
    /// Rust `TemplateBuildFailure::with_step`.
    static TemplateBuildFailure WithStep(const std::string& message, const std::string& step);

    std::string ToString() const { return reason.ToString(); }
};

/// Rust `command_output_suffix` — appends whichever stream carries a clue,
/// preferring stderr. Returns an empty string when both are blank, so callers
/// can concatenate unconditionally.
std::string CommandOutputSuffix(const std::string& stdout_text, const std::string& stderr_text);

/// Rust enum `TemplateBuildError`.
enum class TemplateBuildErrorKind {
    /// Rust `InvalidInput { reason }` — the caller's request is unusable.
    InvalidInput,
    /// Rust `System { reason, source }` — the build machinery failed.
    System,
};

/// Rust enum `TemplateBuildError` with its `thiserror` messages.
struct TemplateBuildError {
    TemplateBuildErrorKind kind = TemplateBuildErrorKind::System;
    /// `InvalidInput` carries a plain string; `System` carries the structured
    /// reason (which additionally remembers the failing step).
    std::string invalid_input_reason;
    snapshot::TemplateBuildErrorReason system_reason;
    /// Rust `#[source] Option<AnyhowError>`, flattened to its rendered chain
    /// because that is all the C++ call sites do with it.
    core::Optional<std::string> source;

    /// Rust `TemplateBuildError::invalid_input`.
    static TemplateBuildError InvalidInput(const std::string& reason);
    /// Rust `TemplateBuildError::system`.
    static TemplateBuildError System(const std::string& message);
    /// Rust `TemplateBuildError::with_source`.
    static TemplateBuildError WithSource(const std::string& message, const std::string& source);
    /// Rust `TemplateBuildError::with_reason_source`.
    static TemplateBuildError WithReasonSource(const snapshot::TemplateBuildErrorReason& reason,
                                               const std::string& source);
    /// Promotes a `TemplateBuildFailure` into the `System` variant, which is
    /// how the builder surfaces step failures.
    static TemplateBuildError FromFailure(const TemplateBuildFailure& failure);

    /// Rust `thiserror` `Display`.
    std::string ToString() const;

    /// The failing step, when the failure was attributed to one.
    const core::Optional<std::string>& step() const { return system_reason.step; }

    bool operator==(const TemplateBuildError& o) const;
    bool operator!=(const TemplateBuildError& o) const { return !(*this == o); }
};

std::ostream& operator<<(std::ostream& os, TemplateBuildErrorKind kind);
std::ostream& operator<<(std::ostream& os, const TemplateBuildError& error);

/// Rust `type TemplateBuildResult<T>`.
template <typename T>
using TemplateBuildResult = core::Expected<T, TemplateBuildError>;

/// Rust enum `TemplatePipelineError` — both variants are `#[error(transparent)]`,
/// so the rendered message is the inner error's, with no added prefix.
enum class TemplatePipelineErrorKind {
    Build,
    Repository,
};

struct TemplatePipelineError {
    TemplatePipelineErrorKind kind = TemplatePipelineErrorKind::Build;
    TemplateBuildError build;
    snapshot::repository::RepositoryError repository;

    /// Rust `#[from] TemplateBuildError`.
    static TemplatePipelineError FromBuild(const TemplateBuildError& error);
    /// Rust `#[from] RepositoryError`.
    static TemplatePipelineError FromRepository(const snapshot::repository::RepositoryError& error);

    std::string ToString() const;
};

std::ostream& operator<<(std::ostream& os, const TemplatePipelineError& error);

/// Rust `type TemplatePipelineResult<T>`.
template <typename T>
using TemplatePipelineResult = core::Expected<T, TemplatePipelineError>;

}  // namespace tpl
}  // namespace agentenv
#endif  // AGENTENV_TEMPLATE_ERRORS_H_
