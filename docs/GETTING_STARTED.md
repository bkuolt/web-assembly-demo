# Getting Started with bgl Engine

## 1. Prerequisites & Build

Install dependencies via Conan 2.x and build the engine, viewer, samples, and unit tests:

```bash
task build
```

Or manually with CMake:

```bash
conan install . -s build_type=Release --build=missing --lockfile=conan.lock
cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake
cmake --build build/Release -j"$(nproc)"
```

## 2. Run the Samples

From the repository root:

```bash
./build/Release/sample_01_hello_engine
./build/Release/sample_02_entities_and_lights
./build/Release/sample_03_sprites_and_billboards
```

Run the full reference viewer:

```bash
./build/Release/bgl_viewer
```

## 3. Use bgl Engine in Your Own Project

Add the repository as a subdirectory (with Conan dependencies and `third_party/wgpu-native` configured):

```cmake
add_subdirectory(bgl-engine)
target_link_libraries(my_app PRIVATE bgl::engine)
```

In `main.cpp`, include `<bgl/bgl.hpp>`, subclass `bgl::App`, and finish with `BGL_RUN(MyApp)`.

## 4. Add a New Sample

1. Create `samples/04_my_sample/main.cpp` using `#include <bgl/bgl.hpp>`.
2. Register it in `samples/CMakeLists.txt`:
   ```cmake
   bgl_add_sample(sample_04_my_sample 04_my_sample/main.cpp)
   ```
3. Build and verify tests & layering rules:
   ```bash
   ctest --test-dir build/Release --output-on-failure -E BrowserE2ETest
   ```
