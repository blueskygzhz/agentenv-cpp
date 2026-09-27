// SPDX-License-Identifier: MIT
// Rust: the `toml` crate as consumed by `confique` in src/cfg.rs.
//
// The pre-existing `core::Config` loader only understood flat
// `key = "string" | int | bool` lines, which cannot express `AppConfig`:
// `src/cfg.rs` needs nested tables (`[image.cache.gc]`), string arrays
// (`search_registries`, `always_denied_cidrs`) and floats
// (`high_watermark_ratio`). This is a self-contained TOML *subset* reader that
// covers exactly what AgentENV's config files use.
//
// Supported:
//   - comments (`#`), blank lines, CRLF
//   - `[table]` and `[nested.table]` headers (flattened to dotted keys)
//   - dotted keys inside a table (`a.b = 1`)
//   - values: basic strings (with \" \\ \n \r \t \0 escapes), integers
//     (decimal, `_` separators, optional sign), floats, `true`/`false`
//   - single-line arrays of the above, plus multi-line arrays
//   - single-line inline tables (`k = { a = "x", b = "y" }`), needed by
//     `config/deps_manifest.toml`'s `[packages.runtime_commands]`
//
// Not supported (unused by AgentENV configs): array-of-tables (`[[x]]`),
// nested inline tables, datetimes, literal/multi-line strings, hex/oct/bin
// ints.
#ifndef AGENTENV_CORE_TOML_H_
#define AGENTENV_CORE_TOML_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace core {

/// One scalar TOML value. Arrays keep their elements as `TomlValue`s so a
/// caller can ask for `AsStringArray()` without a second parse.
class TomlValue {
 public:
    enum class Kind { String, Integer, Float, Boolean, Array };

    TomlValue() : kind_(Kind::String) {}

    static TomlValue String(std::string v);
    static TomlValue Integer(long long v);
    static TomlValue Float(double v);
    static TomlValue Boolean(bool v);
    static TomlValue Array(std::vector<TomlValue> v);

    Kind kind() const { return kind_; }

    /// Scalar accessors. Each performs the small amount of coercion TOML
    /// readers conventionally allow (int -> float, int -> string, ...) so a
    /// config that writes `0` where a float is expected still loads.
    Expected<std::string, std::string> AsString() const;
    Expected<long long, std::string> AsInteger() const;
    Expected<double, std::string> AsFloat() const;
    Expected<bool, std::string> AsBoolean() const;
    Expected<std::vector<std::string>, std::string> AsStringArray() const;

    const std::vector<TomlValue>& array() const { return array_; }

 private:
    Kind kind_;
    std::string string_;
    long long integer_ = 0;
    double float_ = 0.0;
    bool boolean_ = false;
    std::vector<TomlValue> array_;
};

/// A parsed document, flattened to dotted keys: `[image.cache] root_dir = "x"`
/// is stored as `image.cache.root_dir`.
class TomlTable {
 public:
    /// Parse TOML text. Errors carry a 1-based line number.
    static Expected<TomlTable, std::string> ParseString(const std::string& text);
    /// Read and parse a file. Missing/unreadable files are an error.
    static Expected<TomlTable, std::string> ParseFile(const std::string& path);

    /// Look up a flattened dotted key.
    const TomlValue* Find(const std::string& dotted_key) const;
    bool Contains(const std::string& dotted_key) const { return Find(dotted_key) != nullptr; }

    /// True when any key starts with `prefix + '.'`. `src/cfg.rs` needs this to
    /// tell "table omitted" (Rust `Option<T>` stays `None`) from "table present
    /// but every field defaulted" (`Some(T::default())`).
    bool HasTable(const std::string& prefix) const;

    /// Names of the inline tables declared directly under `prefix`, in source
    /// order.
    ///
    /// This exists because an inline table's *name* can itself contain a dot:
    /// `"mkfs.ext4" = { default = "e2fsprogs" }` flattens to the key
    /// `...mkfs.ext4.default`, which is indistinguishable from a table `mkfs`
    /// holding `ext4.default`. Keeping the original names lets a caller iterate
    /// the entries unambiguously — it recombines `prefix + name + field` the
    /// same way the parser split it. Rust has no such problem because it
    /// deserializes straight into `BTreeMap<String, _>`.
    std::vector<std::string> InlineTableKeys(const std::string& prefix) const;

    const std::map<std::string, TomlValue>& entries() const { return entries_; }

 private:
    std::map<std::string, TomlValue> entries_;
    /// prefix -> inline table names declared under it.
    std::map<std::string, std::vector<std::string> > inline_tables_;
};

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_TOML_H_
