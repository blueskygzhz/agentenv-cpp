// SPDX-License-Identifier: MIT
// Rust: `serde_json` — everywhere. C++11: hand-rolled minimal JSON value type
// (parsing / stringifying) so DTOs have zero deps.
#ifndef AGENTENV_CORE_JSON_H_
#define AGENTENV_CORE_JSON_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"

namespace agentenv {
namespace core {

class Json;
using JsonArray  = std::vector<Json>;
using JsonObject = std::map<std::string, Json>;

/// Recursive JSON value with `serde_json::Value` semantics. Containers are
/// shared between copies and cloned on the first mutation, so a copy behaves
/// as an independent value (as `Value::clone` does) while staying cheap to
/// pass around.
class Json {
 public:
    enum class Kind { Null, Bool, Int, Double, String, Array, Object };

    Json();
    Json(bool b);
    Json(int64_t i);
    Json(double d);
    Json(const char* s);
    Json(std::string s);
    Json(JsonArray a);
    Json(JsonObject o);

    Kind kind() const { return kind_; }

    // Accessors (undefined on wrong kind — check with kind()).
    bool         as_bool()   const;
    int64_t      as_int()    const;
    double       as_double() const;
    const std::string& as_string() const;
    const JsonArray&   as_array()  const;
    const JsonObject&  as_object() const;

    // Fluent object builders.
    Json& operator[](const std::string& k);              // object mutation
    const Json& operator[](const std::string& k) const;
    Json& push_back(Json v);                              // array mutation

    /// `serde_json::Value: PartialEq` — structural; numbers compare equal only
    /// when they have the same representation (an integer never equals a
    /// float), as serde's `Number` does.
    bool operator==(const Json& other) const;
    bool operator!=(const Json& other) const { return !(*this == other); }

    /// `serde_json::to_string` — compact, object keys in byte order (serde's
    /// default `BTreeMap`), floats in shortest round-trip form.
    std::string ToString() const;
    /// `serde_json::to_string_pretty` — two-space indent, `"key": value`.
    std::string ToStringPretty() const;
    static Expected<Json, AnyError> Parse(const std::string& text);

 private:
    void DetachArray();
    void DetachObject();

    Kind kind_;
    // Only one is populated per kind. In C++11 we don't have std::variant.
    bool          b_{false};
    int64_t       i_{0};
    double        d_{0.0};
    std::string   s_;
    std::shared_ptr<JsonArray>  a_;
    std::shared_ptr<JsonObject> o_;
};

/// The quoted, escaped form serde_json writes for a string: `"`, `\` and the
/// C0 controls are escaped, with the short forms `\b \t \n \f \r` and
/// lowercase `\u00xx` for the rest. Exposed for writers that must emit keys in
/// declaration order (serde's struct order), which `JsonObject` cannot hold.
std::string JsonQuote(const std::string& value);

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_JSON_H_
