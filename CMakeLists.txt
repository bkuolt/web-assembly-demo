cmake_minimum_required(VERSION 3.28)
project(bgl_engine VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Options
option(BGL_ENABLE_FORMAT_PNG   "Enable PNG decoding via libpng"         ON)
option(BGL_ENABLE_FORMAT_JPEG  "Enable JPEG decoding via libjpeg-turbo" ON)
option(BGL_ENABLE_FORMAT_WEBP  "Enable WebP decoding via libwebp"       ON)
option(BGL_ENABLE_FORMAT_KTX2  "Enable KTX2/BasisU decoding via libktx" ON)
option(BGL_ENABLE_AUDIO        "Enable 3D Audio via miniaudio"          ON)
option(BGL_ENABLE_GAMEPAD      "Enable Gamepad controller support"      ON)
option(BGL_ENABLE_SKYBOX       "Enable Skybox and IBL reflections"      ON)
option(BGL_ENABLE_MODEL_TOYCAR "Include default ToyCar.glb model"       ON)
option(BGL_BUILD_TESTING       "Build GTest unit test suite"            ON)

# Find Packages
find_package(PkgConfig REQUIRED)
find_package(glm REQUIRED)
find_package(spdlog REQUIRED)

if(EMSCRIPTEN)
    add_compile_definitions(BGL_PLATFORM_WASM)
else()
    add_compile_definitions(BGL_PLATFORM_NATIVE)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(LIBPNG libpng)
    pkg_check_modules(LIBJPEG libjpeg)
    pkg_check_modules(LIBWEBP libwebp)
endif()

# Global includes
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fastgltf
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webgpu
)

# 1. libbgl_core.so
add_library(bgl_core SHARED
    src/core/Log.cpp
    src/core/MemoryPool.cpp
    src/core/NetworkFetcher.cpp
    src/core/Gamepad.cpp
    src/core/Camera.cpp
)
target_link_libraries(bgl_core PUBLIC spdlog::spdlog glm::glm)
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
target_link_libraries(bgl_gpu PUBLIC bgl_core)

# 3. libbgl_audio.so
add_library(bgl_audio SHARED
    src/audio/SoundEngine.cpp
)
target_link_libraries(bgl_audio PUBLIC bgl_core)

# 4. libbgl_scene.so
add_library(bgl_scene SHARED
    src/scene/TextureDecoder.cpp
    src/scene/AssetManager.cpp
    src/scene/FastGltfLoader.cpp
    src/scene/Quadtree.cpp
    src/scene/RayPicker.cpp
)
target_link_libraries(bgl_scene PUBLIC bgl_core bgl_gpu bgl_audio)
if(NOT EMSCRIPTEN)
    if(LIBPNG_FOUND)
        target_link_libraries(bgl_scene PRIVATE ${LIBPNG_LIBRARIES})
    endif()
    if(LIBJPEG_FOUND)
        target_link_libraries(bgl_scene PRIVATE ${LIBJPEG_LIBRARIES})
    endif()
    if(LIBWEBP_FOUND)
        target_link_libraries(bgl_scene PRIVATE ${LIBWEBP_LIBRARIES})
    endif()
endif()

# 5. libbgl_app.so
add_library(bgl_app SHARED
    src/app/Application.cpp
)
target_link_libraries(bgl_app PUBLIC bgl_scene)

# Viewer Executable
add_executable(bgl_viewer src/main.cpp)
target_link_libraries(bgl_viewer PRIVATE bgl_app)

# Unit Testing
if(BGL_BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(bgl_unit_tests
        tests/test_main.cpp
        tests/test_memory_pool.cpp
        tests/test_asset_manager.cpp
        tests/test_quadtree_lod.cpp
        tests/test_ray_picker.cpp
    )
    target_link_libraries(bgl_unit_tests PRIVATE bgl_app GTest::gtest GTest::gtest_main)
    add_test(NAME AllUnitTests COMMAND bgl_unit_tests)
endif()

