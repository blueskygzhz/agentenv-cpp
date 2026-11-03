// SPDX-License-Identifier: MIT
// Rust: `serde_json::Value` and its (de)serializer.
//
// The output has to be byte-identical to serde_json, not merely valid JSON:
// several documents are content-addressed by the sha256 of their serialized
// form (commit-index paths, published image descriptions), and the two
// implementations share those stores. Hence the exact escape table, the byte
// order of object keys, and the shortest-round-trip float format below.
#include "agentenv/core/json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace agentenv {
namespace core {

Json::Json() : kind_(Kind::Null) {}
Json::Json(bool b) : kind_(Kind::Bool), b_(b) {}
Json::Json(int64_t i) : kind_(Kind::Int), i_(i) {}
Json::Json(double d) : kind_(Kind::Double), d_(d) {}
Json::Json(const char* s) : kind_(Kind::String), s_(s ? s : "") {}
Json::Json(std::string s) : kind_(Kind::String), s_(std::move(s)) {}
Json::Json(JsonArray  a) : kind_(Kind::Array),  a_(std::make_shared<JsonArray>(std::move(a))) {}
Json::Json(JsonObject o) : kind_(Kind::Object), o_(std::make_shared<JsonObject>(std::move(o))) {}

bool               Json::as_bool()   const { return b_; }
int64_t            Json::as_int()    const { return i_; }
double             Json::as_double() const { return d_; }
const std::string& Json::as_string() const { return s_; }
const JsonArray&   Json::as_array()  const { return *a_; }
const JsonObject&  Json::as_object() const { return *o_; }

// Copy-on-write: a copied `Json` shares its container until one side
// mutates. Without the detach, `Json b = a; b["k"] = v;` would also change
// `a`, which no `serde_json::Value` user expects.
void Json::DetachArray() {
    if (!a_) {
        a_ = std::make_shared<JsonArray>();
    } else if (a_.use_count() > 1) {
        a_ = std::make_shared<JsonArray>(*a_);
    }
}

void Json::DetachObject() {
    if (!o_) {
        o_ = std::make_shared<JsonObject>();
    } else if (o_.use_count() > 1) {
        o_ = std::make_shared<JsonObject>(*o_);
    }
}

Json& Json::operator[](const std::string& k) {
    if (kind_ != Kind::Object) {
        kind_ = Kind::Object;
        o_ = std::make_shared<JsonObject>();
    }
    DetachObject();
    return (*o_)[k];
}

const Json& Json::operator[](const std::string& k) const {
    static const Json kNull;
    if (kind_ != Kind::Object) return kNull;
    auto it = o_->find(k);
    return it == o_->end() ? kNull : it->second;
}

Json& Json::push_back(Json v) {
    if (kind_ != Kind::Array) {
        kind_ = Kind::Array;
        a_ = std::make_shared<JsonArray>();
    }
    DetachArray();
    a_->emplace_back(std::move(v));
    return a_->back();
}

bool Json::operator==(const Json& other) const {
    if (kind_ != other.kind_) return false;
    switch (kind_) {
        case Kind::Null:   return true;
        case Kind::Bool:   return b_ == other.b_;
        case Kind::Int:    return i_ == other.i_;
        case Kind::Double: return d_ == other.d_;
        case Kind::String: return s_ == other.s_;
        case Kind::Array:  return a_ == other.a_ || *a_ == *other.a_;
        case Kind::Object: return o_ == other.o_ || *o_ == *other.o_;
    }
    return false;
}

// ---------- serialization ----------

std::string JsonQuote(const std::string& value) {
    // serde_json `ESCAPE` table: only `"`, `\` and bytes below 0x20 are
    // escaped; DEL and non-ASCII UTF-8 pass through untouched.
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() + 2);
    out += '"';
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\t': out += "\\t";  break;
            case '\n': out += "\\n";  break;
            case '\f': out += "\\f";  break;
            case '\r': out += "\\r";  break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 0xf];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

namespace {

/// serde_json formats `f64` through `ryu`: the shortest digit string that
/// round-trips, laid out by `ryu::pretty::format64`. Non-finite values have no
/// JSON form and serde_json writes them as `null`.
std::string FormatDouble(double value) {
    if (!std::isfinite(value)) return "null";
    if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";

    char buf[40];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*e", precision - 1, value);
        if (std::strtod(buf, 0) == value) break;
    }
    std::string text(buf);
    std::string out;
    if (text[0] == '-') {
        out += '-';
        text.erase(0, 1);
    }
    const std::size_t e = text.find('e');
    const int exponent = std::atoi(text.c_str() + e + 1);
    std::string digits;
    for (std::size_t i = 0; i < e; ++i) {
        if (text[i] != '.') digits += text[i];
    }
    while (digits.size() > 1 && digits[digits.size() - 1] == '0') {
        digits.erase(digits.size() - 1);
    }

    // value = digits * 10^k, and 10^(kk-1) <= |value| < 10^kk.
    const int length = static_cast<int>(digits.size());
    const int k = exponent - length + 1;
    const int kk = length + k;
    if (k >= 0 && kk <= 16) {
        out += digits;
        out.append(static_cast<std::size_t>(k), '0');
        out += ".0";
    } else if (kk > 0 && kk <= 16) {
        out += digits.substr(0, static_cast<std::size_t>(kk));
        out += '.';
        out += digits.substr(static_cast<std::size_t>(kk));
    } else if (kk > -5 && kk <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-kk), '0');
        out += digits;
    } else {
        out += digits[0];
        if (length > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += 'e';
        out += std::to_string(kk - 1);
    }
    return out;
}

void WriteIndent(std::string* out, int depth) {
    out->append(static_cast<std::size_t>(depth) * 2, ' ');
}

void WriteJson(std::string* out, const Json& j, bool pretty, int depth) {
    switch (j.kind()) {
        case Json::Kind::Null:   *out += "null"; return;
        case Json::Kind::Bool:   *out += j.as_bool() ? "true" : "false"; return;
        case Json::Kind::Int:    *out += std::to_string(j.as_int()); return;
        case Json::Kind::Double: *out += FormatDouble(j.as_double()); return;
        case Json::Kind::String: *out += JsonQuote(j.as_string()); return;
        case Json::Kind::Array: {
            const JsonArray& array = j.as_array();
            // serde_json's PrettyFormatter writes an empty container as `[]`,
            // with no inner newline.
            if (array.empty()) {
                *out += "[]";
                return;
            }
            *out += '[';
            for (std::size_t i = 0; i < array.size(); ++i) {
                if (i != 0) *out += ',';
                if (pretty) {
                    *out += '\n';
                    WriteIndent(out, depth + 1);
                }
                WriteJson(out, array[i], pretty, depth + 1);
            }
            if (pretty) {
                *out += '\n';
                WriteIndent(out, depth);
            }
            *out += ']';
            return;
        }
        case Json::Kind::Object: {
            const JsonObject& object = j.as_object();
            if (object.empty()) {
                *out += "{}";
                return;
            }
            *out += '{';
            bool first = true;
            for (JsonObject::const_iterator it = object.begin(); it != object.end(); ++it) {
                if (!first) *out += ',';
                first = false;
                if (pretty) {
                    *out += '\n';
                    WriteIndent(out, depth + 1);
                }
                *out += JsonQuote(it->first);
                *out += pretty ? ": " : ":";
                WriteJson(out, it->second, pretty, depth + 1);
            }
            if (pretty) {
                *out += '\n';
                WriteIndent(out, depth);
            }
            *out += '}';
            return;
        }
    }
}

}  // namespace

std::string Json::ToString() const {
    std::string out;
    WriteJson(&out, *this, false, 0);
    return out;
}

std::string Json::ToStringPretty() const {
    std::string out;
    WriteJson(&out, *this, true, 0);
    return out;
}

// ---------- parser ----------
namespace {

void AppendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
 public:
    explicit Parser(const std::string& s) : s_(s), p_(0) {}
    Expected<Json, AnyError> Parse() {
        SkipWs();
        auto r = ParseValue();
        if (!r.ok()) return r;
        SkipWs();
        if (p_ != s_.size())
            return make_unexpected(err("json: trailing garbage @ " + std::to_string(p_)));
        return r;
    }

 private:
    Expected<Json, AnyError> ParseValue() {
        SkipWs();
        if (p_ >= s_.size()) return make_unexpected(err("json: eof"));
        char c = s_[p_];
        if (c == '{') return ParseObject();
        if (c == '[') return ParseArray();
        if (c == '"') return ParseString();
        if (c == 't' || c == 'f') return ParseBool();
        if (c == 'n') return ParseNull();
        return ParseNumber();
    }

    Expected<Json, AnyError> ParseObject() {
        JsonObject o;
        ++p_; SkipWs();
        if (p_ < s_.size() && s_[p_] == '}') { ++p_; return Json(std::move(o)); }
        for (;;) {
            SkipWs();
            if (p_ >= s_.size() || s_[p_] != '"')
                return make_unexpected(err("json: expected string key"));
            auto key = ParseString();
            if (!key.ok()) return key;
            SkipWs();
            if (p_ >= s_.size() || s_[p_] != ':')
                return make_unexpected(err("json: expected ':'"));
            ++p_;
            auto val = ParseValue();
            if (!val.ok()) return val;
            // Last occurrence wins, as `serde_json::Map::insert` does.
            o[key.value().as_string()] = val.value();
            SkipWs();
            if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
            if (p_ < s_.size() && s_[p_] == '}') { ++p_; break; }
            return make_unexpected(err("json: expected ',' or '}'"));
        }
        return Json(std::move(o));
    }

    Expected<Json, AnyError> ParseArray() {
        JsonArray a;
        ++p_; SkipWs();
        if (p_ < s_.size() && s_[p_] == ']') { ++p_; return Json(std::move(a)); }
        for (;;) {
            auto v = ParseValue();
            if (!v.ok()) return v;
            a.emplace_back(v.value());
            SkipWs();
            if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
            if (p_ < s_.size() && s_[p_] == ']') { ++p_; break; }
            return make_unexpected(err("json: expected ',' or ']'"));
        }
        return Json(std::move(a));
    }

    bool ParseHex4(uint32_t* out) {
        if (p_ + 4 > s_.size()) return false;
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[p_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        *out = value;
        return true;
    }

    Expected<Json, AnyError> ParseString() {
        ++p_;  // skip "
        std::string out;
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c == '\\') {
                if (p_ >= s_.size()) return make_unexpected(err("json: bad escape"));
                char e = s_[p_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        // Go's encoding/json writes `<`, `>` and `&` as
                        // `\u003c` etc., so OCI configs built by Docker carry
                        // these routinely.
                        uint32_t cp = 0;
                        if (!ParseHex4(&cp)) return make_unexpected(err("json: bad \\u escape"));
                        if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return make_unexpected(err("json: lone trailing surrogate"));
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            uint32_t low = 0;
                            if (p_ + 2 > s_.size() || s_[p_] != '\\' || s_[p_ + 1] != 'u') {
                                return make_unexpected(err("json: lone leading surrogate"));
                            }
                            p_ += 2;
                            if (!ParseHex4(&low) || low < 0xDC00 || low > 0xDFFF) {
                                return make_unexpected(err("json: lone leading surrogate"));
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                        AppendUtf8(&out, cp);
                        break;
                    }
                    default: return make_unexpected(err(std::string("json: bad escape ") + e));
                }
            } else {
                out += c;
            }
        }
        if (p_ >= s_.size()) return make_unexpected(err("json: unterminated string"));
        ++p_;  // skip closing "
        return Json(std::move(out));
    }

    Expected<Json, AnyError> ParseBool() {
        if (s_.compare(p_, 4, "true") == 0)  { p_ += 4; return Json(true); }
        if (s_.compare(p_, 5, "false") == 0) { p_ += 5; return Json(false); }
        return make_unexpected(err("json: expected bool"));
    }
    Expected<Json, AnyError> ParseNull() {
        if (s_.compare(p_, 4, "null") == 0) { p_ += 4; return Json(); }
        return make_unexpected(err("json: expected null"));
    }
    Expected<Json, AnyError> ParseNumber() {
        size_t start = p_;
        bool is_float = false;
        if (s_[p_] == '-') ++p_;
        const size_t int_start = p_;
        while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
        if (p_ == int_start) return make_unexpected(err("json: bad number"));
        if (p_ < s_.size() && s_[p_] == '.') { is_float = true; ++p_;
            while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
        }
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) { is_float = true; ++p_;
            if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) ++p_;
            while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
        }
        std::string tok = s_.substr(start, p_ - start);
        try {
            if (is_float) return Json(std::stod(tok));
            return Json(static_cast<int64_t>(std::stoll(tok)));
        } catch (const std::out_of_range&) {
            // serde_json falls back to f64 for an integer outside i64/u64.
            return Json(std::strtod(tok.c_str(), 0));
        } catch (const std::exception&) {
            return make_unexpected(err("json: bad number: " + tok));
        }
    }

    // JSON whitespace only (RFC 8259 §2); `isspace` would also accept \v, \f.
    void SkipWs() {
        while (p_ < s_.size() &&
               (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) {
            ++p_;
        }
    }

    const std::string& s_;
    size_t p_;
};
}  // namespace

Expected<Json, AnyError> Json::Parse(const std::string& text) {
    Parser p(text);
    return p.Parse();
}

}  // namespace core
}  // namespace agentenv
