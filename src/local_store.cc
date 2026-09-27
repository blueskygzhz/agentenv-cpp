// SPDX-License-Identifier: MIT
// Rust: src/local_store.rs — skeleton; a real impl uses fsync + atomic rename.
#include "agentenv/local_store.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace agentenv {

static std::string encode_key(const std::string& key) {
    std::string out;
    out.reserve(key.size() * 2);
    for (char c : key) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            out.push_back(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        }
    }
    return out;
}

FsLocalStore::FsLocalStore(std::string root_dir) : root_(std::move(root_dir)) {
    ::mkdir(root_.c_str(), 0755);
}

core::Expected<std::string, std::string>
FsLocalStore::Get(const std::string& key) {
    std::string path = root_ + "/" + encode_key(key);
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return core::make_unexpected(std::string("not found: ") + key);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

core::Expected<core::Unit, std::string>
FsLocalStore::Put(const std::string& key, const std::string& value) {
    std::string path = root_ + "/" + encode_key(key);
    std::string tmp  = path + ".tmp";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return core::make_unexpected(std::string("open tmp failed"));
        f.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        return core::make_unexpected(std::string(::strerror(errno)));
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
FsLocalStore::Delete(const std::string& key) {
    std::string path = root_ + "/" + encode_key(key);
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        return core::make_unexpected(std::string(::strerror(errno)));
    }
    return core::Unit{};
}

core::Expected<std::vector<std::string>, std::string>
FsLocalStore::ListKeys(const std::string& prefix) {
    DIR* d = ::opendir(root_.c_str());
    if (!d) return core::make_unexpected(std::string("opendir failed"));
    std::vector<std::string> out;
    const std::string enc_prefix = encode_key(prefix);
    while (true) {
        struct dirent* e = ::readdir(d);
        if (!e) break;
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (name.compare(0, enc_prefix.size(), enc_prefix) != 0) continue;
        out.push_back(name);
    }
    ::closedir(d);
    return out;
}

}  // namespace agentenv
