// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/common/acr/source_image.rs
#include "agentenv/snapshot/source_registry.h"

#include <vector>

#include "agentenv/core/url.h"

namespace agentenv {
namespace snapshot {
namespace {

using repository::RepositoryError;

/// Rust `url.path_segments()`: splits on `/`, dropping the leading empty
/// segment but keeping interior empties so a malformed path is still caught.
std::vector<std::string> PathSegments(const std::string& path) {
    std::vector<std::string> segments;
    std::size_t i = 0;
    if (i < path.size() && path[i] == '/') ++i;
    std::string current;
    for (; i < path.size(); ++i) {
        if (path[i] == '/') {
            segments.push_back(current);
            current.clear();
            continue;
        }
        current += path[i];
    }
    segments.push_back(current);
    return segments;
}

std::string TrimTrailingSlashes(const std::string& value) {
    std::size_t end = value.size();
    while (end > 0 && value[end - 1] == '/') --end;
    return value.substr(0, end);
}

bool IsLoopbackHost(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
}

}  // namespace

repository::RepositoryResult<SourceRegistryRepository> SourceRegistryRepository::Parse(
    const std::string& repo_blob_url) {
    const core::Expected<core::Url, std::string> url = core::Url::Parse(repo_blob_url);
    if (!url.ok()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "invalid ACR repoBlobUrl '" + repo_blob_url + "': " + url.error()));
    }

    // Plain HTTP would send the bearer token in the clear; loopback is
    // allowed only so tests can run against a local registry.
    const bool loopback_http =
        url.value().scheme == "http" && IsLoopbackHost(url.value().host);
    if (url.value().scheme != "https" && !loopback_http) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR repoBlobUrl must use https: " + repo_blob_url));
    }
    if (url.value().host.empty()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR repoBlobUrl is missing registry host: " + repo_blob_url));
    }

    // Strip a default `:443` so two spellings of one registry normalise
    // together; every other explicit port is significant.
    std::string registry = url.value().host;
    if (url.value().port.has_value()) {
        const bool default_https = *url.value().port == 443 && url.value().scheme == "https";
        if (!default_https) {
            char port_text[16];
            std::snprintf(port_text, sizeof(port_text), ":%u",
                          static_cast<unsigned>(*url.value().port));
            registry += port_text;
        }
    }

    const std::vector<std::string> segments = PathSegments(url.value().path);
    if (segments.size() < 3 || segments[0] != "v2" ||
        segments[segments.size() - 1] != "blobs") {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR repoBlobUrl must have shape https://<registry>/v2/<repo>/blobs: " +
            repo_blob_url));
    }

    // Everything between `v2` and `blobs`, so a nested repository path such
    // as `ns/team/app` survives.
    std::string repository_path;
    for (std::size_t i = 1; i + 1 < segments.size(); ++i) {
        if (!repository_path.empty()) repository_path += "/";
        repository_path += segments[i];
    }
    if (repository_path.empty()) {
        return core::make_unexpected(RepositoryError::Unsupported(
            "ACR repoBlobUrl repository is empty: " + repo_blob_url));
    }

    SourceRegistryRepository source;
    source.registry = registry;
    source.repository = repository_path;
    // Rebuilt from the normalised registry, so the stored URL and the parsed
    // registry can never disagree.
    source.repo_blob_url =
        url.value().scheme + "://" + registry + TrimTrailingSlashes(url.value().path);
    return source;
}

std::string SourceRegistryRepository::ImageRef(const std::string& tag) const {
    return registry + "/" + repository + ":" + tag;
}

namespace {

/// Rust `registry_api_url` — reuses the blob URL's scheme so a loopback HTTP
/// source does not silently switch to HTTPS for the API calls.
std::string RegistryApiUrl(const SourceRegistryRepository& source) {
    const std::size_t separator = source.repo_blob_url.find("://");
    const std::string scheme = separator == std::string::npos
                                   ? std::string("https")
                                   : source.repo_blob_url.substr(0, separator);
    return scheme + "://" + source.registry;
}

}  // namespace

std::string SourceRegistryRepository::UploadUrl() const {
    return RegistryApiUrl(*this) + "/v2/" + repository + "/blobs/uploads/";
}

std::string SourceRegistryRepository::ManifestUrl(const std::string& tag) const {
    return RegistryApiUrl(*this) + "/v2/" + repository + "/manifests/" + tag;
}

}  // namespace snapshot
}  // namespace agentenv
