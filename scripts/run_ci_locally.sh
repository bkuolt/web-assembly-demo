#!/usr/bin/env bash
set -euo pipefail

echo "================================================================================"
echo "[Local CI Runner] Starting bgl-Engine CI Pipeline"
echo "================================================================================"

BUILD_DIR="build/Release"

# Step 1: Conan Dependency Resolution
echo "[Step 1/5] Resolving dependencies via Conan 2.x..."
conan install . -s build_type=Release --build=missing

# Step 2: CMake Configuration
echo "[Step 2/5] Configuring CMake with Conan Toolchain..."
cmake -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${BUILD_DIR}/generators/conan_toolchain.cmake" \
  -DBGL_BUILD_TESTING=ON

# Step 3: Compilation
echo "[Step 3/5] Building native libraries and viewer..."
cmake --build "${BUILD_DIR}" -j"$(nproc)"

mkdir -p build-native
cp -f "${BUILD_DIR}/bgl_viewer" build-native/
cp -f "${BUILD_DIR}/bgl_unit_tests" build-native/
cp -f "${BUILD_DIR}"/libbgl_*.so build-native/
cp -f third_party/wgpu-native/lib/libwgpu_native.so build-native/

# Step 4: Unit Tests
echo "[Step 4/5] Running GTest unit test suites..."
ctest --test-dir "${BUILD_DIR}" --output-on-failure

# Step 5: Smoke Test
echo "[Step 5/5] Running native WebGPU smoke test (timeout 2s)..."
timeout 2s ./"${BUILD_DIR}/bgl_viewer" || [ $? -eq 124 ]

echo "================================================================================"
echo "[Local CI Runner] SUCCESS: All CI pipeline stages passed cleanly!"
echo "================================================================================"

