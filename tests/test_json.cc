// SPDX-License-Identifier: MIT
#include "microtest.h"
#include "agentenv/core/json.h"

using namespace agentenv::core;

MT_TEST(json_int_string) {
    Json j;
    j["a"] = Json(int64_t(1));
    j["b"] = Json("hello");
    std::string s = j.ToString();
    auto p = Json::Parse(s);
    MT_EXPECT_TRUE(p.ok());
    MT_EXPECT_EQ(p.value()["a"].as_int(), int64_t(1));
    MT_EXPECT_EQ(p.value()["b"].as_string(), std::string("hello"));
}

MT_TEST(json_array_bool_null) {
    JsonArray arr;
    arr.emplace_back(Json(true));
    arr.emplace_back(Json());   // null
    Json j(std::move(arr));
    std::string s = j.ToString();
    auto p = Json::Parse(s);
    MT_EXPECT_TRUE(p.ok());
    MT_EXPECT_EQ(static_cast<int>(p.value().as_array().size()), 2);
}

MT_TEST(json_escapes) {
    Json j("line1\nline2\t\"quoted\"");
    auto p = Json::Parse(j.ToString());
    MT_EXPECT_TRUE(p.ok());
    MT_EXPECT_EQ(p.value().as_string(), std::string("line1\nline2\t\"quoted\""));
}

// serde_json's escape table: short forms for \b \t \n \f \r, lowercase
// `\u00xx` for the remaining C0 controls, DEL untouched.
MT_TEST(json_quote_matches_serde_json_escape_table) {
    MT_EXPECT_EQ(JsonQuote(std::string("a\b\f\x01\x1f\x7f")),
                 std::string("\"a\\b\\f\\u0001\\u001f\x7f\""));
}

// Go's encoding/json writes `<`, `>`, `&` as `\u003c` etc.; surrogate pairs
// decode to one 4-byte UTF-8 sequence.
MT_TEST(json_parse_decodes_unicode_escapes) {
    auto p = Json::Parse("\"a\\u003cb\\u003e\\u0026 \\u00e9 \\ud83d\\ude00\"");
    MT_EXPECT_TRUE(p.ok());
    MT_EXPECT_EQ(p.value().as_string(), std::string("a<b>& \xc3\xa9 \xf0\x9f\x98\x80"));
    MT_EXPECT_TRUE(!Json::Parse("\"\\ud83d\"").ok());
    MT_EXPECT_TRUE(!Json::Parse("\"\\ude00\"").ok());
}

MT_TEST(json_duplicate_keys_keep_last_like_serde_json) {
    auto p = Json::Parse("{\"a\":1,\"a\":2}");
    MT_EXPECT_TRUE(p.ok());
    MT_EXPECT_EQ(p.value()["a"].as_int(), int64_t(2));
}

// ryu's layout, which serde_json uses for f64.
MT_TEST(json_float_formatting_matches_ryu) {
    MT_EXPECT_EQ(Json(1.0).ToString(), std::string("1.0"));
    MT_EXPECT_EQ(Json(0.1).ToString(), std::string("0.1"));
    MT_EXPECT_EQ(Json(-2.5).ToString(), std::string("-2.5"));
    MT_EXPECT_EQ(Json(123456.789).ToString(), std::string("123456.789"));
    MT_EXPECT_EQ(Json(1e21).ToString(), std::string("1e21"));
    MT_EXPECT_EQ(Json(1.5e-7).ToString(), std::string("1.5e-7"));
    MT_EXPECT_EQ(Json(0.0001).ToString(), std::string("0.0001"));
    MT_EXPECT_EQ(Json(1e16).ToString(), std::string("1e16"));
    MT_EXPECT_EQ(Json(1e15).ToString(), std::string("1000000000000000.0"));
    MT_EXPECT_EQ(Json(0.30000000000000004).ToString(), std::string("0.30000000000000004"));
}

// `serde_json::Value` is a value type: mutating a clone must not touch the
// original.
MT_TEST(json_copies_are_independent_values) {
    Json a;
    a["k"] = Json("v");
    a["list"].push_back(Json(int64_t(1)));
    Json b = a;
    b["k"] = Json("changed");
    b["list"].push_back(Json(int64_t(2)));
    MT_EXPECT_EQ(a["k"].as_string(), std::string("v"));
    MT_EXPECT_EQ(static_cast<int>(a["list"].as_array().size()), 1);
    MT_EXPECT_TRUE(a != b);
    Json c = a;
    MT_EXPECT_TRUE(a == c);
    MT_EXPECT_TRUE(Json(int64_t(1)) != Json(1.0));
}

MT_TEST(json_pretty_matches_serde_json_pretty) {
    Json j;
    j["b"] = Json(JsonArray());
    j["a"].push_back(Json(int64_t(1)));
    j["c"] = Json(JsonObject());
    MT_EXPECT_EQ(j.ToStringPretty(),
                 std::string("{\n  \"a\": [\n    1\n  ],\n  \"b\": [],\n  \"c\": {}\n}"));
}

MT_MAIN
