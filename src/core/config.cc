// SPDX-License-Identifier: MIT
#include "agentenv/core/config.h"

#include <fstream>
#include <sstream>

namespace agentenv {
namespace core {

// A tiny TOML *subset* loader: supports `key = value` where value is
// quoted-string, bare-integer, or literal `true`/`false`. Section headers
// [foo.bar] are honored via `foo.bar.key = ...` flattening.
// No arrays / nested tables in this skeleton.
static bool StripQuotes(std::string* s) {
    if (s->size() >= 2 && (*s)[0] == '"' && s->back() == '"') {
        *s = s->substr(1, s->size() - 2);
        return true;
    }
    return false;
}

Expected<Config, AnyError> Config::ParseString(const std::string& text) {
    Config c;
    std::istringstream in(text);
    std::string line, section;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        // trim
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                                   s.back() == '\r' || s.back() == '\n')) s.pop_back();
        };
        trim(line);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos)
            return make_unexpected(err("config parse: line " + std::to_string(lineno)));
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key); trim(val);
        StripQuotes(&val);
        std::string full = section.empty() ? key : section + "." + key;

        try {
            if      (full == "server_bind_addr")     c.server_bind_addr = val;
            else if (full == "data_dir")             c.data_dir = val;
            else if (full == "log_level")            c.log_level = std::stoi(val);
            else if (full == "num_workers")          c.num_workers = std::stoi(val);
            else if (full == "sandbox_backend")      c.sandbox_backend = val;
            else if (full == "snapshot.repo_kind")   c.snapshot_repo_kind = val;
            else if (full == "snapshot.repo_path")   c.snapshot_repo_path = val;
            else if (full == "overlaybd.enable")     c.overlaybd_enable = (val == "true");
            else if (full == "ublk.enable")          c.ublk_enable = (val == "true");
            // unknown keys are ignored, matching a forward-compatible policy.
        } catch (const std::exception& e) {
            return make_unexpected(err("config: bad value for " + full + ": " + e.what()));
        }
    }
    return c;
}

Expected<Config, AnyError> Config::Load(const std::string& toml_path) {
    std::ifstream in(toml_path);
    if (!in.is_open()) return make_unexpected(err("cannot open config: " + toml_path));
    std::ostringstream oss;
    oss << in.rdbuf();
    return ParseString(oss.str());
}

}  // namespace core
}  // namespace agentenv
