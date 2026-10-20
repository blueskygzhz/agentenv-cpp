// SPDX-License-Identifier: MIT
// Rust: src/image/reference.rs
#include "agentenv/image/reference.h"

#include <set>
#include <sstream>

namespace agentenv {
namespace image {

namespace {

/// Rust `invalid_image_ref`.
template <typename T>
core::Expected<T, ImageError> InvalidRef(const std::string& message) {
    return core::make_unexpected(InvalidReference(message));
}

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
                           text[begin] == '\r')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\n' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::vector<std::string> SplitOn(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = text.find(separator, start);
        if (at == std::string::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

/// Rust `has_registry_host`.
bool HasRegistryHost(const std::string& input) {
    const std::string image = Trim(input);
    const std::size_t slash = image.find('/');
    if (slash == std::string::npos) return false;
    return IsRegistryHost(image.substr(0, slash));
}

/// Rust `validate_registry_search_prefix`.
///
/// All four rejections are kept. Each one exists because the bad value is
/// otherwise concatenated into a reference that fails much later, with an
/// error naming the registry rather than the config entry that produced it.
core::Expected<std::string, ImageError> ValidateRegistrySearchPrefix(const std::string& input) {
    if (input.empty()) {
        return InvalidRef<std::string>("image resolver search registry entry must be non-empty");
    }
    if (input.find("://") != std::string::npos) {
        return InvalidRef<std::string>(
            "image resolver search registry entry must not include a URL scheme: " + input);
    }
    if (input.find('@') != std::string::npos) {
        return InvalidRef<std::string>(
            "image resolver search registry entry must not include a digest: " + input);
    }
    const std::vector<std::string> segments = SplitOn(input, '/');
    if (!IsRegistryHost(segments[0])) {
        return InvalidRef<std::string>(
            "image resolver search registry entry must start with a registry host: " + input);
    }
    // Only the segments *after* the host: a colon in the host is its port,
    // while a colon further along is a tag, and a tag here would be spliced
    // into the middle of the repository path.
    for (std::size_t i = 1; i < segments.size(); ++i) {
        if (segments[i].find(':') != std::string::npos) {
            return InvalidRef<std::string>(
                "image resolver search registry entry must not include an image tag: " + input);
        }
    }
    return input;
}

/// Rust `validate_image_ref_parts`.
core::Expected<core::Unit, ImageError> ValidateImageRefParts(const std::string& image,
                                                             const std::string& name,
                                                             const std::string& suffix) {
    if (name.empty()) {
        return InvalidRef<core::Unit>("image reference has an empty repository name: " + image);
    }
    // A `@` left in the *name* means the digest was not at the leaf, so the
    // split above did not understand the input.
    if (name.find('@') != std::string::npos) {
        return InvalidRef<core::Unit>("image reference has invalid digest syntax: " + image);
    }
    if (!suffix.empty() && suffix[0] == ':') {
        const std::string tag = suffix.substr(1);
        if (tag.empty()) {
            return InvalidRef<core::Unit>("image reference has an empty tag: " + image);
        }
        if (tag.find('@') != std::string::npos) {
            return InvalidRef<core::Unit>("image reference has invalid tag syntax: " + image);
        }
    } else if (!suffix.empty() && suffix[0] == '@') {
        const std::string digest = suffix.substr(1);
        if (digest.empty()) {
            return InvalidRef<core::Unit>("image reference has an empty digest: " + image);
        }
        if (digest.find('@') != std::string::npos) {
            return InvalidRef<core::Unit>("image reference has invalid digest syntax: " + image);
        }
    }
    return core::Unit();
}

std::string JoinWith(const std::vector<std::string>& values, const std::string& separator) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += separator;
        out += values[i];
    }
    return out;
}

/// Rust `enforce_allowed_registries`.
core::Expected<std::vector<std::string>, ImageError> EnforceAllowedRegistries(
    const std::string& image, const std::vector<std::string>& candidates,
    const core::Optional<std::vector<std::string> >& allowed_registries) {
    // Absent imposes no restriction. Checked before the emptiness test so an
    // explicitly empty list still reaches the filter below and denies
    // everything.
    if (!allowed_registries.has_value()) return candidates;

    std::vector<std::string> permitted;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const core::Optional<std::string> host = RegistryHostOf(candidates[i]);
        if (!host.has_value()) continue;
        for (std::size_t j = 0; j < allowed_registries->size(); ++j) {
            if ((*allowed_registries)[j] == *host) {
                permitted.push_back(candidates[i]);
                break;
            }
        }
    }

    if (permitted.empty()) {
        std::ostringstream o;
        o << "image reference '" << image
          << "' is not from an allowed registry; allowed registries: ["
          << JoinWith(*allowed_registries, ", ") << "]";
        return InvalidRef<std::vector<std::string> >(o.str());
    }
    return permitted;
}

}  // namespace

bool IsRegistryHost(const std::string& segment) {
    return segment.find('.') != std::string::npos ||
           segment.find(':') != std::string::npos || segment == "localhost";
}

core::Optional<std::string> RegistryHostOf(const std::string& image_ref) {
    const std::size_t slash = image_ref.find('/');
    if (slash == std::string::npos) return core::Optional<std::string>();
    const std::string host = image_ref.substr(0, slash);
    if (!IsRegistryHost(host)) return core::Optional<std::string>();
    return core::Optional<std::string>(host);
}

void SplitNameSuffix(const std::string& image, std::string* name, std::string* suffix) {
    const std::size_t last_slash_pos = image.rfind('/');
    const std::size_t last_slash = last_slash_pos == std::string::npos ? 0 : last_slash_pos + 1;
    const std::string leaf = image.substr(last_slash);

    const std::size_t at_idx = leaf.find('@');
    if (at_idx != std::string::npos) {
        // OCI refs may carry both tag and digest (`repo:tag@sha256:...`). The
        // digest is authoritative, so the tag is dropped and only
        // `repo@digest` survives -- keeping both lets the registry resolve the
        // tag to something other than the digest the caller pinned.
        const std::size_t digest_split = last_slash + at_idx;
        const std::size_t tag_idx = leaf.substr(0, at_idx).find(':');
        const std::size_t name_split =
            tag_idx == std::string::npos ? digest_split : last_slash + tag_idx;
        *name = image.substr(0, name_split);
        *suffix = image.substr(digest_split);
        return;
    }
    // Searched in the *leaf* only: a colon before the last slash is a registry
    // port, and treating it as a tag would split the host off the repository.
    const std::size_t colon = leaf.find(':');
    if (colon != std::string::npos) {
        const std::size_t split = last_slash + colon;
        *name = image.substr(0, split);
        *suffix = image.substr(split);
        return;
    }
    // Defaulted, not left empty: a tagless reference is resolved by the
    // registry to whatever it likes, so two pulls of the same string can get
    // different images.
    *name = image;
    *suffix = ":latest";
}

core::Expected<std::vector<std::string>, ImageError> ImageRefCandidates(
    const std::string& input, const std::vector<std::string>& search_registries,
    const core::Optional<std::vector<std::string> >& allowed_registries) {
    const std::string image = Trim(input);
    if (image.empty()) {
        return InvalidRef<std::vector<std::string> >("image reference must be non-empty");
    }

    std::string name;
    std::string suffix;
    SplitNameSuffix(image, &name, &suffix);
    const core::Expected<core::Unit, ImageError> valid =
        ValidateImageRefParts(image, name, suffix);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    std::vector<std::string> candidates;
    if (HasRegistryHost(name)) {
        candidates.push_back(name + suffix);
    } else {
        // Refused rather than silently falling back to Docker Hub: an operator
        // who cleared the search list did so to stop implicit registry access.
        if (search_registries.empty()) {
            return InvalidRef<std::vector<std::string> >(
                "cannot resolve unqualified image ref '" + image +
                "': search registry list is empty");
        }
        std::set<std::string> seen;
        for (std::size_t i = 0; i < search_registries.size(); ++i) {
            const core::Expected<std::string, ImageError> registry =
                ValidateRegistrySearchPrefix(search_registries[i]);
            if (!registry.ok()) return core::make_unexpected(registry.error());

            // Docker Hub's official images live under `library/`, and only for
            // a name with no namespace of its own: `docker.io/e2b/app` must not
            // become `docker.io/library/e2b/app`.
            std::string repository;
            if (registry.value() == "docker.io" && name.find('/') == std::string::npos) {
                repository = "docker.io/library/" + name;
            } else {
                repository = registry.value() + "/" + name;
            }
            const std::string candidate = repository + suffix;
            // Deduplicated through an auxiliary set so the vector keeps the
            // configured order; sorting it would discard the operator's
            // preference.
            if (seen.insert(candidate).second) candidates.push_back(candidate);
        }
    }

    return EnforceAllowedRegistries(image, candidates, allowed_registries);
}

}  // namespace image
}  // namespace agentenv
