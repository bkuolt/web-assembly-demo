cmake_minimum_required(VERSION 3.28)
project(bgl_engine VERSION 0.6.8 LANGUAGES C CXX)

# Modern C++23 Standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Automatic RPATH configuration for native desktop execution
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH
    "$ORIGIN"
    "$ORIGIN/../../third_party/wgpu-native/lib"
    "$ORIGIN/third_party/wgpu-native/lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib"
)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Options
option(BGL_ENABLE_AUDIO        "Enable 3D Spatial Audio via miniaudio"  ON)
option(BGL_ENABLE_GAMEPAD      "Enable Gamepad controller support"      ON)
option(BGL_ENABLE_SKYBOX       "Enable Skybox and reflections"          ON)
option(BGL_ENABLE_MODEL_TOYCAR "Include default ToyCar.glb model"       ON)
option(BGL_BUILD_TESTING       "Build GTest unit test suite"            ON)

# Compiler Optimization and Warning Flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -fno-math-errno)
    endif()
endif()

# ==============================================================================
# Texture Format Options (All ON by default)
# ==============================================================================
option(BGL_ENABLE_PNG "Enable PNG texture format decoding (libpng)" ON)
option(BGL_ENABLE_JPEG "Enable JPEG texture format decoding (libjpeg-turbo)" ON)
option(BGL_ENABLE_WEBP "Enable WebP texture format decoding (libwebp)" ON)
option(BGL_ENABLE_KTX "Enable KTX and KTX2 texture format decoding (libktx)" ON)

message(STATUS "bgl-Engine Texture Formats Configuration:")
message(STATUS "  - PNG:  ${BGL_ENABLE_PNG}")
message(STATUS "  - JPEG: ${BGL_ENABLE_JPEG}")
message(STATUS "  - WebP: ${BGL_ENABLE_WEBP}")
message(STATUS "  - KTX:  ${BGL_ENABLE_KTX}")

# Find Conan-managed Dependencies
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

# Determine correct Conan Targets
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

if(EMSCRIPTEN)
    add_compile_definitions(BGL_PLATFORM_WASM)
else()
    add_compile_definitions(BGL_PLATFORM_NATIVE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    find_package(OpenGL REQUIRED)
endif()

# 1. libbgl_core.so
add_library(bgl_core SHARED
    src/core/Log.cpp
    src/core/MemoryPool.cpp
    src/core/NetworkFetcher.cpp
    src/core/Gamepad.cpp
    src/core/Camera.cpp
    src/core/CrashHandler.cpp
)
target_include_directories(bgl_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
)
target_link_libraries(bgl_core PUBLIC ${BGL_SPDLOG_TARGET} ${BGL_GLM_TARGET})
if(NOT EMSCRIPTEN)
    target_link_libraries(bgl_core PUBLIC ${SDL2_LIBRARIES} ${CMAKE_DL_LIBS})
    target_include_directories(bgl_core PUBLIC ${SDL2_INCLUDE_DIRS})

    # C++23 std::stacktrace detection
    include(CheckCXXSourceCompiles)
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
endif()

# 2. libbgl_gpu.so
add_library(bgl_gpu SHARED
    src/gpu/Context.cpp
    src/gpu/Skybox.cpp
    src/gpu/Pipeline.cpp
)
target_include_directories(bgl_gpu PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/include>
)
target_link_libraries(bgl_gpu PUBLIC bgl_core)
if(NOT EMSCRIPTEN)
    target_link_libraries(bgl_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib/libwgpu_native.so OpenGL::GL)
endif()

# 3. libbgl_audio.so
add_library(bgl_audio SHARED
    src/audio/SoundEngine.cpp
)
target_include_directories(bgl_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(bgl_audio PUBLIC bgl_core miniaudio::miniaudio)

# 4. libbgl_scene.so
add_library(bgl_scene SHARED
    src/scene/TextureDecoder.cpp
    src/scene/AssetManager.cpp
    src/scene/FastGltfLoader.cpp
    src/scene/Quadtree.cpp
    src/scene/RayPicker.cpp
)
target_include_directories(bgl_scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
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
    target_link_libraries(bgl_scene PUBLIC libwebp::libwebp)
endif()
if(BGL_ENABLE_KTX)
    target_compile_definitions(bgl_scene PUBLIC BGL_ENABLE_KTX)
    target_link_libraries(bgl_scene PUBLIC KTX::ktx)
endif()
if(NOT EMSCRIPTEN)
    target_link_libraries(bgl_scene PUBLIC OpenGL::GL)
endif()

# 5. libbgl_text.so (FreeType 2 & HarfBuzz)
add_library(bgl_text SHARED
    src/text/EmbeddedFont.cpp
    src/text/FontAtlas.cpp
    src/text/TextShaper.cpp
    src/text/TextRenderer.cpp
)
target_include_directories(bgl_text PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/include>
)
target_link_libraries(bgl_text PUBLIC
    bgl_core
    bgl_gpu
    Freetype::Freetype
    harfbuzz::harfbuzz
)

# 6. libbgl_ui.so (Debug Overlay & HUD)
add_library(bgl_ui SHARED
    src/ui/DebugOverlay.cpp
)
target_include_directories(bgl_ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/include>
)
target_link_libraries(bgl_ui PUBLIC
    bgl_core
    bgl_gpu
    bgl_text
)

# 7. libbgl_app.so
add_library(bgl_app SHARED
    src/app/Application.cpp
    src/app/CommandLine.cpp
)
target_include_directories(bgl_app PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(bgl_app PUBLIC bgl_scene bgl_text bgl_ui)

# 8. Native Viewer Executable
add_executable(bgl_viewer src/main.cpp)
target_include_directories(bgl_viewer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(bgl_viewer PRIVATE bgl_app)

# 9. Unit Testing Suite
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
    )
    target_include_directories(bgl_unit_tests PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    )
    if(TARGET GTest::gtest)
        target_link_libraries(bgl_unit_tests PRIVATE bgl_app GTest::gtest GTest::gtest_main)
    else()
        target_link_libraries(bgl_unit_tests PRIVATE bgl_app gtest::gtest gtest::gtest_main)
    endif()
    add_test(NAME AllUnitTests COMMAND bgl_unit_tests)
endif()

# ==============================================================================
# Packaging Targets
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
