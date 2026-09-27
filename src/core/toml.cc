// SPDX-License-Identifier: MIT
// Rust: the `toml` crate subset consumed by `confique` in src/cfg.rs.
#include "agentenv/core/toml.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace agentenv {
namespace core {
namespace {

void TrimInPlace(std::string* s) {
    while (!s->empty() && (s->front() == ' ' || s->front() == '\t')) s->erase(s->begin());
    while (!s->empty() && (s->back() == ' ' || s->back() == '\t' || s->back() == '\r' ||
                           s->back() == '\n')) {
        s->pop_back();
    }
}

/// Strip a trailing comment, honouring `#` inside a basic string.
void StripComment(std::string* line) {
    bool in_string = false;
    for (std::size_t i = 0; i < line->size(); ++i) {
        const char c = (*line)[i];
        if (in_string) {
            if (c == '\\') {
                ++i;  // skip the escaped byte
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '#') {
            line->erase(i);
            return;
        }
    }
}

/// Is the value text a complete single-line array? Used to decide whether we
/// must keep consuming lines for a multi-line array.
bool BracketsBalanced(const std::string& text) {
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
        }
    }
    return depth <= 0;
}

Expected<std::string, std::string> UnescapeBasicString(const std::string& body) {
    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= body.size()) return make_unexpected(std::string("dangling escape in string"));
        const char esc = body[++i];
        switch (esc) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case '0':
                out.push_back('\0');
                break;
            default:
                return make_unexpected(std::string("unsupported escape \\") + esc);
        }
    }
    return out;
}

/// Find the index of the comma that separates two array elements at depth 0,
/// or npos. Quote- and nesting-aware.
std::size_t FindTopLevelComma(const std::string& text, std::size_t from) {
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = from; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
        } else if (c == ',' && depth == 0) {
            return i;
        }
    }
    return std::string::npos;
}

Expected<TomlValue, std::string> ParseValue(const std::string& raw);

Expected<TomlValue, std::string> ParseArray(const std::string& raw) {
    // raw includes the surrounding brackets.
    if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') {
        return make_unexpected(std::string("malformed array: ") + raw);
    }
    std::string inner = raw.substr(1, raw.size() - 2);
    TrimInPlace(&inner);

    std::vector<TomlValue> elements;
    std::size_t cursor = 0;
    while (cursor < inner.size()) {
        std::size_t comma = FindTopLevelComma(inner, cursor);
        std::string element = inner.substr(
            cursor, comma == std::string::npos ? std::string::npos : comma - cursor);
        TrimInPlace(&element);
        if (!element.empty()) {
            Expected<TomlValue, std::string> parsed = ParseValue(element);
            if (!parsed.has_value()) return make_unexpected(parsed.error());
            elements.push_back(parsed.value());
        }
        if (comma == std::string::npos) break;
        cursor = comma + 1;
    }
    return TomlValue::Array(elements);
}

/// One `key = value` pair lifted out of an inline table, with both sides still
/// unparsed.
struct InlineField {
    std::string key;
    std::string value_text;
};

/// Splits `{ a = "x", b = "y" }` into its fields.
///
/// TOML requires an inline table to fit on one line, so there is no
/// continuation handling here. Nested inline tables are rejected rather than
/// silently mis-parsed, since nothing in AgentENV's configs uses them.
Expected<std::vector<InlineField>, std::string> SplitInlineTable(const std::string& raw) {
    if (raw.size() < 2 || raw.front() != '{' || raw.back() != '}') {
        return make_unexpected(std::string("malformed inline table: ") + raw);
    }
    std::string inner = raw.substr(1, raw.size() - 2);
    TrimInPlace(&inner);

    std::vector<InlineField> fields;
    if (inner.empty()) return fields;  // `{}` is a valid empty inline table

    std::size_t cursor = 0;
    while (cursor < inner.size()) {
        const std::size_t comma = FindTopLevelComma(inner, cursor);
        std::string chunk = inner.substr(
            cursor, comma == std::string::npos ? std::string::npos : comma - cursor);
        TrimInPlace(&chunk);

        if (!chunk.empty()) {
            const std::size_t eq = chunk.find('=');
            if (eq == std::string::npos) {
                return make_unexpected(std::string("inline table field needs `key = value`: ") +
                                       chunk);
            }
            InlineField field;
            field.key = chunk.substr(0, eq);
            field.value_text = chunk.substr(eq + 1);
            TrimInPlace(&field.key);
            TrimInPlace(&field.value_text);

            if (field.key.size() >= 2 && field.key.front() == '"' && field.key.back() == '"') {
                field.key = field.key.substr(1, field.key.size() - 2);
            }
            if (field.key.empty()) {
                return make_unexpected(std::string("inline table field has an empty key"));
            }
            if (!field.value_text.empty() && field.value_text.front() == '{') {
                return make_unexpected(
                    std::string("nested inline tables are not supported: ") + chunk);
            }
            fields.push_back(field);
        }

        if (comma == std::string::npos) break;
        cursor = comma + 1;
    }
    return fields;
}

Expected<TomlValue, std::string> ParseValue(const std::string& raw) {
    if (raw.empty()) return make_unexpected(std::string("empty value"));

    if (raw.front() == '[') return ParseArray(raw);

    if (raw.front() == '"') {
        if (raw.size() < 2 || raw.back() != '"') {
            return make_unexpected(std::string("unterminated string: ") + raw);
        }
        Expected<std::string, std::string> body = UnescapeBasicString(raw.substr(1, raw.size() - 2));
        if (!body.has_value()) return make_unexpected(body.error());
        return TomlValue::String(body.value());
    }

    if (raw == "true") return TomlValue::Boolean(true);
    if (raw == "false") return TomlValue::Boolean(false);

    // Numbers: TOML allows `_` as a digit separator.
    std::string digits;
    digits.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '_') digits.push_back(raw[i]);
    }
    if (digits.empty()) return make_unexpected(std::string("empty numeric value"));

    const bool looks_float = digits.find('.') != std::string::npos ||
                             digits.find('e') != std::string::npos ||
                             digits.find('E') != std::string::npos;

    const char* begin = digits.c_str();
    char* end = nullptr;
    if (looks_float) {
        const double value = std::strtod(begin, &end);
        if (end != begin + digits.size()) {
            return make_unexpected(std::string("invalid float: ") + raw);
        }
        return TomlValue::Float(value);
    }
    const long long value = std::strtoll(begin, &end, 10);
    if (end != begin + digits.size()) {
        return make_unexpected(std::string("invalid integer: ") + raw);
    }
    return TomlValue::Integer(value);
}

}  // namespace

TomlValue TomlValue::String(std::string v) {
    TomlValue out;
    out.kind_ = Kind::String;
    out.string_ = std::move(v);
    return out;
}

TomlValue TomlValue::Integer(long long v) {
    TomlValue out;
    out.kind_ = Kind::Integer;
    out.integer_ = v;
    return out;
}

TomlValue TomlValue::Float(double v) {
    TomlValue out;
    out.kind_ = Kind::Float;
    out.float_ = v;
    return out;
}

TomlValue TomlValue::Boolean(bool v) {
    TomlValue out;
    out.kind_ = Kind::Boolean;
    out.boolean_ = v;
    return out;
}

TomlValue TomlValue::Array(std::vector<TomlValue> v) {
    TomlValue out;
    out.kind_ = Kind::Array;
    out.array_ = std::move(v);
    return out;
}

Expected<std::string, std::string> TomlValue::AsString() const {
    switch (kind_) {
        case Kind::String:
            return string_;
        case Kind::Integer: {
            std::ostringstream oss;
            oss << integer_;
            return oss.str();
        }
        case Kind::Boolean:
            return std::string(boolean_ ? "true" : "false");
        default:
            return make_unexpected(std::string("expected a string"));
    }
}

Expected<long long, std::string> TomlValue::AsInteger() const {
    if (kind_ == Kind::Integer) return integer_;
    if (kind_ == Kind::String) {
        const char* begin = string_.c_str();
        char* end = nullptr;
        const long long value = std::strtoll(begin, &end, 10);
        if (!string_.empty() && end == begin + string_.size()) return value;
    }
    return make_unexpected(std::string("expected an integer"));
}

Expected<double, std::string> TomlValue::AsFloat() const {
    if (kind_ == Kind::Float) return float_;
    if (kind_ == Kind::Integer) return static_cast<double>(integer_);
    if (kind_ == Kind::String) {
        const char* begin = string_.c_str();
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (!string_.empty() && end == begin + string_.size()) return value;
    }
    return make_unexpected(std::string("expected a float"));
}

Expected<bool, std::string> TomlValue::AsBoolean() const {
    if (kind_ == Kind::Boolean) return boolean_;
    if (kind_ == Kind::String) {
        if (string_ == "true") return true;
        if (string_ == "false") return false;
    }
    return make_unexpected(std::string("expected a boolean"));
}

Expected<std::vector<std::string>, std::string> TomlValue::AsStringArray() const {
    std::vector<std::string> out;
    if (kind_ == Kind::Array) {
        out.reserve(array_.size());
        for (std::size_t i = 0; i < array_.size(); ++i) {
            Expected<std::string, std::string> element = array_[i].AsString();
            if (!element.has_value()) return make_unexpected(element.error());
            out.push_back(element.value());
        }
        return out;
    }
    // A bare scalar is accepted as a one-element list, matching how operators
    // often write a single value where a list is allowed.
    Expected<std::string, std::string> single = AsString();
    if (!single.has_value()) return make_unexpected(std::string("expected an array of strings"));
    out.push_back(single.value());
    return out;
}

Expected<TomlTable, std::string> TomlTable::ParseString(const std::string& text) {
    TomlTable table;
    std::istringstream in(text);
    std::string line;
    std::string section;
    int lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;
        StripComment(&line);
        TrimInPlace(&line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.size() > 1 && line[1] == '[') {
                std::ostringstream oss;
                oss << "toml line " << lineno << ": array-of-tables is not supported";
                return make_unexpected(oss.str());
            }
            if (line.back() != ']') {
                std::ostringstream oss;
                oss << "toml line " << lineno << ": unterminated table header";
                return make_unexpected(oss.str());
            }
            section = line.substr(1, line.size() - 2);
            TrimInPlace(&section);
            if (section.empty()) {
                std::ostringstream oss;
                oss << "toml line " << lineno << ": empty table header";
                return make_unexpected(oss.str());
            }
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            std::ostringstream oss;
            oss << "toml line " << lineno << ": expected `key = value`";
            return make_unexpected(oss.str());
        }
        std::string key = line.substr(0, eq);
        std::string value_text = line.substr(eq + 1);
        TrimInPlace(&key);
        TrimInPlace(&value_text);
        // Quoted keys are accepted so `"a.b" = 1` behaves like the Rust reader.
        if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
            key = key.substr(1, key.size() - 2);
        }
        if (key.empty()) {
            std::ostringstream oss;
            oss << "toml line " << lineno << ": empty key";
            return make_unexpected(oss.str());
        }

        // Multi-line array: keep appending following lines until balanced.
        if (!value_text.empty() && value_text.front() == '[' && !BracketsBalanced(value_text)) {
            while (std::getline(in, line)) {
                ++lineno;
                StripComment(&line);
                TrimInPlace(&line);
                value_text += ' ';
                value_text += line;
                if (BracketsBalanced(value_text)) break;
            }
            if (!BracketsBalanced(value_text)) {
                std::ostringstream oss;
                oss << "toml line " << lineno << ": unterminated array";
                return make_unexpected(oss.str());
            }
        }

        const std::string dotted = section.empty() ? key : section + "." + key;

        // An inline table contributes one flattened entry per field, plus a
        // record of its own name so the caller can iterate it back (see
        // `InlineTableKeys`).
        if (!value_text.empty() && value_text.front() == '{') {
            const Expected<std::vector<InlineField>, std::string> fields =
                SplitInlineTable(value_text);
            if (!fields.has_value()) {
                std::ostringstream oss;
                oss << "toml line " << lineno << ": " << fields.error();
                return make_unexpected(oss.str());
            }
            for (std::size_t i = 0; i < fields.value().size(); ++i) {
                const InlineField& field = fields.value()[i];
                Expected<TomlValue, std::string> value = ParseValue(field.value_text);
                if (!value.has_value()) {
                    std::ostringstream oss;
                    oss << "toml line " << lineno << ": " << value.error();
                    return make_unexpected(oss.str());
                }
                table.entries_[dotted + "." + field.key] = value.value();
            }
            table.inline_tables_[section].push_back(key);
            continue;
        }

        Expected<TomlValue, std::string> parsed = ParseValue(value_text);
        if (!parsed.has_value()) {
            std::ostringstream oss;
            oss << "toml line " << lineno << ": " << parsed.error();
            return make_unexpected(oss.str());
        }

        table.entries_[dotted] = parsed.value();
    }

    return table;
}

Expected<TomlTable, std::string> TomlTable::ParseFile(const std::string& path) {
    std::ifstream in(path.c_str());
    if (!in.is_open()) {
        return make_unexpected(std::string("cannot open config file: ") + path);
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return ParseString(oss.str());
}

std::vector<std::string> TomlTable::InlineTableKeys(const std::string& prefix) const {
    std::map<std::string, std::vector<std::string> >::const_iterator it =
        inline_tables_.find(prefix);
    return it == inline_tables_.end() ? std::vector<std::string>() : it->second;
}

const TomlValue* TomlTable::Find(const std::string& dotted_key) const {
    std::map<std::string, TomlValue>::const_iterator it = entries_.find(dotted_key);
    return it == entries_.end() ? nullptr : &it->second;
}

bool TomlTable::HasTable(const std::string& prefix) const {
    const std::string needle = prefix + ".";
    for (std::map<std::string, TomlValue>::const_iterator it = entries_.begin();
         it != entries_.end(); ++it) {
        if (it->first.compare(0, needle.size(), needle) == 0) return true;
    }
    return false;
}

}  // namespace core
}  // namespace agentenv
