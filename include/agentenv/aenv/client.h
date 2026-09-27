// SPDX-License-Identifier: MIT
// Rust: crates/aenv/src/client/*.rs — thin HTTP client for the API server.
#ifndef AGENTENV_AENV_CLIENT_H_
#define AGENTENV_AENV_CLIENT_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace aenv {
namespace client {

/// Rust: shared HTTP client transport (crates/aenv/src/client/mod.rs).
class HttpClient {
 public:
    virtual ~HttpClient() {}
    virtual core::Expected<std::string, std::string>
        DoRequest(const std::string& method,
                  const std::string& path,
                  const std::string& body) = 0;
};

// Rust: client/sandboxes.rs
core::Expected<std::string, std::string>
    SandboxesCreate(HttpClient* c, const std::string& template_id);
core::Expected<std::string, std::string>
    SandboxesGet(HttpClient* c, const std::string& sandbox_id);
core::Expected<core::Unit, std::string>
    SandboxesDelete(HttpClient* c, const std::string& sandbox_id);
core::Expected<std::string, std::string>
    SandboxesList(HttpClient* c);

// Rust: client/snapshots.rs
core::Expected<std::string, std::string>
    SnapshotsCreate(HttpClient* c, const std::string& sandbox_id);
core::Expected<std::string, std::string>
    SnapshotsList(HttpClient* c);

// Rust: client/templates.rs
core::Expected<std::string, std::string>
    TemplatesList(HttpClient* c);

// Rust: client/files.rs — upload/download blobs to a running sandbox.
core::Expected<core::Unit, std::string>
    FilesUpload(HttpClient* c,
                const std::string& sandbox_id,
                const std::string& remote_path,
                const std::string& local_path);
core::Expected<core::Unit, std::string>
    FilesDownload(HttpClient* c,
                  const std::string& sandbox_id,
                  const std::string& remote_path,
                  const std::string& local_path);

}  // namespace client
}  // namespace aenv
}  // namespace agentenv
#endif  // AGENTENV_AENV_CLIENT_H_
