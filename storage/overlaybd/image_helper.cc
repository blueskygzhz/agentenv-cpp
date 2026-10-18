// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/image/helper.rs
#include "agentenv/storage/overlaybd/image_helper.h"

#include <unistd.h>

#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

const char* const kLowerPathKeys[4] = {"file", "targetFile", "gzipIndex", "dir"};

namespace {

/// Rust `normalize_path`.
///
/// Lexical only, and absolute: a relative input is first joined onto the
/// process's working directory. Rust does the same, which is why this can
/// fail -- reading the working directory is a syscall.
core::Expected<std::string, std::string> NormalizePath(const std::string& path) {
    std::string absolute;
    if (!path.empty() && path[0] == '/') {
        absolute = path;
    } else {
        char buf[4096];
        if (::getcwd(buf, sizeof(buf)) == 0) {
            return core::make_unexpected(
                std::string("resolve current dir for overlaybd path"));
        }
        absolute = core::fs::Join(std::string(buf), path);
    }

    // `.` is dropped and `..` pops, with no filesystem access. An absolute
    // path cannot pop past its root, so a leading `..` is discarded rather
    // than preserved.
    std::vector<std::string> stack;
    std::size_t cursor = 0;
    while (cursor <= absolute.size()) {
        const std::size_t slash = absolute.find('/', cursor);
        const std::string component = absolute.substr(
            cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
        if (!component.empty() && component != ".") {
            if (component == "..") {
                if (!stack.empty()) stack.pop_back();
            } else {
                stack.push_back(component);
            }
        }
        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }

    std::string out;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        out += "/";
        out += stack[i];
    }
    // An empty stack is the root, which still has to render as `/`.
    if (out.empty()) out = "/";
    return out;
}

/// Splits a normalized absolute path into its components.
std::vector<std::string> Components(const std::string& path) {
    std::vector<std::string> out;
    std::size_t cursor = 0;
    while (cursor <= path.size()) {
        const std::size_t slash = path.find('/', cursor);
        const std::string component = path.substr(
            cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
        if (!component.empty()) out.push_back(component);
        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }
    return out;
}

}  // namespace

core::Expected<std::string, std::string> ResolveConfigPath(const std::string& base,
                                                            const std::string& raw) {
    if (!raw.empty() && raw[0] == '/') return NormalizePath(raw);
    return NormalizePath(core::fs::Join(base, raw));
}

core::Expected<std::string, std::string> RelativePath(const std::string& from_dir,
                                                        const std::string& to_path) {
    const core::Expected<std::string, std::string> from = NormalizePath(from_dir);
    if (!from.ok()) return core::make_unexpected(from.error());
    const core::Expected<std::string, std::string> to = NormalizePath(to_path);
    if (!to.ok()) return core::make_unexpected(to.error());

    const std::vector<std::string> from_parts = Components(from.value());
    const std::vector<std::string> to_parts = Components(to.value());

    std::size_t shared = 0;
    while (shared < from_parts.size() && shared < to_parts.size() &&
           from_parts[shared] == to_parts[shared]) {
        ++shared;
    }

    std::string relative;
    for (std::size_t i = shared; i < from_parts.size(); ++i) {
        if (!relative.empty()) relative += "/";
        relative += "..";
    }
    for (std::size_t i = shared; i < to_parts.size(); ++i) {
        if (!relative.empty()) relative += "/";
        relative += to_parts[i];
    }

    // `.` rather than the empty string when the two paths are the same. An
    // empty path is not usable, and only reads as "here" by accident.
    if (relative.empty()) relative = ".";
    return relative;
}

namespace {

/// Rust `rewrite_relative_path` — one field of one lower.
core::Expected<core::Json, std::string> RewriteOne(const core::Json& value,
                                                     const std::string& source_base,
                                                     const std::string& runtime_dir) {
    if (value.kind() != core::Json::Kind::String) return value;
    const std::string raw = value.as_string();

    // Left alone, and the two reasons differ. An empty value means the field
    // is unset, not that it points at the config's own directory. A `://`
    // means it is a URL, and rewriting that as a path would destroy the only
    // way to fetch the layer.
    if (raw.empty() || raw.find("://") != std::string::npos) return value;

    const core::Expected<std::string, std::string> resolved =
        ResolveConfigPath(source_base, raw);
    if (!resolved.ok()) return core::make_unexpected(resolved.error());
    const core::Expected<std::string, std::string> relative =
        RelativePath(runtime_dir, resolved.value());
    if (!relative.ok()) return core::make_unexpected(relative.error());
    return core::Json(relative.value());
}

}  // namespace

core::Expected<core::Unit, std::string> RewriteOverlaybdLowerPaths(
    core::Json* image_config, const std::string& source_base,
    const std::string& runtime_dir) {
    if (image_config == 0) {
        return core::make_unexpected(std::string("rewrite lower paths: null config"));
    }
    if (image_config->kind() != core::Json::Kind::Object) return core::Unit();

    const core::JsonObject& root = image_config->as_object();
    const core::JsonObject::const_iterator lowers_field = root.find("lowers");
    // No `lowers` at all, or one that is not an array: nothing to rewrite.
    // Rust returns `Ok` here too -- a config without lowers is valid.
    if (lowers_field == root.end() ||
        lowers_field->second.kind() != core::Json::Kind::Array) {
        return core::Unit();
    }

    // Rebuilt rather than mutated in place: `core::Json` exposes array
    // elements only by const reference, and widening that for this one caller
    // would let every other caller mutate a shared document.
    const core::JsonArray& lowers = lowers_field->second.as_array();
    core::JsonArray rewritten;
    rewritten.reserve(lowers.size());

    for (std::size_t i = 0; i < lowers.size(); ++i) {
        if (lowers[i].kind() != core::Json::Kind::Object) {
            // Carried through untouched, matching Rust's `continue`. It is
            // malformed, but this function's job is paths -- failing here
            // would reject a config the loader can report on more precisely.
            rewritten.push_back(lowers[i]);
            continue;
        }

        core::JsonObject lower = lowers[i].as_object();
        for (std::size_t k = 0; k < 4; ++k) {
            const core::JsonObject::iterator field = lower.find(kLowerPathKeys[k]);
            if (field == lower.end()) continue;
            const core::Expected<core::Json, std::string> value =
                RewriteOne(field->second, source_base, runtime_dir);
            if (!value.ok()) return core::make_unexpected(value.error());
            field->second = value.value();
        }
        rewritten.push_back(core::Json(lower));
    }

    (*image_config)["lowers"] = core::Json(rewritten);
    return core::Unit();
}

core::Expected<core::Unit, std::string> PrepareRuntimeUpper(
    const std::string& data_path, const core::Optional<std::string>& index_path,
    uint64_t virtual_size, UpperMode mode) {
    // Rust `impl From<UpperMode> for RwLayout`.
    RwLayout layout = kRwLogStructured;
    switch (mode) {
        case UpperMode::Sparse:
            layout = kRwSparse;
            break;
        case UpperMode::LogStructured:
            layout = kRwLogStructured;
            break;
        case UpperMode::HybridLogStructured:
            layout = kRwHybridLogStructured;
            break;
    }

    const core::Expected<core::Unit, std::string> initialized =
        InitializeFileRwPaths(data_path, index_path, virtual_size, layout);
    if (!initialized.ok()) {
        // Rust's `with_context`, which names both paths. The index is the one
        // a caller is most likely to have got wrong, so `<none>` is spelled
        // out rather than omitted.
        return core::make_unexpected(
            std::string("initialize overlaybd upper failed: ") + data_path + " / " +
            (index_path.has_value() ? *index_path : std::string("<none>")) + ": " +
            initialized.error());
    }
    return core::Unit();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
