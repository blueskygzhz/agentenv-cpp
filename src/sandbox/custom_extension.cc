// SPDX-License-Identifier: MIT
// Rust: src/sandbox/custom_extension/{client,mod}.rs, plus the generated
// OpenAPI client it calls (src/custom_extension_api/generated).
#include "agentenv/sandbox/custom_extension.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>

#include "agentenv/cfg.h"
#include "agentenv/core/executor.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace sandbox {
namespace custom_extension {

namespace {

/// The generated client `Configuration::default().user_agent`.
const char* const kUserAgent = "OpenAPI-Generator/0.1.0/rust";

std::string Trim(const std::string& s) {
    const char* const ws = " \t\r\n\f\v";
    const std::string::size_type b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    const std::string::size_type e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

/// `http::StatusCode::canonical_reason`.
const char* CanonicalReason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default: return NULL;
    }
}

std::string Quote(const std::string& s) { return core::Json(s).ToString(); }

/// The generated client treats everything that is neither a client nor a
/// server error as success (`!is_client_error() && !is_server_error()`).
bool IsErrorStatus(int status) { return status >= 400 && status < 600; }

/// Rust `custom_extension_params.cloned().map(Value::Object)
/// .unwrap_or_else(empty_params)` -- the caller's own text when present.
core::Expected<std::string, core::AnyError> ParamsJson(const core::Optional<Params>& params) {
    if (ParamsIsEmpty(params)) return std::string("{}");
    const std::string text = Trim(params->json_bytes);
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
    if (!parsed.ok()) {
        return core::make_unexpected(
            parsed.error().context("custom extension params are not valid JSON"));
    }
    if (parsed.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            core::err("custom extension params must be a JSON object: " + text));
    }
    return text;
}

/// Rust `hook_error` for every non-status failure:
/// `anyhow::Error::new(other).context("custom extension {op} hook failed")`,
/// where `other` displays as the generated `error in {module}: {e}`.
core::AnyError HookFailure(const char* operation, const char* module, const std::string& e) {
    return core::AnyError(std::string("error in ") + module + ": " + e)
        .context(std::string("custom extension ") + operation + " hook failed");
}

/// The generated success path for a modelled response: the body is decoded
/// only for an `application/*json*` content type (an absent one counts as
/// `application/octet-stream`), and must be the model struct, a JSON object.
core::Expected<core::JsonObject, std::string> DecodeModel(const core::http::Response& response,
                                                          const char* model) {
    const core::Optional<std::string> header =
        core::http::FindHeader(response.headers, "content-type");
    const std::string content_type =
        header.has_value() ? *header : std::string("application/octet-stream");
    if (!(StartsWith(content_type, "application") &&
          content_type.find("json") != std::string::npos)) {
        const std::string shown =
            StartsWith(content_type, "text/plain") ? std::string("text/plain") : content_type;
        return core::make_unexpected("Received `" + shown +
                                     "` content type response that cannot be converted to "
                                     "`models::" + model + "`");
    }
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(response.body);
    if (!parsed.ok()) return core::make_unexpected(parsed.error().chain());
    if (parsed.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("invalid type: ") + response.body +
                                     ", expected struct " + model);
    }
    return parsed.value().as_object();
}

/// A member of a decoded model, or NULL when absent or `null` (serde maps
/// both to `None`).
const core::Json* Member(const core::JsonObject& object, const char* name) {
    const core::JsonObject::const_iterator it = object.find(name);
    if (it == object.end() || it->second.kind() == core::Json::Kind::Null) return NULL;
    return &it->second;
}

}  // namespace

bool ParamsIsEmpty(const Params& p) {
    const std::string text = Trim(p.json_bytes);
    if (text.empty() || text == "null") return true;
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
    if (!parsed.ok()) return false;
    if (parsed.value().kind() == core::Json::Kind::Null) return true;
    return parsed.value().kind() == core::Json::Kind::Object &&
           parsed.value().as_object().empty();
}

bool ParamsIsEmpty(const core::Optional<Params>& p) {
    return !p.has_value() || ParamsIsEmpty(*p);
}

SandboxInstanceId SandboxInstanceId::New() {
    SandboxInstanceId id;
    id.value = core::Uuid::GenV7();
    return id;
}

std::string StatusCodeDisplay(int status) {
    const char* reason = CanonicalReason(status);
    return std::to_string(status) + " " + (reason != NULL ? reason : "<unknown status code>");
}

std::string HookStatusError::ToString() const {
    return "custom extension " + operation_ + " hook failed with status " +
           StatusCodeDisplay(status_) + ": " + content_;
}

core::AnyError HookStatusError::ToAnyError() const {
    return core::AnyError(ToString(), std::make_shared<HookStatusError>(*this));
}

const HookStatusError* FindHookStatusError(const core::AnyError& err) {
    return err.Find<HookStatusError>();
}

// ---- client ---------------------------------------------------------------

std::shared_ptr<Client> Client::Global() {
    // Rust `GLOBAL_CLIENT.get_or_init(build_from_config)`. Cached only once
    // the config exists: an early call in a process that loads its config
    // later would otherwise latch "no extension" and never retry.
    static std::shared_ptr<Client> cached;
    static bool built = false;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (built) return cached;
    const cfg::AppConfig* app = cfg::ConfigManager::GlobalConfig();
    if (app == NULL) return cached;
    built = true;
    cached = HttpClient::FromConfig(app->custom_extension.url,
                                    app->custom_extension.timeout_ms);
    return cached;
}

/// `http://host[:port][/prefix]` -> target + prefix.
bool ParseHttpUrl(const std::string& url, core::http::Target* target, std::string* prefix) {
    const std::string scheme = "http://";
    if (!StartsWith(url, scheme)) return false;
    const std::string rest = url.substr(scheme.size());
    const std::string::size_type slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    *prefix = slash == std::string::npos ? std::string() : rest.substr(slash);
    while (!prefix->empty() && (*prefix)[prefix->size() - 1] == '/') {
        prefix->erase(prefix->size() - 1);
    }
    std::string host = authority;
    unsigned long port = 80;
    const std::string::size_type colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string digits = authority.substr(colon + 1);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        port = ::strtoul(digits.c_str(), NULL, 10);
        if (port == 0 || port > 65535) return false;
    }
    if (host.empty()) return false;
    *target = core::http::Target::Tcp(host, static_cast<uint16_t>(port));
    return true;
}

std::shared_ptr<HttpClient> HttpClient::FromConfig(const core::Optional<std::string>& url,
                                                   uint64_t timeout_ms) {
    if (!url.has_value()) return std::shared_ptr<HttpClient>();
    const std::string trimmed = Trim(*url);
    if (trimmed.empty()) return std::shared_ptr<HttpClient>();
    // Rust `base_path: url.trim_end_matches('/')`.
    std::string base = trimmed;
    while (!base.empty() && base[base.size() - 1] == '/') base.erase(base.size() - 1);
    if (StartsWith(base, "http://")) {
        core::http::Target target;
        std::string prefix;
        if (ParseHttpUrl(base, &target, &prefix)) {
            core::http::Timeouts timeouts;
            const int64_t ms = static_cast<int64_t>(timeout_ms);
            if (timeout_ms == 0 || ms < 0) {
                timeouts.connect_ms = 0;
                timeouts.read_ms = 0;
            } else {
                // reqwest's timeout bounds the whole request; this client can
                // only bound the connect and each read separately.
                timeouts.connect_ms = ms;
                timeouts.read_ms = ms;
            }
            return std::shared_ptr<HttpClient>(new HttpClient(target, prefix, timeouts));
        }
        AGENTENV_WARN("custom extension url is not a usable http://host[:port][/prefix] "
                      "address: " << base);
        return std::shared_ptr<HttpClient>();
    }
    // No TLS client in this build: an https:// URL disables the integration,
    // as Rust does when its HTTP client cannot be built.
    AGENTENV_WARN("custom extension url is not http://; the custom extension integration is "
                  "disabled: " << base);
    return std::shared_ptr<HttpClient>();
}

core::Expected<core::http::Response, std::string> HttpClient::Post(const char* operation,
                                                                   const std::string& body) const {
    core::http::Request request;
    request.method = "POST";
    request.path = path_prefix_ + std::string("/sandbox-hook/") + operation;
    request.headers["Content-Type"] = "application/json";
    request.headers["Accept"] = "application/json";
    request.headers["User-Agent"] = kUserAgent;
    request.body = body;
    const core::http::HttpClient client(target_, timeouts_);
    return client.Request(request);
}

core::Expected<core::Optional<std::string>, core::AnyError> HttpClient::HookStartFresh(
    const core::SandboxId& sandbox_id, const SandboxInstanceId& instance_id,
    const std::string& network_namespace_path, const std::string& host_interaction_ip,
    const core::Optional<Params>& params) {
    const core::Expected<std::string, core::AnyError> params_json = ParamsJson(params);
    if (!params_json.ok()) return core::make_unexpected(params_json.error());
    const std::string body =
        "{\"sandboxId\":" + Quote(sandbox_id.ToString()) +
        ",\"sandboxInstanceId\":" + Quote(instance_id.ToString()) +
        ",\"networkNamespacePath\":" + Quote(network_namespace_path) +
        ",\"hostInteractionIp\":" + Quote(host_interaction_ip) +
        ",\"customExtensionParams\":" + params_json.value() + "}";
    const core::Expected<core::http::Response, std::string> response =
        Post(hook::kStartFresh, body);
    if (!response.ok()) {
        return core::make_unexpected(
            HookFailure(hook::kStartFresh, "reqwest", response.error()));
    }
    if (IsErrorStatus(response.value().status)) {
        return core::make_unexpected(
            HookStatusError(hook::kStartFresh, response.value().status,
                            response.value().body).ToAnyError());
    }
    const core::Expected<core::JsonObject, std::string> model =
        DecodeModel(response.value(), "StartFreshHookResponse");
    if (!model.ok()) {
        return core::make_unexpected(
            HookFailure(hook::kStartFresh, "serde", model.error()));
    }
    // Rust `response.extra_boot_args.flatten()` -- absent, null or "" -> None.
    const core::Json* extra = Member(model.value(), "extraBootArgs");
    if (extra == NULL || extra->kind() != core::Json::Kind::String) {
        return core::Optional<std::string>();
    }
    const std::string value = extra->as_string();
    if (value.empty()) return core::Optional<std::string>();
    return core::Optional<std::string>(value);
}

core::Expected<core::Unit, core::AnyError> HttpClient::HookStartResume(
    const core::SandboxId& sandbox_id, const SandboxInstanceId& instance_id,
    const std::string& network_namespace_path, const std::string& host_interaction_ip,
    const core::Optional<Params>& params) {
    const core::Expected<std::string, core::AnyError> params_json = ParamsJson(params);
    if (!params_json.ok()) return core::make_unexpected(params_json.error());
    const std::string body =
        "{\"sandboxId\":" + Quote(sandbox_id.ToString()) +
        ",\"sandboxInstanceId\":" + Quote(instance_id.ToString()) +
        ",\"networkNamespacePath\":" + Quote(network_namespace_path) +
        ",\"hostInteractionIp\":" + Quote(host_interaction_ip) +
        ",\"customExtensionParams\":" + params_json.value() + "}";
    const core::Expected<core::http::Response, std::string> response =
        Post(hook::kStartResume, body);
    if (!response.ok()) {
        return core::make_unexpected(
            HookFailure(hook::kStartResume, "reqwest", response.error()));
    }
    // The generated call has no response model: only the status is read.
    if (IsErrorStatus(response.value().status)) {
        return core::make_unexpected(
            HookStatusError(hook::kStartResume, response.value().status,
                            response.value().body).ToAnyError());
    }
    return core::Unit();
}

core::Expected<core::Optional<Params>, core::AnyError> HttpClient::HookPatchParams(
    const core::SandboxId& sandbox_id, const Params& patch) {
    const core::Expected<std::string, core::AnyError> patch_json =
        ParamsJson(core::Optional<Params>(patch));
    if (!patch_json.ok()) return core::make_unexpected(patch_json.error());
    const std::string body = "{\"sandboxId\":" + Quote(sandbox_id.ToString()) +
                             ",\"patch\":" + patch_json.value() + "}";
    const core::Expected<core::http::Response, std::string> response =
        Post(hook::kPatchParams, body);
    if (!response.ok()) {
        return core::make_unexpected(
            HookFailure(hook::kPatchParams, "reqwest", response.error()));
    }
    if (IsErrorStatus(response.value().status)) {
        return core::make_unexpected(
            HookStatusError(hook::kPatchParams, response.value().status,
                            response.value().body).ToAnyError());
    }
    const core::Expected<core::JsonObject, std::string> model =
        DecodeModel(response.value(), "PatchCustomExtensionParamsHookResponse");
    if (!model.ok()) {
        return core::make_unexpected(
            HookFailure(hook::kPatchParams, "serde", model.error()));
    }
    const core::Json* value = Member(model.value(), "customExtensionParams");
    if (value == NULL) return core::Optional<Params>();
    if (value->kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            core::err("custom extension patch-params hook returned non-object params: " +
                      value->ToString()));
    }
    Params params;
    params.json_bytes = value->ToString();
    return core::Optional<Params>(params);
}

void HttpClient::HookStop(const core::SandboxId& sandbox_id,
                          const SandboxInstanceId& instance_id) {
    const std::string body = "{\"sandboxId\":" + Quote(sandbox_id.ToString()) +
                             ",\"sandboxInstanceId\":" + Quote(instance_id.ToString()) + "}";
    const core::Expected<core::http::Response, std::string> response =
        Post(hook::kStop, body);
    if (!response.ok()) {
        AGENTENV_WARN("custom extension stop hook failed: " << response.error()
                      << " sandbox_id=" << sandbox_id.ToString()
                      << " sandbox_instance_id=" << instance_id.ToString());
    } else if (IsErrorStatus(response.value().status)) {
        AGENTENV_WARN("custom extension stop hook failed with status "
                      << response.value().status << ": " << response.value().body
                      << " sandbox_id=" << sandbox_id.ToString()
                      << " sandbox_instance_id=" << instance_id.ToString());
    }
}

// ---- hook guard -----------------------------------------------------------

HookGuard::HookGuard(std::shared_ptr<Client> client, core::SandboxId sandbox_id)
    : client_(std::move(client)), sandbox_id_(sandbox_id) {}

HookGuard::~HookGuard() {
    if (!instance_id_.has_value()) return;
    const SandboxInstanceId instance_id = *instance_id_;
    instance_id_.reset();
    std::shared_ptr<Client> client = client_;
    const core::SandboxId sandbox_id = sandbox_id_;
    // Rust spawns the notification on the current Tokio runtime so the drop
    // never blocks; the shared executor is this port's equivalent. A drop
    // after the executor is shut down silently drops the notification, which
    // is the same outcome a dropped Tokio task has.
    core::GlobalExecutor().Post([client, sandbox_id, instance_id]() {
        client->HookStop(sandbox_id, instance_id);
    });
}

core::Expected<core::Optional<std::string>, core::AnyError> HookGuard::StartFresh(
    const std::string& network_namespace_path, const std::string& host_interaction_ip,
    const core::Optional<Params>& params) {
    // Recorded before delivery: if the request fails after the extension
    // already processed it (e.g. a client-side timeout), teardown still
    // delivers the matching stop.
    const SandboxInstanceId instance_id = SandboxInstanceId::New();
    instance_id_ = core::Optional<SandboxInstanceId>(instance_id);
    const core::Expected<core::Optional<std::string>, core::AnyError> extra =
        client_->HookStartFresh(sandbox_id_, instance_id, network_namespace_path,
                                host_interaction_ip, params);
    if (!extra.ok()) return extra;
    AGENTENV_DEBUG("custom extension start-fresh hook delivered: sandbox_id="
                   << sandbox_id_.ToString() << " sandbox_instance_id="
                   << instance_id.ToString());
    return extra;
}

core::Expected<core::Unit, core::AnyError> HookGuard::StartResume(
    const std::string& network_namespace_path, const std::string& host_interaction_ip,
    const core::Optional<Params>& params) {
    const SandboxInstanceId instance_id = SandboxInstanceId::New();
    instance_id_ = core::Optional<SandboxInstanceId>(instance_id);
    const core::Expected<core::Unit, core::AnyError> r = client_->HookStartResume(
        sandbox_id_, instance_id, network_namespace_path, host_interaction_ip, params);
    if (!r.ok()) return r;
    AGENTENV_DEBUG("custom extension start-resume hook delivered: sandbox_id="
                   << sandbox_id_.ToString() << " sandbox_instance_id="
                   << instance_id.ToString());
    return core::Unit();
}

void HookGuard::Stop() {
    if (!instance_id_.has_value()) return;
    const SandboxInstanceId instance_id = *instance_id_;
    instance_id_.reset();
    if (client_) client_->HookStop(sandbox_id_, instance_id);
}

}  // namespace custom_extension
}  // namespace sandbox
}  // namespace agentenv
