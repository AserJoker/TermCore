#include <gtest/gtest.h>

#include <cstring>

#include <termcore/tc.h>

namespace {

constexpr size_t kAlign = alignof(max_align_t);

}  // namespace

TEST(Allocator, DefaultIsNeverNull) {
    const tc_allocator* a = tc_allocator_default();
    ASSERT_NE(a, nullptr);
    EXPECT_NE(a->alloc, nullptr);
    EXPECT_NE(a->free, nullptr);
    EXPECT_NE(a->realloc, nullptr);
}

TEST(Allocator, DefaultAllocatesUsableMemory) {
    const tc_allocator* a = tc_allocator_default();
    void* p = a->alloc(a->ctx, 64, kAlign);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, 64);
    a->free(a->ctx, p, 64);
}

TEST(Allocator, DefaultReallocKeepsContent) {
    const tc_allocator* a = tc_allocator_default();
    void* p = a->alloc(a->ctx, 8, kAlign);
    ASSERT_NE(p, nullptr);
    std::memset(p, 7, 8);

    void* q = a->realloc(a->ctx, p, 8, 256, kAlign);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(static_cast<unsigned char*>(q)[0], 7u);
    a->free(a->ctx, q, 256);
}

TEST(Allocator, CounterTracksLiveBytes) {
    tc_alloc_counter counter;
    tc_alloc_counter_init(&counter, nullptr);

    tc_allocator* a = &counter.allocator;
    void* p = a->alloc(a->ctx, 128, kAlign);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(tc_alloc_counter_live_bytes(&counter), 128u);
    EXPECT_FALSE(tc_alloc_counter_balanced(&counter));

    a->free(a->ctx, p, 128);
    EXPECT_EQ(tc_alloc_counter_live_bytes(&counter), 0u);
    EXPECT_TRUE(tc_alloc_counter_balanced(&counter));
}

TEST(Allocator, CounterTracksPeakAndRealloc) {
    tc_alloc_counter counter;
    tc_alloc_counter_init(&counter, nullptr);
    tc_allocator* a = &counter.allocator;

    void* p = a->alloc(a->ctx, 32, kAlign);
    ASSERT_NE(p, nullptr);
    void* q = a->realloc(a->ctx, p, 32, 1024, kAlign);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(tc_alloc_counter_live_bytes(&counter), 1024u);
    EXPECT_GE(counter.bytes_peak, 1024u);

    a->free(a->ctx, q, 1024);
    EXPECT_TRUE(tc_alloc_counter_balanced(&counter));
}

TEST(Allocator, CounterReportsUnbalancedWhenLeaked) {
    tc_alloc_counter counter;
    tc_alloc_counter_init(&counter, nullptr);
    void* leaked = counter.allocator.alloc(counter.allocator.ctx, 16, kAlign);
    ASSERT_NE(leaked, nullptr);
    EXPECT_FALSE(tc_alloc_counter_balanced(&counter));

    counter.allocator.free(counter.allocator.ctx, leaked, 16);
    EXPECT_TRUE(tc_alloc_counter_balanced(&counter));
}
