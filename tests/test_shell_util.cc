// SPDX-License-Identifier: MIT
// Ported from crates/shell-util/src/lib.rs `mod tests`.
#include "agentenv/shell-util/shell.h"

#include "microtest.h"

using agentenv::shellutil::ShellQuote;

MT_TEST(safe_chars_pass_through) {
    MT_EXPECT_EQ(ShellQuote("nginx"), std::string("nginx"));
    MT_EXPECT_EQ(ShellQuote("/bin/sh"), std::string("/bin/sh"));
    MT_EXPECT_EQ(ShellQuote("node:20"), std::string("node:20"));
}

MT_TEST(wraps_special_chars) {
    MT_EXPECT_EQ(ShellQuote("daemon off;"), std::string("'daemon off;'"));
    MT_EXPECT_EQ(ShellQuote("$HOME"), std::string("'$HOME'"));
    MT_EXPECT_EQ(ShellQuote(""), std::string("''"));
}

MT_TEST(escapes_embedded_single_quotes) {
    MT_EXPECT_EQ(ShellQuote("it's"), std::string("'it'\\''s'"));
    // Three consecutive single quotes: each ' becomes '\'' .
    MT_EXPECT_EQ(ShellQuote("'''"), std::string("''\\'''\\'''\\'''"));
}

MT_TEST(prevents_variable_expansion) {
    MT_EXPECT_EQ(ShellQuote("$HOME"), std::string("'$HOME'"));
    MT_EXPECT_EQ(ShellQuote("${VAR:-default}"), std::string("'${VAR:-default}'"));
}

MT_TEST(backslash_is_literal_inside_single_quotes) {
    MT_EXPECT_EQ(ShellQuote("a\\b"), std::string("'a\\b'"));
}

MT_MAIN
