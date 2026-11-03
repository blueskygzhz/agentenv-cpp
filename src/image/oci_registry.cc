// SPDX-License-Identifier: MIT
// Rust: src/image/oci_image.rs — `RegistryAuth`, `RegistryConfig`,
// `ecr_credentials`, the regctl helpers and the manifest fetch.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "agentenv/core/base64.h"
#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/image/oci_image.h"
#include "agentenv/storage/overlaybd/http_transport.h"
#include "agentenv/storage/overlaybd/s3_signer.h"

namespace agentenv {
namespace image {

const char* const kRegctlGomaxprocs = "4";

namespace {

namespace overlaybd = storage::overlaybd;

const char* const kOciIndexMediaTypes[] = {
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
};

std::string TrimAscii(const std::string& text) {
    const char* const kSpace = " \t\n\r\v\f";
    const std::size_t begin = text.find_first_not_of(kSpace);
    if (begin == std::string::npos) return std::string();
    return text.substr(begin, text.find_last_not_of(kSpace) - begin + 1);
}

std::string JoinArgs(const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) out += ' ';
        out += args[i];
    }
    return out;
}

bool IsUtf8(const std::string& bytes) { return core::FromUtf8Lossy(bytes) == bytes; }

std::string TempDirectory() {
    const char* tmp = ::getenv("TMPDIR");
    return (tmp != 0 && tmp[0] != '\0') ? std::string(tmp) : std::string("/tmp");
}

}  // namespace

// ---- RegistryConfig ----

struct RegistryConfig::Holder {
    std::string path;
    ~Holder() {
        if (!path.empty()) ::unlink(path.c_str());
    }
};

core::Expected<RegistryConfig, std::string> RegistryConfig::Create(const std::string& host,
                                                                   const std::string& username,
                                                                   const std::string& password) {
    core::Json entry;
    entry["user"] = core::Json(username);
    entry["pass"] = core::Json(password);
    core::Json hosts;
    hosts[host] = entry;
    core::Json document;
    document["version"] = core::Json(static_cast<int64_t>(1));
    document["incDockerCred"] = core::Json(false);
    document["hosts"] = hosts;
    const std::string text = document.ToString();

    // mkstemp creates the file 0600: the credentials are never world-readable,
    // not even between the create and a later chmod.
    std::string tmpl = TempDirectory() + "/aenv-registry-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const int fd = ::mkostemp(buf.data(), O_CLOEXEC);
    if (fd < 0) {
        return core::make_unexpected(std::string("create registry config: ") +
                                     std::strerror(errno));
    }

    RegistryConfig config;
    config.holder_ = std::make_shared<Holder>();
    config.holder_->path = buf.data();

    const char* cursor = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, cursor, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string why = std::strerror(errno);
            ::close(fd);
            return core::make_unexpected("write registry config: " + why);
        }
        cursor += n;
        left -= static_cast<std::size_t>(n);
    }
    ::close(fd);
    return config;
}

const std::string& RegistryConfig::Path() const { return holder_->path; }

// ---- RegistryAuth ----

RegistryAuth RegistryAuth::General(const std::string& username, const std::string& password) {
    RegistryAuth auth;
    auth.kind = kGeneral;
    auth.username = username;
    auth.password = password;
    return auth;
}

RegistryAuth RegistryAuth::Gcp(const std::string& service_account_json) {
    RegistryAuth auth;
    auth.kind = kGcp;
    auth.service_account_json = service_account_json;
    return auth;
}

RegistryAuth RegistryAuth::Aws(const std::string& access_key_id,
                               const std::string& secret_access_key,
                               const std::string& region) {
    RegistryAuth auth;
    auth.kind = kAws;
    auth.access_key_id = access_key_id;
    auth.secret_access_key = secret_access_key;
    auth.region = region;
    return auth;
}

core::Expected<core::Unit, std::string> RegistryAuth::Validate() const {
    switch (kind) {
        case kGeneral:
            if (username.empty() || password.empty()) {
                return core::make_unexpected(
                    std::string("registry username and password must be non-empty"));
            }
            break;
        case kGcp: {
            const core::Expected<core::Json, core::AnyError> key =
                core::Json::Parse(service_account_json);
            if (!key.ok() || key.value().kind() != core::Json::Kind::Object) {
                return core::make_unexpected(
                    std::string("serviceAccountJson must be a JSON object"));
            }
            break;
        }
        case kAws: {
            if (access_key_id.empty() || secret_access_key.empty()) {
                return core::make_unexpected(std::string("AWS access keys must be non-empty"));
            }
            bool region_ok = !region.empty();
            for (std::size_t i = 0; region_ok && i < region.size(); ++i) {
                const char c = region[i];
                region_ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            }
            if (!region_ok) return core::make_unexpected(std::string("invalid AWS region"));
            break;
        }
    }
    return core::Unit();
}

EcrAuthorizationRequest BuildEcrAuthorizationRequest(const std::string& access_key_id,
                                                     const std::string& secret_access_key,
                                                     const std::string& region,
                                                     const std::string& amz_date) {
    const std::string suffix =
        region.compare(0, 3, "cn-") == 0 ? "amazonaws.com.cn" : "amazonaws.com";
    const std::string host = "api.ecr." + region + "." + suffix;
    const std::string body = "{}";
    const std::string payload_hash = core::Sha256Hex(body);

    // Every header sent is signed, as `reqsign::AwsV4Signer` does: `x-amz-target`
    // selects the operation, so it must not be replaceable on the path.
    overlaybd::http::HeaderMap headers;
    headers["content-type"] = "application/x-amz-json-1.1";
    headers["x-amz-target"] = "AmazonEC2ContainerRegistry_V20150921.GetAuthorizationToken";
    headers["x-amz-content-sha256"] = payload_hash;
    headers["x-amz-date"] = amz_date;
    overlaybd::http::HeaderMap signed_headers = headers;
    signed_headers["host"] = host;

    overlaybd::S3Credentials credentials;
    credentials.access_key_id = access_key_id;
    credentials.secret_access_key = secret_access_key;
    overlaybd::S3SigningContext context;
    context.method = "POST";
    context.canonical_uri = "/";
    context.host = host;
    context.region = region;
    context.service = "ecr";
    context.payload_sha256_hex = payload_hash;
    context.amz_date = amz_date;
    headers["authorization"] =
        overlaybd::S3BuildAuthorization(credentials, context, signed_headers);

    EcrAuthorizationRequest request;
    request.url = "https://" + host + "/";
    request.body = body;
    for (overlaybd::http::HeaderMap::const_iterator it = headers.begin(); it != headers.end();
         ++it) {
        request.headers[it->first] = it->second;
    }
    return request;
}

core::Expected<std::pair<std::string, std::string>, std::string> ParseEcrAuthorizationResponse(
    const std::string& body) {
    const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(body);
    if (!value.ok()) {
        return core::make_unexpected(std::string("decode ECR authorization response"));
    }
    const std::string no_token = "ECR returned no authorization token";
    const core::Json& data = value.value()["authorizationData"];
    if (data.kind() != core::Json::Kind::Array || data.as_array().empty()) {
        return core::make_unexpected(no_token);
    }
    const core::Json& token = data.as_array()[0]["authorizationToken"];
    if (token.kind() != core::Json::Kind::String) return core::make_unexpected(no_token);

    const core::Expected<std::string, std::string> decoded =
        core::Base64StandardDecode(token.as_string());
    if (!decoded.ok() || !IsUtf8(decoded.value())) {
        return core::make_unexpected(std::string("invalid ECR authorization token"));
    }
    const std::size_t colon = decoded.value().find(':');
    if (colon == std::string::npos) {
        return core::make_unexpected(std::string("invalid ECR authorization token format"));
    }
    return std::make_pair(decoded.value().substr(0, colon), decoded.value().substr(colon + 1));
}

namespace {

core::Expected<std::pair<std::string, std::string>, std::string> EcrCredentials(
    const std::string& access_key_id, const std::string& secret_access_key,
    const std::string& region) {
    const EcrAuthorizationRequest signed_request = BuildEcrAuthorizationRequest(
        access_key_id, secret_access_key, region, overlaybd::S3AmzDateNow());

    overlaybd::http::Request request;
    request.method = "POST";
    request.url = signed_request.url;
    for (std::map<std::string, std::string>::const_iterator it = signed_request.headers.begin();
         it != signed_request.headers.end(); ++it) {
        request.headers[it->first] = it->second;
    }
    request.body.assign(signed_request.body.begin(), signed_request.body.end());
    request.timeout_secs = 30;
    request.follow_redirects = false;

    const core::Expected<overlaybd::http::Response, std::string> response =
        overlaybd::http::Perform(request);
    if (!response.ok()) {
        return core::make_unexpected("request ECR authorization token: " + response.error());
    }
    if (!response.value().IsSuccess()) {
        return core::make_unexpected("ECR authorization request failed with HTTP " +
                                     std::to_string(response.value().status));
    }
    const std::vector<uint8_t>& body = response.value().body;
    return ParseEcrAuthorizationResponse(std::string(body.begin(), body.end()));
}

}  // namespace

core::Expected<RegistryConfig, std::string> RegistryAuth::Config(const std::string& host) const {
    switch (kind) {
        case kGcp:
            return RegistryConfig::Create(host, "_json_key", service_account_json);
        case kAws: {
            const core::Expected<std::pair<std::string, std::string>, std::string> credentials =
                EcrCredentials(access_key_id, secret_access_key, region);
            if (!credentials.ok()) return core::make_unexpected(credentials.error());
            return RegistryConfig::Create(host, credentials.value().first,
                                          credentials.value().second);
        }
        case kGeneral:
        default:
            return RegistryConfig::Create(host, username, password);
    }
}

// ---- regctl ----

RegctlCommand MakeRegctlCommand(const std::string& binary,
                                const core::Optional<std::string>& registry_config) {
    RegctlCommand command;
    command.argv.push_back(binary);
    if (registry_config.has_value()) {
        command.env.push_back(std::make_pair(std::string("REGCTL_CONFIG"), *registry_config));
    }
    command.env.push_back(
        std::make_pair(std::string("GOMAXPROCS"), std::string(kRegctlGomaxprocs)));
    return command;
}

core::Expected<core::Unit, ImageError> EnsureRegctlBinary(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        return core::Unit();
    }
    return core::make_unexpected(OtherError("regctl is required for OCI registry access: " + path));
}

core::Expected<core::process::Output, ImageError> RunRegctl(
    const std::string& regctl_binary, const std::vector<std::string>& args,
    const core::Optional<std::string>& registry_config) {
    const core::Expected<core::Unit, ImageError> ensured = EnsureRegctlBinary(regctl_binary);
    if (!ensured.ok()) return core::make_unexpected(ensured.error());

    RegctlCommand command = MakeRegctlCommand(regctl_binary, registry_config);
    command.argv.insert(command.argv.end(), args.begin(), args.end());
    core::process::RunOptions options;
    options.env = command.env;

    int64_t backoff_ms = kRegctlRetryBaseDelayMs;
    std::string last_stderr;
    for (uint32_t attempt = 1; attempt <= kRegctlRetryAttempts; ++attempt) {
        // `Child`, not `Run`: `Command::output` reports a failed exec as an
        // error immediately, where `Run` would turn it into exit 127 and spend
        // the whole retry budget on a binary that can never start.
        core::Expected<std::shared_ptr<core::process::Child>, std::string> child =
            core::process::Child::Spawn(command.argv, options);
        if (!child.ok()) {
            return core::make_unexpected(OtherError("spawn regctl: " + child.error()));
        }
        const core::Expected<int, std::string> code = child.value()->Wait();
        if (!code.ok()) return core::make_unexpected(OtherError("spawn regctl: " + code.error()));

        core::process::Output output;
        output.code = code.value();
        output.stdout_text = child.value()->StdoutBytes();
        output.stderr_text = child.value()->StderrBytes();
        if (output.success()) return output;

        last_stderr = TrimAscii(core::FromUtf8Lossy(output.stderr_text));
        if (RegctlStderrIsNotFound(last_stderr)) {
            return core::make_unexpected(
                NotFound("regctl " + JoinArgs(args) +
                         " reported the OCI resource does not exist: " + last_stderr));
        }
        if (attempt < kRegctlRetryAttempts) {
            AGENTENV_WARN("regctl failed; retrying: attempt=" << attempt << " args="
                                                              << JoinArgs(args)
                                                              << " error=" << last_stderr);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms *= 2;
        }
    }
    return core::make_unexpected(OtherError("regctl " + JoinArgs(args) + " failed after " +
                                            std::to_string(kRegctlRetryAttempts) +
                                            " attempts: " + last_stderr));
}

// ---- manifest fetch ----

namespace {

/// Rust `fetch_manifest`.
core::Expected<FetchedManifest, ImageError> FetchManifest(
    const std::string& regctl_binary, const std::string& image_ref, const std::string& arch,
    const std::string& os, const core::Optional<std::string>& registry_config) {
    std::string current_ref = image_ref;
    for (std::size_t depth = 0; depth < kMaxIndexResolutionDepth; ++depth) {
        std::vector<std::string> args;
        args.push_back("manifest");
        args.push_back("get");
        args.push_back("--format");
        args.push_back("raw-body");
        args.push_back(current_ref);
        const core::Expected<core::process::Output, ImageError> output =
            RunRegctl(regctl_binary, args, registry_config);
        if (!output.ok()) return core::make_unexpected(output.error());

        const std::string& raw = output.value().stdout_text;
        if (!IsUtf8(raw)) {
            return core::make_unexpected(
                OtherError("regctl manifest get output is not UTF-8 for " + current_ref));
        }
        const std::string parse_context = "parse manifest JSON for " + current_ref;
        const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(raw);
        if (!value.ok()) {
            return core::make_unexpected(
                OtherError(parse_context + ": " + value.error().chain()));
        }

        const core::Json& media_type_field = value.value()["mediaType"];
        const bool has_media_type = media_type_field.kind() == core::Json::Kind::String;
        const bool has_index_manifests =
            value.value()["manifests"].kind() == core::Json::Kind::Array;
        bool media_type_is_index = false;
        if (has_media_type) {
            for (std::size_t i = 0; i < 2; ++i) {
                if (media_type_field.as_string() == kOciIndexMediaTypes[i]) {
                    media_type_is_index = true;
                }
            }
        }
        const bool is_index = media_type_is_index || (!has_media_type && has_index_manifests);

        if (!is_index) {
            // Hashes only the manifest JSON (normally KB-scale) to derive the
            // immutable manifest digest, not any layer data.
            FetchedManifest fetched;
            fetched.manifest_digest = core::Sha256Digest(raw);
            const core::Expected<OciManifest, std::string> manifest =
                ParseOciManifest(value.value());
            if (!manifest.ok()) {
                return core::make_unexpected(OtherError(parse_context + ": " + manifest.error()));
            }
            fetched.manifest = manifest.value();

            const core::Expected<ImageFormat, ImageError> format =
                ClassifyManifest(fetched.manifest);
            if (!format.ok()) return core::make_unexpected(format.error());
            fetched.format = format.value();

            const core::Expected<std::string, std::string> selected =
                ImageRefWithDigest(current_ref, fetched.manifest_digest);
            if (!selected.ok()) return core::make_unexpected(OtherError(selected.error()));
            fetched.selected_image_ref = selected.value();

            if (fetched.format == ImageFormat::OverlaybdNative) {
                const core::Expected<ParsedImageRef, std::string> parsed =
                    ParseImageRef(fetched.selected_image_ref);
                if (!parsed.ok()) return core::make_unexpected(OtherError(parsed.error()));
                fetched.repository_scope = core::Optional<std::string>(
                    RepositoryCacheKey(parsed.value().host, parsed.value().repository));
            }
            return fetched;
        }

        const core::Expected<OciIndex, std::string> index = ParseOciIndex(value.value());
        if (!index.ok()) {
            return core::make_unexpected(
                OtherError("parse OCI index for " + current_ref + ": " + index.error()));
        }
        const core::Expected<std::size_t, std::string> selected =
            SelectManifestForPlatform(index.value().manifests, arch, os);
        if (!selected.ok()) return core::make_unexpected(OtherError(selected.error()));
        const core::Expected<std::string, std::string> next = ImageRefWithDigest(
            current_ref, index.value().manifests[selected.value()].digest);
        if (!next.ok()) return core::make_unexpected(OtherError(next.error()));
        current_ref = next.value();
    }
    return core::make_unexpected(OtherError(
        "OCI manifest index nesting exceeded maximum depth of " +
        std::to_string(kMaxIndexResolutionDepth) + " while resolving " + image_ref));
}

}  // namespace

core::Expected<FetchedManifest, ImageError> FetchOciManifest(
    const std::string& regctl_binary, const std::string& image_ref, const std::string& host_arch,
    const core::Optional<std::string>& registry_config) {
    const core::Expected<core::Unit, ImageError> ensured = EnsureRegctlBinary(regctl_binary);
    if (!ensured.ok()) return core::make_unexpected(ensured.error());
    const core::Expected<std::string, std::string> oci_arch = HostArchToOci(host_arch);
    if (!oci_arch.ok()) return core::make_unexpected(OtherError(oci_arch.error()));
    return FetchManifest(regctl_binary, image_ref, oci_arch.value(), "linux", registry_config);
}

core::Expected<ImageResolutionMetadata, ImageError> FetchOciImageConfigMetadata(
    const std::string& regctl_binary, const std::string& image_ref,
    const std::string& config_digest, const core::Optional<std::string>& registry_config) {
    const core::Expected<std::string, std::string> repository = ImageRefRepository(image_ref);
    if (!repository.ok()) return core::make_unexpected(OtherError(repository.error()));

    std::vector<std::string> args;
    args.push_back("blob");
    args.push_back("get");
    args.push_back(repository.value());
    args.push_back(config_digest);
    const core::Expected<core::process::Output, ImageError> output =
        RunRegctl(regctl_binary, args, registry_config);
    if (!output.ok()) return core::make_unexpected(output.error());

    const std::string& bytes = output.value().stdout_text;
    const core::Expected<core::Unit, std::string> verified =
        VerifyBlobDigest(config_digest, bytes.data(), bytes.size());
    if (!verified.ok()) {
        return core::make_unexpected(
            OtherError("verify image config blob for " + image_ref + ": " + verified.error()));
    }
    if (!IsUtf8(bytes)) {
        return core::make_unexpected(
            OtherError("image config blob for " + image_ref + " is not UTF-8"));
    }
    const core::Expected<ImageResolutionMetadata, std::string> metadata =
        ParseOciImageConfig(bytes);
    if (!metadata.ok()) {
        return core::make_unexpected(
            OtherError("parse OCI image config for " + image_ref + ": " + metadata.error()));
    }
    const ImageBaseContext& ctx = metadata.value().base_context;
    AGENTENV_DEBUG("resolved OCI image runtime metadata: image="
                   << image_ref << " config_digest=" << config_digest
                   << " user=" << (ctx.user.has_value() ? *ctx.user : std::string("(none)"))
                   << " workdir="
                   << (ctx.workdir.has_value() ? *ctx.workdir : std::string("(none)"))
                   << " exposed_ports=" << ctx.exposed_ports.size()
                   << " volumes=" << ctx.volumes.size() << " env_vars=" << ctx.env_vars.size()
                   << " labels=" << ctx.labels.size());
    return metadata.value();
}

}  // namespace image
}  // namespace agentenv
