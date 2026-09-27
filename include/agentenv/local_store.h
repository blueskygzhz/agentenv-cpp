// SPDX-License-Identifier: MIT
// Rust: src/local_store.rs — on-disk key/value store for lightweight state.
#ifndef AGENTENV_LOCAL_STORE_H_
#define AGENTENV_LOCAL_STORE_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {

class LocalStore {
 public:
    virtual ~LocalStore() {}
    virtual core::Expected<std::string, std::string> Get(const std::string& key) = 0;
    virtual core::Expected<core::Unit, std::string>  Put(const std::string& key,
                                                         const std::string& value) = 0;
    virtual core::Expected<core::Unit, std::string>  Delete(const std::string& key) = 0;
    virtual core::Expected<std::vector<std::string>, std::string>
        ListKeys(const std::string& prefix) = 0;
};

/// Rust: filesystem-backed impl (one file per key under `root_dir`).
class FsLocalStore : public LocalStore {
 public:
    explicit FsLocalStore(std::string root_dir);
    core::Expected<std::string, std::string> Get(const std::string& key) override;
    core::Expected<core::Unit, std::string>  Put(const std::string& key,
                                                 const std::string& value) override;
    core::Expected<core::Unit, std::string>  Delete(const std::string& key) override;
    core::Expected<std::vector<std::string>, std::string>
        ListKeys(const std::string& prefix) override;
 private:
    std::string root_;
};

}  // namespace agentenv
#endif  // AGENTENV_LOCAL_STORE_H_
