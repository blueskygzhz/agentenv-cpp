// SPDX-License-Identifier: MIT
// Tests for the TOML subset reader backing `AppConfig`.
#include <string>

#include "agentenv/core/toml.h"
#include "microtest.h"

namespace {

using agentenv::core::TomlTable;
using agentenv::core::TomlValue;

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

MT_TEST(parses_scalars_and_comments) {
    const std::string text =
        "# leading comment\n"
        "name = \"aenv\"   # trailing comment\n"
        "count = 42\n"
        "signed = -7\n"
        "separated = 1_000_000\n"
        "ratio = 0.95\n"
        "flag = true\n"
        "off = false\n"
        "\n";
    agentenv::core::Expected<TomlTable, std::string> table = TomlTable::ParseString(text);
    MT_EXPECT_TRUE(table.has_value());

    MT_EXPECT_EQ(table.value().Find("name")->AsString().value(), std::string("aenv"));
    MT_EXPECT_EQ(table.value().Find("count")->AsInteger().value(), 42);
    MT_EXPECT_EQ(table.value().Find("signed")->AsInteger().value(), -7);
    MT_EXPECT_EQ(table.value().Find("separated")->AsInteger().value(), 1000000);
    MT_EXPECT_TRUE(table.value().Find("ratio")->AsFloat().value() > 0.94);
    MT_EXPECT_TRUE(table.value().Find("flag")->AsBoolean().value());
    MT_EXPECT_TRUE(!table.value().Find("off")->AsBoolean().value());
    MT_EXPECT_TRUE(table.value().Find("missing") == nullptr);
}

MT_TEST(flattens_nested_tables) {
    const std::string text =
        "[image]\n"
        "top = 1\n"
        "[image.cache.gc]\n"
        "enabled = true\n"
        "interval_secs = 1800\n";
    agentenv::core::Expected<TomlTable, std::string> table = TomlTable::ParseString(text);
    MT_EXPECT_TRUE(table.has_value());

    MT_EXPECT_EQ(table.value().Find("image.top")->AsInteger().value(), 1);
    MT_EXPECT_TRUE(table.value().Find("image.cache.gc.enabled")->AsBoolean().value());
    MT_EXPECT_EQ(table.value().Find("image.cache.gc.interval_secs")->AsInteger().value(), 1800);

    // HasTable distinguishes "table present" from "key present".
    MT_EXPECT_TRUE(table.value().HasTable("image.cache.gc"));
    MT_EXPECT_TRUE(table.value().HasTable("image"));
    MT_EXPECT_TRUE(!table.value().HasTable("network"));
}

MT_TEST(parses_single_and_multi_line_arrays) {
    const std::string text =
        "inline = [\"docker.io\", \"ghcr.io\"]\n"
        "multi = [\n"
        "  \"a\",\n"
        "  \"b\",   # comment inside\n"
        "  \"c\",\n"
        "]\n"
        "numbers = [1, 2, 3]\n"
        "empty = []\n";
    agentenv::core::Expected<TomlTable, std::string> table = TomlTable::ParseString(text);
    MT_EXPECT_TRUE(table.has_value());

    std::vector<std::string> inline_values =
        table.value().Find("inline")->AsStringArray().value();
    MT_EXPECT_EQ(inline_values.size(), 2u);
    MT_EXPECT_EQ(inline_values[1], std::string("ghcr.io"));

    std::vector<std::string> multi = table.value().Find("multi")->AsStringArray().value();
    MT_EXPECT_EQ(multi.size(), 3u);
    MT_EXPECT_EQ(multi[2], std::string("c"));

    MT_EXPECT_EQ(table.value().Find("numbers")->array().size(), 3u);
    MT_EXPECT_EQ(table.value().Find("empty")->array().size(), 0u);
}

MT_TEST(honours_escapes_and_hash_inside_strings) {
    const std::string text =
        "quoted = \"a\\\"b\"\n"
        "hashed = \"not#a#comment\"\n"
        "newline = \"a\\nb\"\n"
        "tabbed = \"a\\tb\"\n";
    agentenv::core::Expected<TomlTable, std::string> table = TomlTable::ParseString(text);
    MT_EXPECT_TRUE(table.has_value());

    MT_EXPECT_EQ(table.value().Find("quoted")->AsString().value(), std::string("a\"b"));
    MT_EXPECT_EQ(table.value().Find("hashed")->AsString().value(),
                 std::string("not#a#comment"));
    MT_EXPECT_EQ(table.value().Find("newline")->AsString().value(), std::string("a\nb"));
    MT_EXPECT_EQ(table.value().Find("tabbed")->AsString().value(), std::string("a\tb"));
}

MT_TEST(reports_line_numbers_on_error) {
    {
        agentenv::core::Expected<TomlTable, std::string> table =
            TomlTable::ParseString("ok = 1\nbroken\n");
        MT_EXPECT_TRUE(!table.has_value());
        MT_EXPECT_TRUE(Contains(table.error(), "toml line 2"));
    }
    {
        agentenv::core::Expected<TomlTable, std::string> table =
            TomlTable::ParseString("[[array_of_tables]]\n");
        MT_EXPECT_TRUE(!table.has_value());
        MT_EXPECT_TRUE(Contains(table.error(), "array-of-tables is not supported"));
    }
    {
        agentenv::core::Expected<TomlTable, std::string> table =
            TomlTable::ParseString("bad = \"unterminated\n");
        MT_EXPECT_TRUE(!table.has_value());
    }
    {
        agentenv::core::Expected<TomlTable, std::string> table = TomlTable::ParseString("[\n");
        MT_EXPECT_TRUE(!table.has_value());
    }
}

MT_TEST(parse_file_reports_missing_path) {
    agentenv::core::Expected<TomlTable, std::string> table =
        TomlTable::ParseFile("/nonexistent/agentenv/toml");
    MT_EXPECT_TRUE(!table.has_value());
    MT_EXPECT_TRUE(Contains(table.error(), "cannot open config file"));
}

MT_TEST(scalar_accessors_coerce_where_toml_readers_do) {
    agentenv::core::Expected<TomlTable, std::string> table =
        TomlTable::ParseString("n = 5\ns = \"7\"\nb = true\n");
    MT_EXPECT_TRUE(table.has_value());

    // int -> float and int -> string are allowed.
    MT_EXPECT_TRUE(table.value().Find("n")->AsFloat().value() > 4.9);
    MT_EXPECT_EQ(table.value().Find("n")->AsString().value(), std::string("5"));
    // numeric strings parse back to integers.
    MT_EXPECT_EQ(table.value().Find("s")->AsInteger().value(), 7);
    // a bare scalar is accepted as a one-element list.
    MT_EXPECT_EQ(table.value().Find("b")->AsStringArray().value().size(), 1u);
    // but a boolean is not an integer.
    MT_EXPECT_TRUE(!table.value().Find("b")->AsInteger().has_value());
}

MT_MAIN
