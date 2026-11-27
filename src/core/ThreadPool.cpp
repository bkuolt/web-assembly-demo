/**
 * @file ThreadPool.cpp
 * @brief Implementation of centralized ThreadPool worker loop.
 */
#include "core/ThreadPool.hpp"
#include "core/Log.hpp"

namespace bgl::core {

namespace {

void ExecuteTaskSafely(const std::function<void()>& task) noexcept {
    if (!task) {
        return;
    }
    try {
        task();
    } catch (const std::exception& ex) {
        BGL_LOG_ERROR("ThreadPool task threw unhandled exception: {}", ex.what());
    } catch (...) {
        BGL_LOG_ERROR("ThreadPool task threw unknown unhandled exception");
    }
}

} // namespace

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
                ExecuteTaskSafely(task);
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    Shutdown();
}

bool ThreadPool::TryExecuteOneTask() {
    std::function<void()> task;
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        if (tasks_.empty()) {
            return false;
        }
        task = std::move(tasks_.front());
        tasks_.pop();
    }
    if (task) {
        ExecuteTaskSafely(task);
        return true;
    }
    return false;
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
