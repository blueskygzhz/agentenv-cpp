// SPDX-License-Identifier: MIT
// Rust: src/image/reference.rs — `mod tests`.
//
// The previous version of this file tested a `ParseReference` that decomposed a
// reference into registry/repository/tag/digest fields. No such function exists
// in `reference.rs`, so those six cases pinned behaviour Rust never had while
// leaving all thirteen of the real cases unchecked -- including the two that
// encode a security decision (the three-valued allowlist) and the one that
// encodes a correctness decision (digest wins over tag).
//
// Each test below corresponds to one `#[test]` in `reference.rs` and carries
// the same name, followed by a few that cover the helpers this header exposes
// and that Rust reaches only through private-module access.
#include "microtest.h"

#include <string>
#include <vector>

#include "agentenv/core/optional.h"
#include "agentenv/image/reference.h"

using namespace agentenv;

namespace {

typedef std::vector<std::string> Strings;
typedef core::Optional<Strings> MaybeStrings;
typedef core::Expected<Strings, image::ImageError> CandidateResult;

/// Rust test helper `candidates` — resolve with no allowlist, i.e. `None`.
///
/// Absence is spelled as a default-constructed `Optional`, never as an empty
/// vector: the two mean opposite things ("no restriction" versus "deny all"),
/// and the last four tests exist to keep that distinction honest.
CandidateResult Candidates(const std::string& input, const Strings& search) {
    return image::ImageRefCandidates(input, search, MaybeStrings());
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// Compared element-wise rather than with `==` so a failure reports which
/// candidate differs; order is part of the contract, so this must not sort.
bool Equals(const CandidateResult& got, const Strings& want) {
    if (!got.ok()) return false;
    if (got.value().size() != want.size()) return false;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (got.value()[i] != want[i]) return false;
    }
    return true;
}

}  // namespace

MT_TEST(image_ref_candidates_searches_unqualified_shortnames_in_order) {
    // `docker.io` gains `library/`, `ghcr.io` does not: the implicit official
    // namespace is a Docker Hub convention, not a registry-wide one.
    const Strings got_search{"docker.io", "ghcr.io"};
    MT_EXPECT_TRUE(Equals(Candidates("ubuntu:24.04", got_search),
                          Strings{"docker.io/library/ubuntu:24.04", "ghcr.io/ubuntu:24.04"}));
}

MT_TEST(image_ref_candidates_preserves_namespace_for_unqualified_refs) {
    // A name that already has a namespace must not get `library/` spliced in
    // front of it, which would address a repository that does not exist.
    const Strings search{"docker.io", "ghcr.io"};
    MT_EXPECT_TRUE(Equals(Candidates("e2b/code-interpreter:latest", search),
                          Strings{"docker.io/e2b/code-interpreter:latest",
                                  "ghcr.io/e2b/code-interpreter:latest"}));
}

MT_TEST(image_ref_candidates_skips_search_for_fully_qualified_refs) {
    // The `:5000` is a port, so the leading segment is a host and the search
    // list is ignored entirely -- expanding it would rewrite an explicit ref.
    const Strings search{"docker.io", "ghcr.io"};
    MT_EXPECT_TRUE(Equals(Candidates("registry.internal:5000/team/app@sha256:abc", search),
                          Strings{"registry.internal:5000/team/app@sha256:abc"}));
}

MT_TEST(image_ref_candidates_strips_tag_when_digest_is_present) {
    // The digest is authoritative. Keeping `:v1` as well would leave the
    // registry free to resolve the tag to an image other than the pinned one.
    const Strings search{"docker.io", "ghcr.io"};
    MT_EXPECT_TRUE(Equals(Candidates("registry.internal:5000/team/app:v1@sha256:abc", search),
                          Strings{"registry.internal:5000/team/app@sha256:abc"}));
    MT_EXPECT_TRUE(Equals(Candidates("ubuntu:24.04@sha256:abc", Strings{"docker.io"}),
                          Strings{"docker.io/library/ubuntu@sha256:abc"}));
}

MT_TEST(image_ref_candidates_supports_registry_namespace_prefixes) {
    // A search entry may carry a path, and `latest` is supplied because a
    // tagless reference is resolved by the registry to whatever it likes.
    MT_EXPECT_TRUE(Equals(Candidates("ubuntu", Strings{"registry.example.com/base"}),
                          Strings{"registry.example.com/base/ubuntu:latest"}));
}

MT_TEST(image_ref_candidates_deduplicates_candidates_without_changing_order) {
    // A repeated registry would otherwise cause the same pull twice; the
    // surviving order is still the configured one.
    const Strings search{"docker.io", "docker.io", "ghcr.io"};
    MT_EXPECT_TRUE(Equals(Candidates("ubuntu", search),
                          Strings{"docker.io/library/ubuntu:latest", "ghcr.io/ubuntu:latest"}));
}

MT_TEST(image_ref_candidates_rejects_search_prefix_without_registry_host) {
    // `myregistry` has no dot, colon, and is not `localhost`, so prepending it
    // would produce `myregistry/ubuntu` -- a Docker Hub repository owned by
    // whoever registered that account.
    const CandidateResult err = Candidates("ubuntu", Strings{"myregistry"});
    MT_EXPECT_TRUE(!err.ok());
    MT_EXPECT_TRUE(Contains(err.error().ToString(), "registry host"));
    // Config, not infrastructure: must not be reported as a server error.
    MT_EXPECT_TRUE(err.error().IsUserError());
}

MT_TEST(image_ref_candidates_allows_fully_qualified_ref_in_allowlist) {
    const Strings allowed{"registry.example.com", "ghcr.io"};
    MT_EXPECT_TRUE(Equals(image::ImageRefCandidates("registry.example.com/team/app:tag", Strings(),
                                                    MaybeStrings(allowed)),
                          Strings{"registry.example.com/team/app:tag"}));
}

MT_TEST(image_ref_candidates_rejects_fully_qualified_ref_not_in_allowlist) {
    const Strings allowed{"registry.example.com"};
    const CandidateResult err =
        image::ImageRefCandidates("ghcr.io/team/app:tag", Strings(), MaybeStrings(allowed));
    MT_EXPECT_TRUE(!err.ok());
    MT_EXPECT_TRUE(Contains(err.error().ToString(), "not from an allowed registry"));
    // The message names the permitted hosts, so the caller can fix the input
    // without reading the server's configuration.
    MT_EXPECT_TRUE(Contains(err.error().ToString(), "registry.example.com"));
}

MT_TEST(image_ref_candidates_filters_shortname_expansions_to_allowlist) {
    // Filtering drops the disallowed expansion instead of failing the whole
    // resolution: one surviving candidate is still resolvable.
    const Strings allowed{"ghcr.io"};
    MT_EXPECT_TRUE(Equals(image::ImageRefCandidates("ubuntu", Strings{"docker.io", "ghcr.io"},
                                                    MaybeStrings(allowed)),
                          Strings{"ghcr.io/ubuntu:latest"}));
}

MT_TEST(image_ref_candidates_rejects_shortname_when_no_candidate_is_allowed) {
    const Strings allowed{"registry.example.com"};
    const CandidateResult err = image::ImageRefCandidates(
        "ubuntu", Strings{"docker.io", "ghcr.io"}, MaybeStrings(allowed));
    MT_EXPECT_TRUE(!err.ok());
    MT_EXPECT_TRUE(Contains(err.error().ToString(), "not from an allowed registry"));
}

MT_TEST(image_ref_candidates_empty_allowlist_rejects_every_registry) {
    // An explicit empty list is a meaningful "deny all", distinct from absent.
    // Treating it as unset -- the obvious simplification -- would silently turn
    // a lockdown into an open gate, so this case is pinned deliberately.
    const CandidateResult err =
        image::ImageRefCandidates("ghcr.io/team/app:tag", Strings(), MaybeStrings(Strings()));
    MT_EXPECT_TRUE(!err.ok());
    MT_EXPECT_TRUE(Contains(err.error().ToString(), "not from an allowed registry"));
}

MT_TEST(image_ref_candidates_unset_allowlist_imposes_no_restriction) {
    MT_EXPECT_TRUE(Equals(image::ImageRefCandidates("ghcr.io/team/app:tag", Strings(),
                                                    MaybeStrings()),
                          Strings{"ghcr.io/team/app:tag"}));
}

// --- Helpers exposed by the C++ header -------------------------------------
//
// Rust tests these only through `image_ref_candidates`, which it can do because
// the module is private to the crate. Here they are part of the public header,
// so they are pinned directly: `SplitNameSuffix` in particular is the one place
// where tag-versus-digest precedence is decided.

MT_TEST(split_name_suffix_defaults_missing_tag_to_latest) {
    std::string name;
    std::string suffix;
    image::SplitNameSuffix("ubuntu", &name, &suffix);
    MT_EXPECT_TRUE(name == "ubuntu");
    MT_EXPECT_TRUE(suffix == ":latest");
}

MT_TEST(split_name_suffix_ignores_registry_port_colon) {
    // The colon in `:5000` precedes the last slash, so it is a port. Splitting
    // there would cut the host away from the repository.
    std::string name;
    std::string suffix;
    image::SplitNameSuffix("registry.internal:5000/team/app", &name, &suffix);
    MT_EXPECT_TRUE(name == "registry.internal:5000/team/app");
    MT_EXPECT_TRUE(suffix == ":latest");
}

MT_TEST(split_name_suffix_prefers_digest_over_tag) {
    std::string name;
    std::string suffix;
    image::SplitNameSuffix("team/app:v1@sha256:abc", &name, &suffix);
    MT_EXPECT_TRUE(name == "team/app");
    MT_EXPECT_TRUE(suffix == "@sha256:abc");
}

MT_TEST(is_registry_host_accepts_dot_port_and_localhost) {
    MT_EXPECT_TRUE(image::IsRegistryHost("ghcr.io"));
    MT_EXPECT_TRUE(image::IsRegistryHost("registry:5000"));
    MT_EXPECT_TRUE(image::IsRegistryHost("localhost"));
    // Not a host, so a bare segment stays a repository name and gets expanded
    // against the search list.
    MT_EXPECT_TRUE(!image::IsRegistryHost("myregistry"));
    MT_EXPECT_TRUE(!image::IsRegistryHost("library"));
}

MT_TEST(registry_host_of_returns_absent_without_host) {
    MT_EXPECT_TRUE(*image::RegistryHostOf("ghcr.io/team/app:tag") == "ghcr.io");
    // Absent rather than an error: "has no host" is what tells the caller the
    // reference still needs expanding.
    MT_EXPECT_TRUE(!image::RegistryHostOf("library/ubuntu").has_value());
    MT_EXPECT_TRUE(!image::RegistryHostOf("ubuntu").has_value());
}

MT_TEST(image_ref_candidates_rejects_empty_and_malformed_refs) {
    MT_EXPECT_TRUE(!Candidates("", Strings{"docker.io"}).ok());
    // Whitespace-only trims to empty, so it must fail the same way rather than
    // becoming the candidate `docker.io/library/   :latest`.
    MT_EXPECT_TRUE(!Candidates("   ", Strings{"docker.io"}).ok());
    MT_EXPECT_TRUE(!Candidates("ubuntu:", Strings{"docker.io"}).ok());
    MT_EXPECT_TRUE(!Candidates("ubuntu@", Strings{"docker.io"}).ok());
    // Unqualified with nothing to expand against: refused rather than falling
    // back to Docker Hub, since clearing the list is how implicit registry
    // access is switched off.
    const CandidateResult err = Candidates("ubuntu", Strings());
    MT_EXPECT_TRUE(!err.ok());
    MT_EXPECT_TRUE(Contains(err.error().ToString(), "search registry list is empty"));
}

MT_MAIN
