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

/// Recursive JSON value. Small — moves are cheap because containers are heap.
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

    std::string ToString() const;
    static Expected<Json, AnyError> Parse(const std::string& text);

 private:
    Kind kind_;
    // Only one is populated per kind. In C++11 we don't have std::variant.
    bool          b_{false};
    int64_t       i_{0};
    double        d_{0.0};
    std::string   s_;
    std::shared_ptr<JsonArray>  a_;
    std::shared_ptr<JsonObject> o_;
};

}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_JSON_H_
