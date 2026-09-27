// SPDX-License-Identifier: MIT
// A minimal JSON encoder/parser (RFC 8259) — enough for our DTOs, not compliant
// with all edge cases (e.g. Unicode escapes are decoded but string keys must be
// ASCII).
#include "agentenv/core/json.h"

#include <cctype>
#include <cstdio>
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

Json& Json::operator[](const std::string& k) {
    if (kind_ != Kind::Object) {
        kind_ = Kind::Object;
        o_ = std::make_shared<JsonObject>();
    }
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
    a_->emplace_back(std::move(v));
    return a_->back();
}

// ---------- serialization ----------
namespace {
void EscapeString(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
    os << '"';
}

void WriteJson(std::ostream& os, const Json& j) {
    switch (j.kind()) {
        case Json::Kind::Null:   os << "null"; return;
        case Json::Kind::Bool:   os << (j.as_bool() ? "true" : "false"); return;
        case Json::Kind::Int:    os << j.as_int(); return;
        case Json::Kind::Double: os << j.as_double(); return;
        case Json::Kind::String: EscapeString(os, j.as_string()); return;
        case Json::Kind::Array: {
            os << '[';
            bool first = true;
            for (const auto& e : j.as_array()) {
                if (!first) os << ',';
                WriteJson(os, e); first = false;
            }
            os << ']';
            return;
        }
        case Json::Kind::Object: {
            os << '{';
            bool first = true;
            for (const auto& kv : j.as_object()) {
                if (!first) os << ',';
                EscapeString(os, kv.first);
                os << ':';
                WriteJson(os, kv.second);
                first = false;
            }
            os << '}';
            return;
        }
    }
}
}  // namespace

std::string Json::ToString() const {
    std::ostringstream oss;
    WriteJson(oss, *this);
    return oss.str();
}

// ---------- parser ----------
namespace {
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
            o.emplace(key.value().as_string(), val.value());
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
        if (s_[p_] == '-' || s_[p_] == '+') ++p_;
        while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
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
        } catch (const std::exception&) {
            return make_unexpected(err("json: bad number: " + tok));
        }
    }

    void SkipWs() {
        while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
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
