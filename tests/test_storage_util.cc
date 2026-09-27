// SPDX-License-Identifier: MIT
// Tests for storage::util — AlignedBuffer, ReloadableIDAllocator, CompactWriter.
#include "microtest.h"

#include "agentenv/storage/util/aligned_buffer.h"
#include "agentenv/storage/util/id_allocator.h"
#include "agentenv/storage/util/compact_writer.h"

using namespace agentenv::storage::util;

MT_TEST(aligned_buffer_new_and_align) {
    for (size_t align : {size_t(512), size_t(4096)}) {
        auto r = AlignedBuffer::New(align, align);
        MT_EXPECT_TRUE(r.ok());
        AlignedBuffer buf = std::move(r.value());
        MT_EXPECT_EQ(buf.Len(), align);
        uintptr_t p = reinterpret_cast<uintptr_t>(buf.Data());
        MT_EXPECT_EQ(static_cast<int>(p % align), 0);
    }
}

MT_TEST(aligned_buffer_zero_rejected) {
    auto r = AlignedBuffer::New(0, 512);
    MT_EXPECT_TRUE(!r.ok());
}

MT_TEST(aligned_buffer_sub_range) {
    auto r = AlignedBuffer::New(1024, 512);
    MT_EXPECT_TRUE(r.ok());
    AlignedBuffer buf = std::move(r.value());
    for (size_t i = 0; i < 512; ++i) buf.Data()[i] = 0x11;
    for (size_t i = 512; i < 1024; ++i) buf.Data()[i] = 0x22;
    auto sr = buf.IntoSubRange(256, 768);
    MT_EXPECT_TRUE(sr.ok());
    MT_EXPECT_EQ(buf.Len(), size_t(512));
    MT_EXPECT_EQ(static_cast<int>(buf.Data()[0]), 0x11);
    MT_EXPECT_EQ(static_cast<int>(buf.Data()[256]), 0x22);

    auto bad = buf.IntoSubRange(0, 4096);
    MT_EXPECT_TRUE(!bad.ok());
}

MT_TEST(id_allocator_sequential) {
    ReloadableIDAllocator a(10);
    for (uint32_t i = 0; i < 100; ++i) {
        MT_EXPECT_EQ(a.Allocate(), i + 10);
    }
}

MT_TEST(id_allocator_recycle) {
    ReloadableIDAllocator a(1);
    uint32_t ids[50];
    for (int i = 0; i < 50; ++i) ids[i] = a.Allocate();
    for (int i = 0; i < 50; ++i) a.Recycle(ids[i]);
    // Recycled ids must come back before fresh ones.
    for (int i = 0; i < 50; ++i) {
        uint32_t got = a.Allocate();
        MT_EXPECT_TRUE(got < 51u);
    }
}

MT_TEST(id_allocator_occupy_reload) {
    ReloadableIDAllocator a(1);
    a.OccupyIdx(120);
    a.OccupyIdx(33);
    a.OccupyIdx(88);
    MT_EXPECT_TRUE(a.IsOccupied(120));
    MT_EXPECT_TRUE(a.IsOccupied(33));
    MT_EXPECT_TRUE(a.IsFree(500));
    for (int i = 0; i < 200; ++i) {
        uint32_t id = a.Allocate();
        MT_EXPECT_TRUE(id != 120u && id != 33u && id != 88u);
    }
}

MT_TEST(compact_writer_le_encoding) {
    CompactBuffer buf;
    CompactWriter w(&buf);
    w.PutU32(0x01020304u);
    MT_EXPECT_EQ(static_cast<int>(buf[0]), 0x04);
    MT_EXPECT_EQ(static_cast<int>(buf[3]), 0x01);
    MT_EXPECT_EQ(static_cast<int>(buf.size()), 4);
}

MT_MAIN
