/**
 * @file Log.hpp
 * @brief Structured logging utilities with spdlog and scoped microsecond timers.
 */
#pragma once

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif
#include <spdlog/spdlog.h>
#include <chrono>
#include <string_view>
#include <string>

namespace bgl::core {

/**
 * @class ScopedTimer
 * @brief RAII timer that logs duration upon destruction in microseconds and milliseconds.
 */
class ScopedTimer {
public:
    explicit ScopedTimer(std::string_view name)
        : name_(name), start_(std::chrono::high_resolution_clock::now()) {}

    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        double durationMs = durationUs / 1000.0;
        spdlog::info("[TIMER] {} took {} us ({:.3f} ms)", name_, durationUs, durationMs);
    }

private:
    std::string name_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

/**
 * @brief Initialize logger configuration and set log level.
 * @param logLevel Log level string (e.g. "debug", "info", "warn", "error").
 */
void InitLogging(std::string_view logLevel = "info");

} // namespace bgl::core

#define BGL_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define BGL_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define BGL_LOG_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define BGL_LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define BGL_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define BGL_PROFILE_SCOPE(name) ::bgl::core::ScopedTimer _bgl_timer_##__LINE__(name)

