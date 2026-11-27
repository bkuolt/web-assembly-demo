/**
 * @file ThreadPool.hpp
 * @brief High-performance centralized thread pool and parallel-for job scheduler.
 */
#ifndef BGL_CORE_THREADPOOL_HPP
#define BGL_CORE_THREADPOOL_HPP

#include <algorithm>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace bgl::core {

/**
 * @class ThreadPool
 * @brief Centralized worker pool preventing thread oversubscription.
 */
class ThreadPool {
public:
    /**
     * @brief Access the global engine thread pool singleton.
     */
    static ThreadPool& Instance() noexcept;

    /**
     * @brief Construct thread pool with specified number of worker threads.
     * @param numThreads Thread count; defaults to std::thread::hardware_concurrency().
     */
    explicit ThreadPool(size_t numThreads = std::max(1u, std::thread::hardware_concurrency()));
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * @brief Enqueue a callable task returning a std::future.
     */
    template <typename F, typename... Args>
        requires std::invocable<F, Args...>
    auto Enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            [func = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
                return std::invoke(std::forward<decltype(func)>(func), std::forward<decltype(args)>(args)...);
            });

        std::future<ReturnType> future = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stop_.load(std::memory_order_relaxed)) {
                throw std::runtime_error("Cannot enqueue task on stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return future;
    }

    inline static thread_local bool t_isWorkerThread{false};
    inline static thread_local int t_parallelDepth{0};

    /**
     * @brief Pop and execute a single queued task from the pool if available.
     * @return True if a task was executed, false if the queue was empty.
     */
    bool TryExecuteOneTask();

    /**
     * @brief Execute a loop in parallel partitioned across workers with dynamic work-stealing, blocking until complete.
     * @param start Inclusive starting index.
     * @param end Exclusive ending index.
     * @param func Callable accepting loop index (size_t).
     * @param minGrainSize Minimum chunk size fetched per atomic steal (0 = auto-computed).
     */
    template <typename Func>
        requires std::invocable<Func, size_t>
    void ParallelFor(size_t start, size_t end, Func&& func, size_t minGrainSize = 0) {
        if (start >= end)
            return;
        const size_t totalWork = end - start;
        const size_t poolSize = workers_.size();
        if (totalWork == 1 || poolSize <= 1 || t_parallelDepth >= 2 || stop_.load(std::memory_order_acquire)) {
            for (size_t i = start; i < end; ++i) {
                func(i);
            }
            return;
        }

        const size_t grainSize =
            (minGrainSize > 0) ? minGrainSize : std::max<size_t>(1, totalWork / std::max<size_t>(1, poolSize * 4));
        const size_t numChunks = (totalWork + grainSize - 1) / grainSize;
        const size_t maxTasks = std::min(poolSize, numChunks);
        if (maxTasks <= 1) {
            for (size_t i = start; i < end; ++i) {
                func(i);
            }
            return;
        }

        struct ParallelState {
            std::atomic<size_t> nextOffset{0};
            std::atomic<size_t> activeWorkers{0};
            std::mutex doneMutex{};
            std::condition_variable doneCv{};
            std::mutex exceptionMutex{};
            std::exception_ptr firstException{};
        };

        struct DepthGuard {
            int& depth;
            explicit DepthGuard(int& d) noexcept : depth(d) { ++depth; }
            ~DepthGuard() noexcept { --depth; }
        } depthGuard{t_parallelDepth};

        auto state = std::make_shared<ParallelState>();
        const size_t workersToSpawn = maxTasks - 1;

        auto runWorkerLoop = [state, func, start, end, totalWork, grainSize]() {
            while (true) {
                const size_t offset = state->nextOffset.fetch_add(grainSize, std::memory_order_relaxed);
                if (offset >= totalWork) {
                    break;
                }
                const size_t chunkStart = start + offset;
                const size_t chunkEnd = std::min(chunkStart + grainSize, end);
                for (size_t i = chunkStart; i < chunkEnd; ++i) {
                    func(i);
                }
            }
        };

        std::vector<std::function<void()>> stagedTasks;
        bool prepOk = true;
        try {
            stagedTasks.reserve(workersToSpawn);
            for (size_t w = 0; w < workersToSpawn; ++w) {
                stagedTasks.emplace_back([state, runWorkerLoop]() {
                    DepthGuard workerDepth{t_parallelDepth};
                    try {
                        runWorkerLoop();
                    } catch (...) {
                        std::lock_guard<std::mutex> exLock(state->exceptionMutex);
                        if (!state->firstException) {
                            state->firstException = std::current_exception();
                        }
                    }
                    {
                        std::lock_guard<std::mutex> doneLock(state->doneMutex);
                        if (state->activeWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            state->doneCv.notify_all();
                        }
                    }
                });
            }
        } catch (...) {
            prepOk = false;
            stagedTasks.clear();
        }

        size_t pushedWorkers = 0;
        if (prepOk && !stagedTasks.empty()) {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (!stop_.load(std::memory_order_relaxed)) {
                state->activeWorkers.store(stagedTasks.size(), std::memory_order_release);
                try {
                    for (auto& t : stagedTasks) {
                        tasks_.emplace(std::move(t));
                        ++pushedWorkers;
                    }
                } catch (...) {
                    // Roll back count for unpushed workers before unlocking queueMutex_.
                }
                if (pushedWorkers < stagedTasks.size()) {
                    state->activeWorkers.fetch_sub(stagedTasks.size() - pushedWorkers, std::memory_order_acq_rel);
                }
            }
        }

        if (pushedWorkers == 0) {
            for (size_t i = start; i < end; ++i) {
                func(i);
            }
            return;
        }

        for (size_t w = 0; w < pushedWorkers; ++w) {
            cv_.notify_one();
        }

        {
            struct WorkerJoinGuard {
                std::shared_ptr<ParallelState> st;
                bool isWorker;
                ThreadPool* pool;
                ~WorkerJoinGuard() {
                    if (!st) {
                        return;
                    }
                    if (isWorker) {
                        while (true) {
                            {
                                std::unique_lock<std::mutex> doneLock(st->doneMutex);
                                if (st->activeWorkers.load(std::memory_order_acquire) == 0) {
                                    break;
                                }
                            }
                            if (!pool->TryExecuteOneTask()) {
                                std::unique_lock<std::mutex> doneLock(st->doneMutex);
                                st->doneCv.wait_for(doneLock, std::chrono::microseconds(100), [&]() {
                                    return st->activeWorkers.load(std::memory_order_acquire) == 0;
                                });
                            }
                        }
                    } else {
                        std::unique_lock<std::mutex> doneLock(st->doneMutex);
                        st->doneCv.wait(doneLock,
                                        [&]() { return st->activeWorkers.load(std::memory_order_acquire) == 0; });
                    }
                }
            } joinGuard{state, t_isWorkerThread, this};

            try {
                runWorkerLoop();
            } catch (...) {
                std::lock_guard<std::mutex> exLock(state->exceptionMutex);
                if (!state->firstException) {
                    state->firstException = std::current_exception();
                }
            }
        }

        if (state->firstException) {
            std::rethrow_exception(state->firstException);
        }
    }

    [[nodiscard]] size_t GetThreadCount() const noexcept { return workers_.size(); }
    void Shutdown() noexcept;

private:
    std::vector<std::thread> workers_{};
    std::queue<std::function<void()>> tasks_{};
    std::mutex queueMutex_{};
    std::condition_variable cv_{};
    std::atomic<bool> stop_{false};
};

} // namespace bgl::core

#endif // BGL_CORE_THREADPOOL_HPP
