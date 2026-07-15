#include <gtest/gtest.h>
#include "caduvelox/util/ThreadLocalPool.hpp"

using namespace caduvelox;

namespace {

// Instrumented type to observe construction/destruction from the pool.
struct Tracked {
    static int live_count;
    static int dtor_count;
    int value;

    explicit Tracked(int v) : value(v) { ++live_count; }
    ~Tracked() {
        --live_count;
        ++dtor_count;
    }
};

int Tracked::live_count = 0;
int Tracked::dtor_count = 0;

class ThreadLocalPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        Tracked::live_count = 0;
        Tracked::dtor_count = 0;
    }
};

TEST_F(ThreadLocalPoolTest, AllocateAndDeallocateBasics) {
    ThreadLocalPool<Tracked> pool(4);
    EXPECT_EQ(pool.capacity(), 4u);
    EXPECT_EQ(pool.available(), 4u);

    Tracked* a = pool.allocate(1);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->value, 1);
    EXPECT_EQ(pool.allocated(), 1u);

    pool.deallocate(a);
    EXPECT_EQ(pool.allocated(), 0u);
    EXPECT_EQ(Tracked::dtor_count, 1);
}

TEST_F(ThreadLocalPoolTest, GenerationInvalidatedByDeallocate) {
    ThreadLocalPool<Tracked> pool(4);

    Tracked* a = pool.allocate(1);
    ASSERT_NE(a, nullptr);
    uint64_t gen = pool.generationOf(a);
    EXPECT_TRUE(pool.isLive(a, gen));

    pool.deallocate(a);
    EXPECT_FALSE(pool.isLive(a, gen));
}

TEST_F(ThreadLocalPoolTest, GenerationDetectsRecycledSlot) {
    ThreadLocalPool<Tracked> pool(4);

    Tracked* a = pool.allocate(1);
    ASSERT_NE(a, nullptr);
    uint64_t stale_gen = pool.generationOf(a);
    pool.deallocate(a);

    // The free list is LIFO, so the next allocation reuses the same slot.
    Tracked* b = pool.allocate(2);
    ASSERT_EQ(static_cast<void*>(b), static_cast<void*>(a));

    // The stale (ptr, gen) pair must NOT validate against the recycled object,
    // while the fresh pair must.
    EXPECT_FALSE(pool.isLive(a, stale_gen));
    EXPECT_TRUE(pool.isLive(b, pool.generationOf(b)));

    pool.deallocate(b);
}

TEST_F(ThreadLocalPoolTest, IsLiveRejectsForeignPointer) {
    ThreadLocalPool<Tracked> pool(4);

    Tracked outside(42);
    EXPECT_FALSE(pool.isLive(&outside, 0));
    EXPECT_FALSE(pool.isLive(nullptr, 0));
}

TEST_F(ThreadLocalPoolTest, DestructorRunsForLiveObjects) {
    {
        ThreadLocalPool<Tracked> pool(4);
        pool.allocate(1);
        pool.allocate(2);
        Tracked* c = pool.allocate(3);
        pool.deallocate(c);

        EXPECT_EQ(Tracked::live_count, 2);
        EXPECT_EQ(Tracked::dtor_count, 1);
        // Two objects intentionally left live at pool destruction.
    }

    // Pool teardown must have destroyed the leaked objects.
    EXPECT_EQ(Tracked::live_count, 0);
    EXPECT_EQ(Tracked::dtor_count, 3);
}

TEST_F(ThreadLocalPoolTest, SweepLiveVisitsOnlyLiveObjects) {
    ThreadLocalPool<Tracked> pool(4);

    pool.allocate(1);
    Tracked* b = pool.allocate(2);
    pool.allocate(3);
    pool.deallocate(b);

    int sum = 0;
    int visited = 0;
    pool.sweepLive([&](Tracked& t) {
        sum += t.value;
        ++visited;
    });

    EXPECT_EQ(visited, 2);
    EXPECT_EQ(sum, 4);  // 1 + 3
}

} // namespace
