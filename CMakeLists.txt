cmake_minimum_required(VERSION 3.28)
project(bgl_engine VERSION 0.6.3 LANGUAGES C CXX)

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

# Find Conan-managed Dependencies
find_package(fastgltf REQUIRED CONFIG)
find_package(glm REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(PNG REQUIRED)
find_package(libjpeg-turbo REQUIRED CONFIG)
find_package(WebP REQUIRED CONFIG)
find_package(miniaudio REQUIRED CONFIG)

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
)
target_include_directories(bgl_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
)
target_link_libraries(bgl_core PUBLIC ${BGL_SPDLOG_TARGET} ${BGL_GLM_TARGET})
if(NOT EMSCRIPTEN)
    target_link_libraries(bgl_core PUBLIC ${SDL2_LIBRARIES})
    target_include_directories(bgl_core PUBLIC ${SDL2_INCLUDE_DIRS})
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
    PNG::PNG
    libjpeg-turbo::libjpeg-turbo
    libwebp::libwebp
)
if(NOT EMSCRIPTEN)
    target_link_libraries(bgl_scene PUBLIC OpenGL::GL)
endif()

# 5. libbgl_app.so
add_library(bgl_app SHARED
    src/app/Application.cpp
)
target_include_directories(bgl_app PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(bgl_app PUBLIC bgl_scene)

# 6. Native Viewer Executable
add_executable(bgl_viewer src/main.cpp)
target_include_directories(bgl_viewer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(bgl_viewer PRIVATE bgl_app)

# 7. Unit Testing Suite
if(BGL_BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED CONFIG)
    add_executable(bgl_unit_tests
        tests/test_main.cpp
        tests/test_memory_pool.cpp
        tests/test_asset_manager.cpp
        tests/test_quadtree_lod.cpp
        tests/test_ray_picker.cpp
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
