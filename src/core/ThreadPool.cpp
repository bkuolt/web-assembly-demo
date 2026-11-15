/**
 * @file ThreadPool.cpp
 * @brief Implementation of centralized ThreadPool worker loop.
 */
#include "core/ThreadPool.hpp"

namespace bgl::core {

ThreadPool& ThreadPool::Instance() noexcept {
    static ThreadPool instance;
    return instance;
}

ThreadPool::ThreadPool(size_t numThreads) {
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this]() {
            t_isWorkerThread = true;
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queueMutex_);
                    cv_.wait(lock, [this]() { return stop_.load(std::memory_order_relaxed) || !tasks_.empty(); });
                    if (stop_.load(std::memory_order_relaxed) && tasks_.empty()) {
                        return;
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    Shutdown();
}

void ThreadPool::Shutdown() noexcept {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        if (stop_.load(std::memory_order_relaxed)) {
            return;
        }
        stop_.store(true, std::memory_order_relaxed);
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace bgl::core
