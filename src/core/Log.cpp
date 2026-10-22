/**
 * @file Log.cpp
 * @brief Implementation of logging initialisation for bgl::core.
 */
#include "core/Log.hpp"
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
    BGL_LOG_INFO("bgl logger initialized successfully at level: {}", logLevel);
}

} // namespace bgl::core

