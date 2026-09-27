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

MT_MAIN
