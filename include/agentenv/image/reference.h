// SPDX-License-Identifier: MIT
// Rust: src/image/reference.rs
//
// Turning what a user typed into the list of fully-qualified OCI references to
// try, in order.
//
// This file previously exposed a `ParseReference` returning a struct of
// registry/repository/tag/digest fields. That had no counterpart in
// `reference.rs` -- Rust never decomposes a reference, it only ever rewrites
// the string -- and the invented version diverged in two ways that matter:
//
//  * It kept both tag and digest for `repo:tag@sha256:...`. Rust drops the tag
//    (`split_name_suffix`), because the digest is authoritative; keeping both
//    produces a reference the registry may resolve to a *different* image than
//    the digest names.
//  * It never defaulted a missing tag to `:latest`, so a bare `ubuntu` stayed
//    tagless and resolved to whatever the registry picked.
#ifndef AGENTENV_IMAGE_REFERENCE_H_
#define AGENTENV_IMAGE_REFERENCE_H_

#include <string>
#include <vector>

#include "agentenv/core/optional.h"
#include "agentenv/image/error.h"

namespace agentenv {
namespace image {

/// Rust `image_ref_candidates` — the references to try for `input`, in order.
///
/// A fully-qualified input yields exactly one candidate. An unqualified one is
/// expanded against every entry of `search_registries`, preserving their
/// order, because that order *is* the operator's preference. Duplicates are
/// dropped without reordering: a repeated registry would otherwise cause the
/// same pull to be attempted twice.
///
/// `allowed_registries` is three-valued on purpose and must stay so:
///
///  * absent  — no restriction at all;
///  * present and non-empty — only those hosts survive;
///  * present and **empty** — a meaningful "deny everything".
///
/// Collapsing the last two (e.g. treating an empty vector as "unset") would
/// turn an explicit lockdown into an open gate, which is the one mistake here
/// with a security consequence.
core::Expected<std::vector<std::string>, ImageError> ImageRefCandidates(
    const std::string& input, const std::vector<std::string>& search_registries,
    const core::Optional<std::vector<std::string> >& allowed_registries);

/// Rust `registry_host_of` — the leading segment when it looks like a registry
/// host, otherwise nothing.
///
/// Absent is not an error: `library/ubuntu` has no host, and that is exactly
/// what tells the caller the reference needs expanding.
core::Optional<std::string> RegistryHostOf(const std::string& image_ref);

/// Rust `is_registry_host` — a segment is a host if it carries a dot, a port
/// colon, or is literally `localhost`.
///
/// Exposed because the same test decides both "does this input need a search
/// registry prepended" and "is this configured search prefix usable", and the
/// two must never drift apart.
bool IsRegistryHost(const std::string& segment);

/// Rust `split_name_suffix` — splits `image` into (name, suffix) where suffix
/// is `@digest`, `:tag`, or the defaulted `:latest`.
///
/// Exposed for testing: the tag-versus-digest precedence is the subtle part,
/// and it is not observable from `ImageRefCandidates` alone for inputs that
/// fail validation first.
void SplitNameSuffix(const std::string& image, std::string* name, std::string* suffix);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_REFERENCE_H_
