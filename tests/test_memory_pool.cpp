/**
 * @file test_memory_pool.cpp
 * @brief Unit tests for bgl::core::MemoryPool and ScratchArena.
 */
#include "core/MemoryPool.hpp"
#include <gtest/gtest.h>

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

// Tracking object for destructor verification
struct LifetimeTracker {
    static inline int activeInstances = 0;
    int id;

    LifetimeTracker(int val) : id(val) { ++activeInstances; }

    ~LifetimeTracker() { --activeInstances; }
};

TEST(MemoryPoolTest, DestructorInvocationTracking) {
    LifetimeTracker::activeInstances = 0;
    {
        bgl::core::MemoryPool<LifetimeTracker, 8> pool;
        EXPECT_EQ(LifetimeTracker::activeInstances, 0);

        LifetimeTracker* obj1 = pool.Allocate(101);
        LifetimeTracker* obj2 = pool.Allocate(102);
        EXPECT_EQ(LifetimeTracker::activeInstances, 2);
        EXPECT_EQ(pool.AllocatedCount(), 2);

        // Deallocating obj1 must invoke ~LifetimeTracker()
        pool.Deallocate(obj1);
        EXPECT_EQ(LifetimeTracker::activeInstances, 1);
        EXPECT_EQ(pool.AllocatedCount(), 1);

        pool.Deallocate(obj2);
        EXPECT_EQ(LifetimeTracker::activeInstances, 0);
        EXPECT_EQ(pool.AllocatedCount(), 0);
    }
    EXPECT_EQ(LifetimeTracker::activeInstances, 0);
}

TEST(MemoryPoolTest, MoveSemanticsTransfer) {
    bgl::core::MemoryPool<TestStruct, 4> sourcePool;
    TestStruct* s1 = sourcePool.Allocate(10, 1.0f);
    TestStruct* s2 = sourcePool.Allocate(20, 2.0f);
    EXPECT_EQ(sourcePool.AllocatedCount(), 2);
    EXPECT_EQ(sourcePool.BlockCount(), 1);

    // Move constructor
    bgl::core::MemoryPool<TestStruct, 4> destPool(std::move(sourcePool));
    EXPECT_EQ(destPool.AllocatedCount(), 2);
    EXPECT_EQ(destPool.BlockCount(), 1);
    EXPECT_EQ(sourcePool.AllocatedCount(), 0);
    EXPECT_EQ(sourcePool.BlockCount(), 0);

    // Objects must still be valid in destPool
    EXPECT_EQ(s1->a, 10);
    EXPECT_EQ(s2->a, 20);

    // Allocating in destPool continues seamlessly
    TestStruct* s3 = destPool.Allocate(30, 3.0f);
    EXPECT_EQ(destPool.AllocatedCount(), 3);

    // Move assignment
    bgl::core::MemoryPool<TestStruct, 4> assignedPool;
    assignedPool = std::move(destPool);
    EXPECT_EQ(assignedPool.AllocatedCount(), 3);
    EXPECT_EQ(destPool.AllocatedCount(), 0);

    assignedPool.Deallocate(s1);
    assignedPool.Deallocate(s2);
    assignedPool.Deallocate(s3);
    EXPECT_EQ(assignedPool.AllocatedCount(), 0);
}

TEST(MemoryPoolTest, NullptrDeallocationSafety) {
    bgl::core::MemoryPool<TestStruct, 8> pool;
    // Calling Deallocate(nullptr) must not crash or modify allocated count
    pool.Deallocate(nullptr);
    EXPECT_EQ(pool.AllocatedCount(), 0);

    TestStruct* item = pool.Allocate(5, 5.0f);
    EXPECT_EQ(pool.AllocatedCount(), 1);

    pool.Deallocate(nullptr);
    EXPECT_EQ(pool.AllocatedCount(), 1);

    pool.Deallocate(item);
    EXPECT_EQ(pool.AllocatedCount(), 0);
}

TEST(MemoryPoolTest, ClearReleasesAllBlocks) {
    bgl::core::MemoryPool<TestStruct, 4> pool;
    for (int i = 0; i < 12; ++i) {
        pool.Allocate(i, static_cast<float>(i));
    }
    EXPECT_EQ(pool.AllocatedCount(), 12);
    EXPECT_EQ(pool.BlockCount(), 3);

    pool.Clear();
    EXPECT_EQ(pool.AllocatedCount(), 0);
    EXPECT_EQ(pool.BlockCount(), 0);

    // Allocations after Clear must function identically from fresh block
    TestStruct* fresh = pool.Allocate(99, 99.0f);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(fresh->a, 99);
    EXPECT_EQ(pool.AllocatedCount(), 1);
    EXPECT_EQ(pool.BlockCount(), 1);

    pool.Deallocate(fresh);
    EXPECT_EQ(pool.AllocatedCount(), 0);
}

TEST(MemoryPoolTest, LargeScaleStressAndInterleavedDeallocation) {
    constexpr size_t kTotalAllocations = 2000;
    bgl::core::MemoryPool<TestStruct, 64> pool;
    std::vector<TestStruct*> ptrs;
    ptrs.reserve(kTotalAllocations);

    for (size_t i = 0; i < kTotalAllocations; ++i) {
        ptrs.push_back(pool.Allocate(static_cast<int>(i), static_cast<float>(i * 0.1f)));
    }
    EXPECT_EQ(pool.AllocatedCount(), kTotalAllocations);

    // Deallocate all odd indices
    for (size_t i = 1; i < kTotalAllocations; i += 2) {
        pool.Deallocate(ptrs[i]);
        ptrs[i] = nullptr;
    }
    EXPECT_EQ(pool.AllocatedCount(), kTotalAllocations / 2);

    // Reallocate 500 items into the free-list
    std::vector<TestStruct*> newPtrs;
    for (size_t i = 0; i < 500; ++i) {
        newPtrs.push_back(pool.Allocate(999, 9.99f));
    }
    EXPECT_EQ(pool.AllocatedCount(), (kTotalAllocations / 2) + 500);

    // Cleanup remaining original even elements and new elements
    for (size_t i = 0; i < kTotalAllocations; i += 2) {
        if (ptrs[i])
            pool.Deallocate(ptrs[i]);
    }
    for (auto* p : newPtrs) {
        pool.Deallocate(p);
    }
    EXPECT_EQ(pool.AllocatedCount(), 0);
}

TEST(MemoryPoolTest, AlignedTypeAllocation) {
    struct alignas(64) HeavilyAligned {
        float values[16]; // 64 bytes
    };

    bgl::core::MemoryPool<HeavilyAligned, 4> alignedPool;
    for (int i = 0; i < 8; ++i) {
        HeavilyAligned* obj = alignedPool.Allocate();
        ASSERT_NE(obj, nullptr);
        // Verify 64-byte alignment guarantee
        EXPECT_EQ(reinterpret_cast<uintptr_t>(obj) % 64, 0u);
        alignedPool.Deallocate(obj);
    }
    EXPECT_EQ(alignedPool.AllocatedCount(), 0);
}

TEST(MemoryPoolTest, ScratchArenaRepeatedResetUnderPMRVectors) {
    bgl::core::ScratchArena arena(1024 * 128); // 128 KB
    auto* mr = arena.GetResource();
    ASSERT_NE(mr, nullptr);

    for (int frame = 0; frame < 10; ++frame) {
        {
            std::pmr::vector<int> numbers(mr);
            numbers.reserve(2000);
            for (int i = 0; i < 2000; ++i) {
                numbers.push_back(i * frame);
            }
            EXPECT_EQ(numbers.size(), 2000u);
            EXPECT_EQ(numbers[10], 10 * frame);
        }
        // Resets monotonic buffer pointer in O(1)
        arena.Reset();
    }
    EXPECT_EQ(arena.Capacity(), 1024 * 128);
}

TEST(MemoryPoolTest, NullptrDeallocateSafety) {
    bgl::core::MemoryPool<TestStruct, 8> pool;
    EXPECT_NO_THROW(pool.Deallocate(nullptr));
    EXPECT_EQ(pool.AllocatedCount(), 0);
}

TEST(MemoryPoolTest, RapidLIFOReuse) {
    bgl::core::MemoryPool<TestStruct, 4> pool;
    TestStruct* p1 = pool.Allocate(1, 1.0f);
    ASSERT_NE(p1, nullptr);
    const uintptr_t addr1 = reinterpret_cast<uintptr_t>(p1);

    pool.Deallocate(p1);
    EXPECT_EQ(pool.AllocatedCount(), 0);

    // Free list must return the recently deallocated slot (LIFO)
    TestStruct* p2 = pool.Allocate(2, 2.0f);
    ASSERT_NE(p2, nullptr);
    const uintptr_t addr2 = reinterpret_cast<uintptr_t>(p2);

    EXPECT_EQ(addr1, addr2);
    pool.Deallocate(p2);
}

TEST(MemoryPoolTest, ScratchArenaMultipleConcurrentPmrVectors) {
    bgl::core::ScratchArena arena(1024 * 64);
    auto* mr = arena.GetResource();

    std::pmr::vector<int> ints(mr);
    std::pmr::vector<float> floats(mr);
    std::pmr::vector<char> chars(mr);

    for (int i = 0; i < 100; ++i) {
        ints.push_back(i);
        floats.push_back(static_cast<float>(i) * 0.5f);
        chars.push_back(static_cast<char>('A' + (i % 26)));
    }

    EXPECT_EQ(ints.size(), 100u);
    EXPECT_EQ(floats.size(), 100u);
    EXPECT_EQ(chars.size(), 100u);

    arena.Reset();
}

TEST(MemoryPoolTest, ZeroInitializationVerification) {
    struct PodType {
        int x{0};
        int y{0};
        int z{0};
    };

    bgl::core::MemoryPool<PodType, 4> podPool;
    PodType* pod = podPool.Allocate();
    ASSERT_NE(pod, nullptr);
    EXPECT_EQ(pod->x, 0);
    EXPECT_EQ(pod->y, 0);
    EXPECT_EQ(pod->z, 0);
    podPool.Deallocate(pod);
}
