cmake_minimum_required(VERSION 3.28)
project(bgl_engine VERSION 0.9.11 LANGUAGES C CXX)

# ==============================================================================
# RPATH Configuration for Native Desktop Execution
# ==============================================================================
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
set(CMAKE_INSTALL_RPATH
    "$ORIGIN"
    "$ORIGIN/../../third_party/wgpu-native/lib"
    "$ORIGIN/third_party/wgpu-native/lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib"
)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# ==============================================================================
# Subsystem and Feature Options
# ==============================================================================
option(BGL_ENABLE_AUDIO        "Enable 3D Spatial Audio via miniaudio"          ON)
option(BGL_ENABLE_GAMEPAD      "Enable Gamepad controller support"              ON)
option(BGL_ENABLE_SKYBOX       "Enable Skybox and reflections"                  ON)
option(BGL_ENABLE_CURL         "Enable HTTP/HTTPS URL fetching (libcurl)"       ON)
option(BGL_ENABLE_MODEL_TOYCAR "Include default ToyCar.glb model"               ON)
option(BGL_BUILD_TESTING       "Build GTest unit test suite"                    ON)
option(BGL_BUILD_FUZZING       "Build libFuzzer fuzzing targets"                OFF)

# Texture Decoder Options
option(BGL_ENABLE_PNG          "Enable PNG texture format decoding (libpng)"         ON)
option(BGL_ENABLE_JPEG         "Enable JPEG texture format decoding (libjpeg-turbo)" ON)
option(BGL_ENABLE_WEBP         "Enable WebP texture format decoding (libwebp)"       ON)
option(BGL_ENABLE_KTX          "Enable KTX/KTX2 texture format decoding (libktx)"    ON)

# ==============================================================================
# Centralized Version Configuration
# ==============================================================================
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Version.hpp.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Version.hpp
    @ONLY
)

# ==============================================================================
# Common Interface Target: bgl_common_settings
# Encapsulates C++23 standard, warning flags, optimizations, and include paths
# ==============================================================================
add_library(bgl_common_settings INTERFACE)

target_compile_features(bgl_common_settings INTERFACE cxx_std_23)
target_compile_definitions(bgl_common_settings INTERFACE GLM_ENABLE_EXPERIMENTAL)
set_target_properties(bgl_common_settings PROPERTIES
    INTERFACE_POSITION_INDEPENDENT_CODE ON
)

target_include_directories(bgl_common_settings INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# Compiler Warning and Optimization Flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bgl_common_settings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-fno-math-errno>
    )
endif()

# Propagate Feature Flags
if(BGL_ENABLE_AUDIO)
    target_compile_definitions(bgl_common_settings INTERFACE BGL_ENABLE_AUDIO)
endif()
if(BGL_ENABLE_GAMEPAD)
    target_compile_definitions(bgl_common_settings INTERFACE BGL_ENABLE_GAMEPAD)
endif()
if(BGL_ENABLE_SKYBOX)
    target_compile_definitions(bgl_common_settings INTERFACE BGL_ENABLE_SKYBOX)
endif()
if(BGL_ENABLE_MODEL_TOYCAR)
    target_compile_definitions(bgl_common_settings INTERFACE BGL_ENABLE_MODEL_TOYCAR)
endif()

if(EMSCRIPTEN)
    target_compile_definitions(bgl_common_settings INTERFACE BGL_PLATFORM_WASM)
    set(BGL_LIB_TYPE STATIC)
else()
    target_compile_definitions(bgl_common_settings INTERFACE BGL_PLATFORM_NATIVE)
    set(BGL_LIB_TYPE SHARED)
endif()

# ==============================================================================
# Find External Dependencies (Conan 2.x & System Packages)
# ==============================================================================
list(PREPEND CMAKE_PREFIX_PATH
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_BINARY_DIR}/generators"
    "${CMAKE_BINARY_DIR}/Release/generators"
    "${CMAKE_BINARY_DIR}/Debug/generators"
)

find_package(fastgltf REQUIRED CONFIG)
find_package(glm REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(miniaudio REQUIRED CONFIG)
find_package(freetype REQUIRED CONFIG)
find_package(harfbuzz REQUIRED CONFIG)

if(BGL_ENABLE_PNG)
    find_package(PNG REQUIRED)
endif()
if(BGL_ENABLE_JPEG)
    find_package(libjpeg-turbo REQUIRED CONFIG)
endif()
if(BGL_ENABLE_WEBP)
    find_package(WebP REQUIRED CONFIG)
endif()
if(BGL_ENABLE_KTX)
    find_package(Ktx REQUIRED CONFIG)
endif()

# Canonical Conan Target Selection
if(TARGET spdlog::spdlog_header_only)
    set(BGL_SPDLOG_TARGET spdlog::spdlog_header_only)
else()
    set(BGL_SPDLOG_TARGET spdlog::spdlog)
endif()

if(TARGET glm::glm-header-only)
    set(BGL_GLM_TARGET glm::glm-header-only)
else()
    set(BGL_GLM_TARGET glm::glm)
endif()

# Platform Native Windowing & Hardware Dependencies
if(NOT EMSCRIPTEN)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

    if(BGL_ENABLE_CURL)
        pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
    endif()

    # Imported Target for wgpu-native
    add_library(wgpu_native SHARED IMPORTED GLOBAL)
    set_target_properties(wgpu_native PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib/libwgpu_native.so"
        IMPORTED_LOCATION_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib/libwgpu_native.so"
        IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib/libwgpu_native.so"
        IMPORTED_LOCATION_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib/libwgpu_native.so"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/include"
    )
endif()

# Diagnostic Status Report
message(STATUS "--------------------------------------------------------------------------------")
message(STATUS "bgl-Engine Configuration (${PROJECT_VERSION}):")
message(STATUS "  - Target Platform:     ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  - Compiler:            ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  - Build Type:          ${CMAKE_BUILD_TYPE}")
message(STATUS "  - Library Type:        ${BGL_LIB_TYPE}")
message(STATUS "  - 3D Audio:            ${BGL_ENABLE_AUDIO}")
message(STATUS "  - Gamepad Support:     ${BGL_ENABLE_GAMEPAD}")
message(STATUS "  - Atmospheric Skybox:  ${BGL_ENABLE_SKYBOX}")
message(STATUS "  - libcurl Fetching:    ${BGL_ENABLE_CURL}")
message(STATUS "  - Texture Decoders:    PNG=${BGL_ENABLE_PNG} JPEG=${BGL_ENABLE_JPEG} WebP=${BGL_ENABLE_WEBP} KTX2=${BGL_ENABLE_KTX}")
message(STATUS "  - Unit Test Suite:     ${BGL_BUILD_TESTING}")
message(STATUS "--------------------------------------------------------------------------------")

# ==============================================================================
# 1. libbgl_core
# ==============================================================================
add_library(bgl_core ${BGL_LIB_TYPE}
    src/core/Log.cpp
    src/core/MemoryPool.cpp
    src/core/ThreadPool.cpp
    src/core/NetworkFetcher.cpp
    src/core/Gamepad.cpp
    src/core/Camera.cpp
    src/core/CrashHandler.cpp
    src/core/MemoryTracker.cpp
)
target_include_directories(bgl_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
)
target_link_libraries(bgl_core PUBLIC
    bgl_common_settings
    ${BGL_SPDLOG_TARGET}
    ${BGL_GLM_TARGET}
)

if(NOT EMSCRIPTEN)
    target_link_libraries(bgl_core PUBLIC PkgConfig::SDL2 ${CMAKE_DL_LIBS})

    if(BGL_ENABLE_CURL AND TARGET PkgConfig::CURL)
        target_link_libraries(bgl_core PUBLIC PkgConfig::CURL)
        target_compile_definitions(bgl_core PUBLIC BGL_HAS_CURL)
    endif()

    # C++23 std::stacktrace feature detection
    include(CheckCXXSourceCompiles)
    set(_PREV_REQ_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "-std=c++23")
    check_cxx_source_compiles("
    #include <stacktrace>
    int main() { (void)std::stacktrace::current(); }
    " BGL_HAVE_STD_STACKTRACE_BUILTIN)

    if(BGL_HAVE_STD_STACKTRACE_BUILTIN)
        target_compile_definitions(bgl_core PUBLIC BGL_HAS_STD_STACKTRACE)
    else()
        set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
        check_cxx_source_compiles("
        #include <stacktrace>
        int main() { (void)std::stacktrace::current(); }
        " BGL_HAVE_STD_STACKTRACE_EXP)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(BGL_HAVE_STD_STACKTRACE_EXP)
            target_link_libraries(bgl_core PUBLIC stdc++exp)
            target_compile_definitions(bgl_core PUBLIC BGL_HAS_STD_STACKTRACE)
        endif()
    endif()
    set(CMAKE_REQUIRED_FLAGS "${_PREV_REQ_FLAGS}")
    unset(_PREV_REQ_FLAGS)
endif()

# ==============================================================================
# 2. libbgl_gpu
# ==============================================================================
add_library(bgl_gpu ${BGL_LIB_TYPE}
    src/gpu/Context.cpp
    src/gpu/Skybox.cpp
    src/gpu/AabbRenderer.cpp
    src/gpu/ShadowPass.cpp
    src/gpu/PostProcessPass.cpp
    src/gpu/GpuCuller.cpp
    src/gpu/GpuRayPicker.cpp
    src/gpu/Pipeline.cpp
)
target_link_libraries(bgl_gpu PUBLIC bgl_core)
if(NOT EMSCRIPTEN)
    target_link_libraries(bgl_gpu PUBLIC wgpu_native)
endif()

# ==============================================================================
# 3. libbgl_audio
# ==============================================================================
add_library(bgl_audio ${BGL_LIB_TYPE}
    src/audio/SoundEngine.cpp
)
target_link_libraries(bgl_audio PUBLIC bgl_core miniaudio::miniaudio)

# ==============================================================================
# 4. libbgl_scene
# ==============================================================================
add_library(bgl_scene ${BGL_LIB_TYPE}
    src/scene/Scene.cpp
    src/scene/Animation.cpp
    src/scene/TextureDecoder.cpp
    src/scene/AssetManager.cpp
    src/scene/FastGltfLoader.cpp
    src/scene/Quadtree.cpp
    src/scene/RayPicker.cpp
)
target_link_libraries(bgl_scene PUBLIC
    bgl_core
    bgl_gpu
    bgl_audio
    fastgltf::fastgltf
)

if(BGL_ENABLE_PNG)
    target_compile_definitions(bgl_scene PUBLIC BGL_ENABLE_PNG)
    target_link_libraries(bgl_scene PUBLIC PNG::PNG)
endif()
if(BGL_ENABLE_JPEG)
    target_compile_definitions(bgl_scene PUBLIC BGL_ENABLE_JPEG)
    target_link_libraries(bgl_scene PUBLIC libjpeg-turbo::libjpeg-turbo)
endif()
if(BGL_ENABLE_WEBP)
    target_compile_definitions(bgl_scene PUBLIC BGL_ENABLE_WEBP)
    if(TARGET libwebp::libwebp)
        target_link_libraries(bgl_scene PUBLIC libwebp::libwebp)
    elseif(TARGET WebP::webp)
        target_link_libraries(bgl_scene PUBLIC WebP::webp)
    endif()
endif()
if(BGL_ENABLE_KTX)
    target_compile_definitions(bgl_scene PUBLIC BGL_ENABLE_KTX)
    target_link_libraries(bgl_scene PUBLIC KTX::ktx)
endif()

# ==============================================================================
# 5. libbgl_text (FreeType 2 & HarfBuzz)
# ==============================================================================
add_library(bgl_text ${BGL_LIB_TYPE}
    src/text/EmbeddedFont.cpp
    src/text/FontAtlas.cpp
    src/text/TextShaper.cpp
    src/text/TextRenderer.cpp
)
target_link_libraries(bgl_text PUBLIC
    bgl_core
    bgl_gpu
    Freetype::Freetype
    harfbuzz::harfbuzz
)

# ==============================================================================
# 6. libbgl_ui (Debug Overlay & In-Engine HUD)
# ==============================================================================
add_library(bgl_ui ${BGL_LIB_TYPE}
    src/ui/DebugOverlay.cpp
)
target_link_libraries(bgl_ui PUBLIC
    bgl_core
    bgl_gpu
    bgl_text
)

# ==============================================================================
# 7. libbgl_app
# ==============================================================================
add_library(bgl_app ${BGL_LIB_TYPE}
    src/app/Application.cpp
    src/app/CommandLine.cpp
)
target_link_libraries(bgl_app PUBLIC
    bgl_scene
    bgl_text
    bgl_ui
)

# ==============================================================================
# 8. Native Viewer Executable
# ==============================================================================
add_executable(bgl_viewer src/main.cpp)
target_link_libraries(bgl_viewer PRIVATE bgl_app)

# ==============================================================================
# 9. Unit Testing Suite (GTest)
# ==============================================================================
if(BGL_BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED CONFIG)
    add_executable(bgl_unit_tests
        tests/test_main.cpp
        tests/test_memory_pool.cpp
        tests/test_asset_manager.cpp
        tests/test_types_math.cpp
        tests/test_camera.cpp
        tests/test_quadtree_lod.cpp
        tests/test_ray_picker.cpp
        tests/test_gltf_loader.cpp
        tests/test_texture_decoders.cpp
        tests/test_text_rendering.cpp
        tests/test_crash_handler.cpp
        tests/test_command_line.cpp
        tests/test_rendering_features.cpp
        tests/test_golden_image.cpp
        tests/test_animation.cpp
    )

    if(TARGET GTest::gtest)
        target_link_libraries(bgl_unit_tests PRIVATE bgl_app GTest::gtest GTest::gtest_main)
    else()
        target_link_libraries(bgl_unit_tests PRIVATE bgl_app gtest::gtest gtest::gtest_main)
    endif()

    add_test(NAME AllUnitTests COMMAND bgl_unit_tests)
    set_tests_properties(AllUnitTests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
    )

    find_program(NODE_EXECUTABLE node)
    if(NODE_EXECUTABLE)
        add_test(NAME WebGLPipelineTest COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_webgl_path.js")
        set_tests_properties(WebGLPipelineTest PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        )
    endif()
endif()

# ==============================================================================
# 10. Fuzzing Targets (libFuzzer)
# ==============================================================================
if(BGL_BUILD_FUZZING)
    add_executable(fuzz_texture_decoder tests/fuzz/fuzz_texture_decoder.cpp)
    target_link_libraries(fuzz_texture_decoder PRIVATE bgl_scene)
    target_compile_options(fuzz_texture_decoder PRIVATE -fsanitize=fuzzer,address)
    target_link_options(fuzz_texture_decoder PRIVATE -fsanitize=fuzzer,address)

    add_executable(fuzz_gltf_loader tests/fuzz/fuzz_gltf_loader.cpp)
    target_link_libraries(fuzz_gltf_loader PRIVATE bgl_scene)
    target_compile_options(fuzz_gltf_loader PRIVATE -fsanitize=fuzzer,address)
    target_link_options(fuzz_gltf_loader PRIVATE -fsanitize=fuzzer,address)
endif()

# ==============================================================================
# 11. Packaging & Web Distribution Targets
# ==============================================================================
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(package_web
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_web.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Packaging minimal self-contained web app into dist/bgl_web_app_minimal.zip"
        VERBATIM
    )
endif()

# ==============================================================================
# 12. Standard Installation Rules (GNUInstallDirs)
# ==============================================================================
include(GNUInstallDirs)

install(TARGETS
    bgl_viewer
    bgl_app
    bgl_ui
    bgl_text
    bgl_scene
    bgl_audio
    bgl_gpu
    bgl_core
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib/libwgpu_native.so")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib/libwgpu_native.so"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bgl_engine
    )
endif()

