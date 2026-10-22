/**
 * @file test_memory_pool.cpp
 * @brief Unit tests for bgl::core::MemoryPool and ScratchArena.
 */
#include <gtest/gtest.h>
#include "core/MemoryPool.hpp"

struct TestStruct {
    int a;
    float b;
    TestStruct(int x, float y) : a(x), b(y) {}
};

TEST(MemoryPoolTest, AllocationAndDeallocation) {
    bgl::core::MemoryPool<TestStruct, 16> pool;
    EXPECT_EQ(pool.AllocatedCount(), 0);

    TestStruct* p1 = pool.Allocate(42, 3.14f);
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(p1->a, 42);
    EXPECT_FLOAT_EQ(p1->b, 3.14f);
    EXPECT_EQ(pool.AllocatedCount(), 1);

    TestStruct* p2 = pool.Allocate(100, 2.71f);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(pool.AllocatedCount(), 2);

    pool.Deallocate(p1);
    EXPECT_EQ(pool.AllocatedCount(), 1);

    pool.Deallocate(p2);
    EXPECT_EQ(pool.AllocatedCount(), 0);
}

TEST(MemoryPoolTest, ScratchArenaReset) {
    bgl::core::ScratchArena arena(1024 * 64);
    auto* mr = arena.GetResource();
    ASSERT_NE(mr, nullptr);

    std::pmr::vector<int> vec(mr);
    for (int i = 0; i < 1000; ++i) {
        vec.push_back(i);
    }
    EXPECT_EQ(vec.size(), 1000);

    arena.Reset();
    mr = arena.GetResource();
    ASSERT_NE(mr, nullptr);
}

