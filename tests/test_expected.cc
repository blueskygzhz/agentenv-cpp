// SPDX-License-Identifier: MIT
#include "microtest.h"
#include "agentenv/core/expected.h"

using namespace agentenv::core;

MT_TEST(expected_holds_value) {
    Expected<int, std::string> e(42);
    MT_EXPECT_TRUE(e.ok());
    MT_EXPECT_EQ(e.value(), 42);
}

MT_TEST(expected_holds_error) {
    Expected<int, std::string> e(make_unexpected(std::string("oops")));
    MT_EXPECT_TRUE(!e.ok());
    MT_EXPECT_EQ(e.error(), std::string("oops"));
}

MT_TEST(expected_move) {
    Expected<std::string, int> a(std::string("hello"));
    Expected<std::string, int> b(std::move(a));
    MT_EXPECT_TRUE(b.ok());
    MT_EXPECT_EQ(b.value(), std::string("hello"));
}

MT_MAIN
