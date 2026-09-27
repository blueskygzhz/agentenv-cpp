// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/client/*.rs
#include "agentenv/aenv/client.h"

namespace agentenv {
namespace aenv {
namespace client {

static core::Expected<std::string, std::string>
call(HttpClient* c, const char* method, const std::string& path,
     const std::string& body = std::string()) {
    if (!c) return core::make_unexpected(std::string("null http client"));
    return c->DoRequest(method, path, body);
}

core::Expected<std::string, std::string>
SandboxesCreate(HttpClient* c, const std::string& template_id) {
    return call(c, "POST", "/sandboxes",
                std::string("{\"template_id\":\"") + template_id + "\"}");
}
core::Expected<std::string, std::string>
SandboxesGet(HttpClient* c, const std::string& id) {
    return call(c, "GET", "/sandboxes/" + id);
}
core::Expected<core::Unit, std::string>
SandboxesDelete(HttpClient* c, const std::string& id) {
    auto r = call(c, "DELETE", "/sandboxes/" + id);
    if (!r.ok()) return core::make_unexpected(r.error());
    return core::Unit{};
}
core::Expected<std::string, std::string>
SandboxesList(HttpClient* c) {
    return call(c, "GET", "/sandboxes");
}

core::Expected<std::string, std::string>
SnapshotsCreate(HttpClient* c, const std::string& id) {
    return call(c, "POST", "/sandboxes/" + id + "/snapshot");
}
core::Expected<std::string, std::string>
SnapshotsList(HttpClient* c) {
    return call(c, "GET", "/snapshots");
}

core::Expected<std::string, std::string>
TemplatesList(HttpClient* c) {
    return call(c, "GET", "/templates");
}

core::Expected<core::Unit, std::string>
FilesUpload(HttpClient* /*c*/, const std::string&, const std::string&,
            const std::string&) {
    // TODO: multipart upload against /files/{sandbox}/{path}
    return core::Unit{};
}
core::Expected<core::Unit, std::string>
FilesDownload(HttpClient* /*c*/, const std::string&, const std::string&,
              const std::string&) {
    return core::Unit{};
}

}}}  // namespace agentenv::aenv::client
