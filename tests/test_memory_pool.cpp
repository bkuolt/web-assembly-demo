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

TEST(MemoryPoolTest, MultiBlockChainExpansion) {
    // Pool with small block capacity of 4 elements
    bgl::core::MemoryPool<TestStruct, 4> pool;
    std::vector<TestStruct*> ptrs;

    // Allocate 10 elements (forces 3 blocks to be chained: 4 + 4 + 2)
    for (int i = 0; i < 10; ++i) {
        TestStruct* p = pool.Allocate(i, static_cast<float>(i * 1.5f));
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    EXPECT_EQ(pool.AllocatedCount(), 10);

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(ptrs[i]->a, i);
        EXPECT_FLOAT_EQ(ptrs[i]->b, static_cast<float>(i * 1.5f));
    }

    // Deallocate all in reverse order
    for (auto it = ptrs.rbegin(); it != ptrs.rend(); ++it) {
        pool.Deallocate(*it);
    }
    EXPECT_EQ(pool.AllocatedCount(), 0);
}

TEST(MemoryPoolTest, FreeListSlotReuse) {
    bgl::core::MemoryPool<TestStruct, 8> pool;

    TestStruct* p1 = pool.Allocate(1, 1.0f);
    TestStruct* p2 = pool.Allocate(2, 2.0f);
    TestStruct* p3 = pool.Allocate(3, 3.0f);
    EXPECT_EQ(pool.AllocatedCount(), 3);

    // Free the middle element
    pool.Deallocate(p2);
    EXPECT_EQ(pool.AllocatedCount(), 2);

    // Next allocation should reuse the freed slot
    TestStruct* p4 = pool.Allocate(4, 4.0f);
    EXPECT_EQ(pool.AllocatedCount(), 3);
    EXPECT_EQ(p4, p2); // Memory location reused

    pool.Deallocate(p1);
    pool.Deallocate(p3);
    pool.Deallocate(p4);
    EXPECT_EQ(pool.AllocatedCount(), 0);
}


