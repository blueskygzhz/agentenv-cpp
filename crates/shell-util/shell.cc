// SPDX-License-Identifier: MIT
#include "agentenv/shell-util/shell.h"

#include <cctype>

namespace agentenv {
namespace shellutil {
namespace {

// Mirror of Rust's char classification for the "safe" set. Rust uses
// `c.is_alphanumeric()` (Unicode) OR one of `- _ . / : @ + =`. This port
// treats the string as bytes; for the ASCII range the behavior is identical
// to the Rust implementation, and every upstream test case is ASCII.
bool IsSafeChar(unsigned char c) {
    if (std::isalnum(c)) {
        return true;
    }
    switch (c) {
        case '-':
        case '_':
        case '.':
        case '/':
        case ':':
        case '@':
        case '+':
        case '=':
            return true;
        default:
            return false;
    }
}

}  // namespace

std::string ShellQuote(const std::string& s) {
    if (s.empty()) {
        return "''";
    }

    bool safe = true;
    for (unsigned char c : s) {
        if (!IsSafeChar(c)) {
            safe = false;
            break;
        }
    }
    if (safe) {
        return s;
    }

    // Wrap in single quotes, escaping embedded single quotes as `'\''`.
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

}  // namespace shellutil
}  // namespace agentenv
