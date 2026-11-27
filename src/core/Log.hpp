/**
 * @file Log.hpp
 * @brief Structured logging utilities with spdlog and scoped microsecond timers.
 */
#ifndef BGL_CORE_LOG_HPP
#define BGL_CORE_LOG_HPP

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif
#include <chrono>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace bgl::core {

/**
 * @class ScopedTimer
 * @brief RAII timer that logs duration upon destruction in microseconds and milliseconds.
 */
class ScopedTimer {
public:
    explicit ScopedTimer(std::string_view name) : name_(name), start_(std::chrono::high_resolution_clock::now()) {}

    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        double durationMs = static_cast<double>(durationUs) / 1000.0;
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

/**
 * @brief Escape ASCII control characters (CR/LF/ESC/NUL) and truncate untrusted strings before logging (CWE-117).
 * @param input Raw string such as a dropped URI or parsed glTF node name.
 * @return Sanitized printable ASCII/UTF-8 string safe for structured single-line logs.
 */
[[nodiscard]] std::string SanitizeForLog(std::string_view input);

} // namespace bgl::core

#define BGL_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define BGL_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define BGL_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define BGL_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define BGL_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define BGL_CONCAT_INNER(a, b) a##b
#define BGL_CONCAT(a, b) BGL_CONCAT_INNER(a, b)
#define BGL_PROFILE_SCOPE(name) ::bgl::core::ScopedTimer BGL_CONCAT(_bgl_timer_, __LINE__)(name)

#endif // BGL_CORE_LOG_HPP
