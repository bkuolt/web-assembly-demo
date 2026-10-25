/**
 * @file ThreadPool.hpp
 * @brief High-performance centralized thread pool and parallel-for job scheduler.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <future>
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

    /**
     * @brief Execute a loop in parallel partitioned across workers, blocking until complete.
     * @param start Inclusive starting index.
     * @param end Exclusive ending index.
     * @param func Callable accepting loop index or slice (size_t).
     */
    template <typename Func>
        requires std::invocable<Func, size_t>
    void ParallelFor(size_t start, size_t end, Func&& func) {
        if (start >= end)
            return;
        const size_t totalWork = end - start;
        const size_t numWorkers = std::min(totalWork, workers_.size());
        // Prevent thread starvation deadlock if already running on a worker thread
        if (t_isWorkerThread || numWorkers <= 1) {
            for (size_t i = start; i < end; ++i) {
                func(i);
            }
            return;
        }

        const size_t chunkSize = (totalWork + numWorkers - 1) / numWorkers;
        std::vector<std::future<void>> futures;
        futures.reserve(numWorkers);

        for (size_t w = 0; w < numWorkers; ++w) {
            const size_t chunkStart = start + w * chunkSize;
            const size_t chunkEnd = std::min(chunkStart + chunkSize, end);
            if (chunkStart >= end)
                break;

            futures.push_back(Enqueue([chunkStart, chunkEnd, &func]() {
                for (size_t i = chunkStart; i < chunkEnd; ++i) {
                    func(i);
                }
            }));
        }

        for (auto& f : futures) {
            while (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queueMutex_);
                    if (!tasks_.empty()) {
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                }
                if (task) {
                    task();
                } else {
                    std::this_thread::yield();
                }
            }
            f.get();
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
