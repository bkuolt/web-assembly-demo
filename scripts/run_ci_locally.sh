#!/usr/bin/env bash
set -euo pipefail

echo "================================================================================"
echo "[Local CI Runner] Starting bgl-Engine CI Pipeline"
echo "================================================================================"

BUILD_DIR="build/Release"
LOCK_ARG=""
if [[ -f "conan.lock" ]]; then
  LOCK_ARG="--lockfile=conan.lock"
fi

# Step 1: Code Formatting, Static Analysis & Web Script Validation
echo "[Step 1/7] Checking C++23 formatting, clang-tidy, and web client scripts..."
if command -v clang-format >/dev/null 2>&1; then
  find src apps include samples tests \( -name "*.cpp" -o -name "*.hpp" \) -print0 | xargs -0 clang-format --dry-run -Werror
fi
if command -v clang-tidy >/dev/null 2>&1; then
  clang-tidy --config-file=.clang-tidy src/wasm/wasm_bridge.cpp -- -std=c++23 -Wall -Wextra -Isrc
fi
if command -v node >/dev/null 2>&1; then
  node --check web/js/core.js
  node --check web/js/gltf.js
  node --check web/js/renderer.js
  node --check web/app.js
  node --check web/gltf_worker.js
  node tests/test_web_unit.js
  node tests/test_webgpu_path.js
fi

# Step 2: Optional Emscripten WebAssembly Build (mirrors wasm-emscripten CI job)
echo "[Step 2/7] Building standalone WebAssembly module (if Emscripten SDK is available)..."
if ! command -v em++ >/dev/null 2>&1 && [[ -f "/opt/emsdk/emsdk_env.sh" ]]; then
  # shellcheck disable=SC1091
  source "/opt/emsdk/emsdk_env.sh" >/dev/null 2>&1 || true
fi
if command -v em++ >/dev/null 2>&1; then
  mkdir -p web/dist
  em++ -O3 -std=c++23 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -ffreestanding -c src/wasm/wasm_bridge.cpp -o /tmp/wasm_bridge.o
  wasm-ld --no-entry --strip-debug --import-memory --initial-memory=16777216 --max-memory=67108864 -z stack-size=65536 \
    --export=malloc --export=free \
    --export=bgl_init --export=bgl_resize --export=bgl_load_file_buffer \
    --export=bgl_select_node --export=bgl_clear_selection --export=bgl_pick_node \
    --export=bgl_get_camera_count --export=bgl_select_camera \
    --export=bgl_set_animation --export=bgl_set_animation_playing \
    --export=bgl_toggle_bbox --export=bgl_toggle_quadtree --export=bgl_toggle_skybox \
    --export=bgl_toggle_debug_overlay --export=bgl_toggle_bump --export=bgl_toggle_mipmaps \
    --export=bgl_toggle_shadows --export=bgl_toggle_reflections --export=bgl_toggle_ao \
    --export=bgl_toggle_pom --export=bgl_toggle_lens_flare --export=bgl_toggle_ssao \
    --export=bgl_toggle_bloom --export=bgl_toggle_occlusion --export=bgl_toggle_camera_mode \
    --export=bgl_toggle_turntable --export=bgl_toggle_day_night_cycle \
    --export=bgl_toggle_lod --export=bgl_toggle_auto_lod \
    --export=bgl_set_ssao_intensity --export=bgl_set_bloom_intensity --export=bgl_set_ao_intensity \
    --export=bgl_set_bump_scale --export=bgl_set_parallax_scale --export=bgl_set_lens_flare_intensity \
    --export=bgl_set_weather_mode --export=bgl_set_weather_intensity --export=bgl_set_wind_strength \
    --export=bgl_set_turntable_speed --export=bgl_set_time_of_day --export=bgl_set_day_night_speed \
    --export=bgl_set_lod_bias --export=bgl_set_debug_view \
    --export=bgl_camera_focus_selection --export=bgl_camera_frame_all \
    --export=bgl_camera_save_bookmark --export=bgl_camera_restore_bookmark \
    --export=bgl_camera_move --export=bgl_camera_zoom --export=bgl_camera_orbit --export=bgl_camera_pan \
    --export=bgl_gamepad_state \
    /tmp/wasm_bridge.o -o web/dist/bgl_engine.wasm
  if command -v node >/dev/null 2>&1; then
    node -e "const fs = require('fs'); const m = new WebAssembly.Module(fs.readFileSync('web/dist/bgl_engine.wasm')); const imp = WebAssembly.Module.imports(m); if (imp.length !== 1 || imp[0].module !== 'env' || imp[0].name !== 'memory') throw new Error('Unexpected WASM imports');"
  fi
  python3 scripts/package_web.py --output dist/bgl_web_app_minimal.zip
else
  echo "  [SKIP] em++ not found; skipping local WASM rebuild."
fi

# Step 3: Conan Dependency Resolution
echo "[Step 3/7] Resolving dependencies via Conan 2.x..."
conan install . -s build_type=Release --build=missing ${LOCK_ARG}

# Step 4: CMake Configuration
echo "[Step 4/7] Configuring CMake with Conan Toolchain..."
cmake -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
  -DCMAKE_TOOLCHAIN_FILE="${BUILD_DIR}/generators/conan_toolchain.cmake" \
  -DBGL_BUILD_TESTING=ON

if command -v clang-tidy >/dev/null 2>&1; then
  clang-tidy -p "${BUILD_DIR}" --config-file=.clang-tidy \
    src/core/Camera.cpp src/core/MemoryPool.cpp src/gpu/ShadowPass.cpp
fi

# Step 5: Compilation
echo "[Step 5/7] Building native libraries and viewer..."
cmake --build "${BUILD_DIR}" -j"$(nproc)"

mkdir -p build-native
cp -f "${BUILD_DIR}/bgl_viewer" build-native/
cp -f "${BUILD_DIR}/bgl_unit_tests" build-native/
cp -f "${BUILD_DIR}"/sample_* build-native/ 2>/dev/null || true
cp -f "${BUILD_DIR}"/libbgl_*.so build-native/
if [[ -f "third_party/wgpu-native/lib/libwgpu_native.so" ]]; then
  cp -f third_party/wgpu-native/lib/libwgpu_native.so build-native/
fi

# Step 6: Unit Tests (GTest & Node WebGPU Suites)
echo "[Step 6/7] Running GTest and Web unit test suites..."
ctest --test-dir "${BUILD_DIR}" --output-on-failure -E BrowserE2ETest

# Step 7: Smoke Test
echo "[Step 7/7] Running native WebGPU smoke test (headless CI mode)..."
timeout 20s ./"${BUILD_DIR}/bgl_viewer" --headless assets/ToyCar.glb

echo "================================================================================"
echo "[Local CI Runner] SUCCESS: All CI pipeline stages passed cleanly!"
echo "================================================================================"
