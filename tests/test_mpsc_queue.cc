// SPDX-License-Identifier: MIT
#include "microtest.h"
#include "agentenv/core/mpsc_queue.h"

#include <thread>

using namespace agentenv::core;

MT_TEST(mpsc_push_pop) {
    MpscQueue<int> q(4);
    MT_EXPECT_TRUE(q.TryPush(1));
    MT_EXPECT_TRUE(q.TryPush(2));
    int v = 0;
    MT_EXPECT_TRUE(q.Pop(&v)); MT_EXPECT_EQ(v, 1);
    MT_EXPECT_TRUE(q.Pop(&v)); MT_EXPECT_EQ(v, 2);
}

MT_TEST(mpsc_close_drains) {
    MpscQueue<int> q;
    q.Push(42);
    q.Close();
    int v = 0;
    MT_EXPECT_TRUE(q.Pop(&v)); MT_EXPECT_EQ(v, 42);
    MT_EXPECT_TRUE(!q.Pop(&v));
}

MT_MAIN
