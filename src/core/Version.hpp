/**
 * @file Version.hpp
 * @brief Centralized engine version information and branding constants.
 */
#pragma once

#include <string_view>

namespace bgl {

inline constexpr std::string_view kEngineVersion = "0.8.3";
inline constexpr std::string_view kEngineName = "WebAssembly and WebGPU Demo";
inline constexpr uint32_t kEngineVersionMajor = 0;
inline constexpr uint32_t kEngineVersionMinor = 8;
inline constexpr uint32_t kEngineVersionPatch = 3;

} // namespace bgl

