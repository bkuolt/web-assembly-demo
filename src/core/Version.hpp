/**
 * @file Version.hpp
 * @brief Centralized engine version information and branding constants.
 * Generated automatically by CMake configure_file from Version.hpp.in.
 */
#ifndef BGL_CORE_VERSION_HPP
#define BGL_CORE_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace bgl {

inline constexpr std::string_view kEngineVersion = "1.2.0";
inline constexpr std::string_view kEngineName = "bgl Engine";
inline constexpr uint32_t kEngineVersionMajor = 1;
inline constexpr uint32_t kEngineVersionMinor = 2;
inline constexpr uint32_t kEngineVersionPatch = 0;

} // namespace bgl

#endif // BGL_CORE_VERSION_HPP
