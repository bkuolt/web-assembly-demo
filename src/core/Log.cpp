/**
 * @file Log.cpp
 * @brief Implementation of logging initialisation for bgl::core.
 */
#include "core/Log.hpp"
#include <cstdio>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace bgl::core {

void InitLogging(std::string_view logLevel) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");
    if (logLevel == "trace") {
        spdlog::set_level(spdlog::level::trace);
    } else if (logLevel == "debug") {
        spdlog::set_level(spdlog::level::debug);
    } else if (logLevel == "warn") {
        spdlog::set_level(spdlog::level::warn);
    } else if (logLevel == "error") {
        spdlog::set_level(spdlog::level::err);
    } else {
        spdlog::set_level(spdlog::level::info);
    }
    BGL_LOG_INFO("bgl logger initialized successfully at level: {}", SanitizeForLog(logLevel));
}

std::string SanitizeForLog(std::string_view input) {
    constexpr size_t kMaxLogFieldChars{1024u};
    const size_t clippedLen = std::min(input.size(), kMaxLogFieldChars);
    std::string out{};
    out.reserve(clippedLen);
    for (size_t i = 0; i < clippedLen; ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch < 0x20u || ch == 0x7Fu) {
            char buf[5];
            std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned int>(ch));
            out += buf;
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    if (input.size() > kMaxLogFieldChars) {
        out += "...";
    }
    return out;
}

} // namespace bgl::core
