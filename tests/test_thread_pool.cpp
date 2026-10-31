/**
 * @file test_thread_pool.cpp
 * @brief Comprehensive unit tests for bgl::core::ThreadPool and ParallelFor scheduler.
 */
#include "core/ThreadPool.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

using namespace bgl::core;

TEST(ThreadPoolTest, ThreadPoolInitializationAndWorkerCount) {
    ThreadPool pool(4);
    EXPECT_NO_THROW({
        auto f = pool.Enqueue([]() { return 42; });
        EXPECT_EQ(f.get(), 42);
    });
}

TEST(ThreadPoolTest, GlobalSingletonInstance) {
    auto& pool = ThreadPool::Instance();
    auto f = pool.Enqueue([](int a, int b) { return a + b; }, 10, 25);
    EXPECT_EQ(f.get(), 35);
}

TEST(ThreadPoolTest, MultipleConcurrentAsyncTasks) {
    ThreadPool pool(4);
    constexpr size_t kNumTasks = 100;
    std::vector<std::future<size_t>> futures;
    futures.reserve(kNumTasks);

    for (size_t i = 0; i < kNumTasks; ++i) {
        futures.push_back(pool.Enqueue([i]() {
            return i * i;
        }));
    }

    for (size_t i = 0; i < kNumTasks; ++i) {
        EXPECT_EQ(futures[i].get(), i * i);
    }
}

TEST(ThreadPoolTest, ParallelForWorkPartitioning) {
    ThreadPool pool(4);
    constexpr size_t kSize = 10000;
    std::vector<int> data(kSize, 0);

    pool.ParallelFor(0, kSize, [&data](size_t i) {
        data[i] = static_cast<int>(i * 2 + 1);
    });

    for (size_t i = 0; i < kSize; ++i) {
        EXPECT_EQ(data[i], static_cast<int>(i * 2 + 1));
    }
}

TEST(ThreadPoolTest, ParallelForAtomicAccumulation) {
    ThreadPool pool(4);
    constexpr size_t kSize = 5000;
    std::atomic<int64_t> sum{0};

    pool.ParallelFor(0, kSize, [&sum](size_t i) {
        sum.fetch_add(static_cast<int64_t>(i), std::memory_order_relaxed);
    });

    const int64_t expected = (static_cast<int64_t>(kSize - 1) * kSize) / 2;
    EXPECT_EQ(sum.load(), expected);
}

TEST(ThreadPoolTest, ParallelForEmptyAndInvertedBounds) {
    ThreadPool pool(2);
    std::atomic<int> calls{0};

    pool.ParallelFor(0, 0, [&calls](size_t) { calls.fetch_add(1); });
    EXPECT_EQ(calls.load(), 0);

    pool.ParallelFor(100, 50, [&calls](size_t) { calls.fetch_add(1); });
    EXPECT_EQ(calls.load(), 0);
}

TEST(ThreadPoolTest, ReentrancyAndNestedStarvationGuard) {
    ThreadPool pool(2);
    std::atomic<int> innerSum{0};

    // ParallelFor invoked from inside ParallelFor (nested)
    pool.ParallelFor(0, 10, [&pool, &innerSum](size_t outer) {
        pool.ParallelFor(0, 5, [&innerSum, outer](size_t inner) {
            innerSum.fetch_add(static_cast<int>(outer * 10 + inner));
        });
    });

    EXPECT_GT(innerSum.load(), 0);
}

TEST(ThreadPoolTest, ExceptionPropagationThroughFuture) {
    ThreadPool pool(2);
    auto future = pool.Enqueue([]() -> int {
        throw std::runtime_error("Test exception in ThreadPool worker");
    });

    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST(ThreadPoolTest, HeavyWorkloadStressAndDistribution) {
    ThreadPool pool(4);
    constexpr size_t kTasks = 200;
    std::atomic<size_t> completed{0};
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    for (size_t i = 0; i < kTasks; ++i) {
        futures.push_back(pool.Enqueue([&completed]() {
            uint64_t v = 0;
            for (int k = 0; k < 500; ++k) v += k;
            (void)v;
            completed.fetch_add(1, std::memory_order_release);
        }));
    }

    for (auto& f : futures) {
        f.get();
    }
    EXPECT_EQ(completed.load(), kTasks);
}
