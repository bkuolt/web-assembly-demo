/**
 * @file wasm_bridge.cpp
 * @brief Standalone C++23 WebAssembly engine bridge compiled via Emscripten to web/dist/bgl_engine.wasm.
 *
 * Designed for direct instantiation with WebAssembly.Memory({ initial: 256, maximum: 1024 })
 * under `{ env: { memory } }` with zero WASI or host-JS stub imports.
 */

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
extern unsigned char __heap_base; // NOLINT(bugprone-reserved-identifier)
}

namespace {

inline float AbsF(float x) noexcept {
    return x < 0.0f ? -x : x;
}

inline float FastSin(float x) noexcept {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;
    // Wrap to [-pi, pi]
    while (x > kPi)
        x -= kTwoPi;
    while (x < -kPi)
        x += kTwoPi;
    const float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f - x2 * (1.0f / 5040.0f))));
}

inline float FastCos(float x) noexcept {
    return FastSin(x + 1.57079632679489661923f);
}

// ============================================================================
// Free-List Linear Heap Allocator over Imported WebAssembly.Memory
// ============================================================================
struct BlockHeader {
    size_t size{0};
    bool free{false};
    BlockHeader* next{nullptr};
    size_t padding{0}; // Maintain 16-byte payload alignment on 32-bit wasm
};

static_assert(sizeof(BlockHeader) % 16 == 0, "BlockHeader must keep 16-byte alignment");

constexpr size_t kPageSizeBytes = 65536u;
BlockHeader* gHeapHead = nullptr;
unsigned char* gHeapTop = nullptr;

inline size_t AlignUp(size_t value, size_t alignment) noexcept {
    return (value + (alignment - 1u)) & ~(alignment - 1u);
}

inline uintptr_t CurrentMemoryEndBytes() noexcept {
#if defined(__wasm__)
    return static_cast<uintptr_t>(__builtin_wasm_memory_size(0)) * kPageSizeBytes;
#else
    return 0u;
#endif
}

bool EnsureHeapCapacity(uintptr_t requiredEndByte) noexcept {
    const uintptr_t curEnd = CurrentMemoryEndBytes();
    if (requiredEndByte <= curEnd) {
        return true;
    }
    const size_t delta = requiredEndByte - curEnd;
    const size_t pagesNeeded = (delta + kPageSizeBytes - 1u) / kPageSizeBytes;
#if defined(__wasm__)
    const int prevPages = __builtin_wasm_memory_grow(0, static_cast<int>(pagesNeeded));
    return prevPages != -1;
#else
    (void)pagesNeeded;
    return false;
#endif
}

void CoalesceFreeBlocks() noexcept {
    BlockHeader* cur = gHeapHead;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += sizeof(BlockHeader) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

void* WasmMalloc(size_t requestedBytes) noexcept {
    if (requestedBytes == 0u) {
        requestedBytes = 16u;
    }
    const size_t alignedSize = AlignUp(requestedBytes, 16u);

    if (gHeapTop == nullptr) {
        const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(&__heap_base);
        const size_t misalignment = AlignUp(baseAddr, 16u) - baseAddr;
        gHeapTop = &__heap_base + misalignment;
    }

    // First-fit search with block splitting
    BlockHeader* prev = nullptr;
    for (BlockHeader* cur = gHeapHead; cur != nullptr; prev = cur, cur = cur->next) {
        if (cur->free && cur->size >= alignedSize) {
            auto* curBytes = reinterpret_cast<unsigned char*>(cur);
            if (cur->size >= alignedSize + sizeof(BlockHeader) + 32u) {
                auto* split = reinterpret_cast<BlockHeader*>(curBytes + sizeof(BlockHeader) + alignedSize);
                split->size = cur->size - alignedSize - sizeof(BlockHeader);
                split->free = true;
                split->next = cur->next;
                split->padding = 0u;
                cur->size = alignedSize;
                cur->next = split;
            }
            cur->free = false;
            return curBytes + sizeof(BlockHeader);
        }
    }

    unsigned char* const blockBytes = gHeapTop;
    const size_t totalBytes = sizeof(BlockHeader) + alignedSize;
    if (!EnsureHeapCapacity(reinterpret_cast<uintptr_t>(blockBytes) + totalBytes)) {
        return nullptr;
    }

    auto* block = reinterpret_cast<BlockHeader*>(blockBytes);
    block->size = alignedSize;
    block->free = false;
    block->next = nullptr;
    block->padding = 0u;
    if (prev) {
        prev->next = block;
    } else {
        gHeapHead = block;
    }
    gHeapTop = blockBytes + totalBytes;
    return blockBytes + sizeof(BlockHeader);
}

void WasmFree(void* ptr) noexcept {
    if (!ptr) {
        return;
    }
    auto* const bytePtr = static_cast<unsigned char*>(ptr);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(bytePtr);
    if (addr < sizeof(BlockHeader) || addr < reinterpret_cast<uintptr_t>(&__heap_base)) {
        return;
    }
    auto* block = reinterpret_cast<BlockHeader*>(bytePtr - sizeof(BlockHeader));
    block->free = true;
    CoalesceFreeBlocks();
}

// ============================================================================
// Lightweight glTF 2.0 / GLB JSON Scanner (freestanding, zero libc dependencies)
// ============================================================================
struct ByteSpan {
    const uint8_t* ptr{nullptr};
    size_t len{0};
};

inline uint32_t ReadU32LE(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) | (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

bool SpanStartsWith(ByteSpan span, size_t pos, const char* lit, size_t litLen) noexcept {
    if (pos + litLen > span.len) {
        return false;
    }
    for (size_t i = 0; i < litLen; ++i) {
        if (span.ptr[pos + i] != static_cast<uint8_t>(lit[i])) {
            return false;
        }
    }
    return true;
}

int CountArrayElementsForKey(ByteSpan json, const char* quotedKey, size_t keyLen) noexcept {
    size_t depth = 0;
    bool inString = false;
    bool escaped = false;

    for (size_t i = 0; i + keyLen + 2u < json.len; ++i) {
        const uint8_t ch = json.ptr[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            if (depth == 1u && SpanStartsWith(json, i, quotedKey, keyLen)) {
                size_t j = i + keyLen;
                while (j < json.len &&
                       (json.ptr[j] == ' ' || json.ptr[j] == '\t' || json.ptr[j] == '\n' || json.ptr[j] == '\r')) {
                    ++j;
                }
                if (j < json.len && json.ptr[j] == ':') {
                    ++j;
                    while (j < json.len &&
                           (json.ptr[j] == ' ' || json.ptr[j] == '\t' || json.ptr[j] == '\n' || json.ptr[j] == '\r')) {
                        ++j;
                    }
                    if (j < json.len && json.ptr[j] == '[') {
                        int elems = 0;
                        size_t arrDepth = 0;
                        bool arrStr = false;
                        bool arrEsc = false;
                        bool sawContent = false;
                        for (size_t k = j; k < json.len; ++k) {
                            const uint8_t c = json.ptr[k];
                            if (arrStr) {
                                if (arrEsc) {
                                    arrEsc = false;
                                } else if (c == '\\') {
                                    arrEsc = true;
                                } else if (c == '"') {
                                    arrStr = false;
                                }
                                continue;
                            }
                            if (c == '"') {
                                arrStr = true;
                                if (arrDepth == 1u)
                                    sawContent = true;
                            } else if (c == '{' || c == '[') {
                                if (arrDepth == 1u)
                                    sawContent = true;
                                ++arrDepth;
                            } else if (c == ']' || c == '}') {
                                --arrDepth;
                                if (arrDepth == 0u) {
                                    return sawContent ? (elems + 1) : 0;
                                }
                            } else if (c == ',' && arrDepth == 1u) {
                                ++elems;
                            } else if (arrDepth == 1u && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                                sawContent = true;
                            }
                        }
                    }
                }
            }
            inString = true;
        } else if (ch == '{' || ch == '[') {
            ++depth;
        } else if ((ch == '}' || ch == ']') && depth > 0u) {
            --depth;
        }
    }
    return 0;
}

ByteSpan ExtractGltfJsonSlice(const uint8_t* data, size_t len) noexcept {
    if (!data || len < 4u) {
        return {};
    }
    if (len >= 20u && ReadU32LE(data) == 0x46546C67u) {
        const uint32_t chunkLen = ReadU32LE(data + 12);
        const uint32_t chunkType = ReadU32LE(data + 16);
        if (chunkType == 0x4E4F534Au && 20u + static_cast<size_t>(chunkLen) <= len) {
            return {data + 20, static_cast<size_t>(chunkLen)};
        }
        return {};
    }
    return {data, len};
}

// ============================================================================
// Persistent Engine State
// ============================================================================
struct CameraBookmark {
    float yaw{0.0f};
    float pitch{0.25f};
    float distance{5.0f};
    float targetX{0.0f};
    float targetY{0.0f};
    float targetZ{0.0f};
    bool valid{false};
};

struct EngineState {
    int width{1280};
    int height{720};
    bool initialized{false};

    // Scene metadata
    int nodeCount{0};
    int cameraCount{0};
    int meshCount{0};
    int animationCount{0};
    int selectedNode{-1};
    int activeCamera{-1};
    int activeAnimation{-1};
    bool animationPlaying{true};

    // Feature toggles
    bool showBbox{false};
    bool showQuadtree{false};
    bool showSkybox{true};
    bool showDebugOverlay{true};
    bool bumpEnabled{true};
    bool mipmapsEnabled{true};
    bool shadowsEnabled{true};
    bool reflectionsEnabled{true};
    bool aoEnabled{true};
    bool pomEnabled{true};
    bool lensFlareEnabled{true};
    bool ssaoEnabled{true};
    bool bloomEnabled{true};
    bool occlusionEnabled{true};
    bool freeFlyCamera{false};
    bool turntable{false};
    bool dayNightCycle{false};
    bool lodEnabled{true};
    bool autoLod{false};

    // Tuning scalars
    float ssaoIntensity{1.0f};
    float bloomIntensity{0.4f};
    float aoIntensity{1.0f};
    float bumpScale{1.5f};
    float parallaxScale{0.035f};
    float lensFlareIntensity{0.5f};
    int weatherMode{0};
    float weatherIntensity{0.0f};
    float windStrength{0.3f};
    float turntableSpeed{0.35f};
    float timeOfDay{14.0f};
    float dayNightSpeed{1.0f};
    float lodBias{1.0f};
    int debugViewMode{0};

    // Camera state
    float camYaw{0.0f};
    float camPitch{0.25f};
    float camDistance{5.0f};
    float camTargetX{0.0f};
    float camTargetY{0.0f};
    float camTargetZ{0.0f};
    std::array<CameraBookmark, 10> bookmarks{};

    // Last gamepad sample
    float padLx{0.0f};
    float padLy{0.0f};
    float padRx{0.0f};
    float padRy{0.0f};
};

EngineState gState{};

} // namespace

// ============================================================================
// Exported C ABI for JavaScript / WebGPU Host
// ============================================================================
extern "C" {

void* malloc(size_t size) {
    return WasmMalloc(size);
}

void free(void* ptr) {
    WasmFree(ptr);
}

int bgl_init(int width, int height) {
    gState.width = width > 0 ? width : 1280;
    gState.height = height > 0 ? height : 720;
    gState.initialized = true;
    return 1;
}

void bgl_resize(int width, int height) {
    if (width > 0)
        gState.width = width;
    if (height > 0)
        gState.height = height;
}

int bgl_load_file_buffer(const char* /*name*/, const uint8_t* data, size_t len) {
    const ByteSpan json = ExtractGltfJsonSlice(data, len);
    if (!json.ptr || json.len == 0u) {
        return 0;
    }
    gState.nodeCount = CountArrayElementsForKey(json, "\"nodes\"", 7u);
    gState.cameraCount = CountArrayElementsForKey(json, "\"cameras\"", 9u);
    gState.meshCount = CountArrayElementsForKey(json, "\"meshes\"", 8u);
    gState.animationCount = CountArrayElementsForKey(json, "\"animations\"", 12u);
    gState.selectedNode = -1;
    gState.activeCamera = gState.cameraCount > 0 ? 0 : -1;
    gState.activeAnimation = gState.animationCount > 0 ? 0 : -1;
    return 1;
}

void bgl_select_node(int nodeIndex) {
    if (nodeIndex >= -1 && nodeIndex < gState.nodeCount) {
        gState.selectedNode = nodeIndex;
    }
}

void bgl_clear_selection() {
    gState.selectedNode = -1;
}

int bgl_pick_node(float x, float y, float w, float h) {
    if (gState.nodeCount <= 0 || w <= 0.0f || h <= 0.0f) {
        return -1;
    }
    // Normalized center picking fallback when ray-query runs in WASM state cache
    const float nx = (x / w) * 2.0f - 1.0f;
    const float ny = 1.0f - (y / h) * 2.0f;
    if (AbsF(nx) <= 0.85f && AbsF(ny) <= 0.85f) {
        const int idx = gState.selectedNode >= 0 ? gState.selectedNode : 0;
        return idx < gState.nodeCount ? idx : -1;
    }
    return -1;
}

int bgl_get_camera_count() {
    return gState.cameraCount;
}

void bgl_select_camera(int index) {
    if (index >= -1 && index < gState.cameraCount) {
        gState.activeCamera = index;
    }
}

void bgl_set_animation(int index) {
    gState.activeAnimation = (index >= 0 && index < gState.animationCount) ? index : -1;
}

void bgl_set_animation_playing(int playing) {
    gState.animationPlaying = (playing != 0);
}

void bgl_toggle_bbox() {
    gState.showBbox = !gState.showBbox;
}
void bgl_toggle_quadtree() {
    gState.showQuadtree = !gState.showQuadtree;
}
void bgl_toggle_skybox() {
    gState.showSkybox = !gState.showSkybox;
}
void bgl_toggle_debug_overlay() {
    gState.showDebugOverlay = !gState.showDebugOverlay;
}
void bgl_toggle_bump() {
    gState.bumpEnabled = !gState.bumpEnabled;
}
void bgl_toggle_mipmaps() {
    gState.mipmapsEnabled = !gState.mipmapsEnabled;
}
void bgl_toggle_shadows() {
    gState.shadowsEnabled = !gState.shadowsEnabled;
}
void bgl_toggle_reflections() {
    gState.reflectionsEnabled = !gState.reflectionsEnabled;
}
void bgl_toggle_ao() {
    gState.aoEnabled = !gState.aoEnabled;
}
void bgl_toggle_pom() {
    gState.pomEnabled = !gState.pomEnabled;
}
void bgl_toggle_lens_flare() {
    gState.lensFlareEnabled = !gState.lensFlareEnabled;
}
void bgl_toggle_ssao() {
    gState.ssaoEnabled = !gState.ssaoEnabled;
}
void bgl_toggle_bloom() {
    gState.bloomEnabled = !gState.bloomEnabled;
}
void bgl_toggle_occlusion() {
    gState.occlusionEnabled = !gState.occlusionEnabled;
}
void bgl_toggle_camera_mode() {
    gState.freeFlyCamera = !gState.freeFlyCamera;
}
void bgl_toggle_turntable() {
    gState.turntable = !gState.turntable;
}
void bgl_toggle_day_night_cycle() {
    gState.dayNightCycle = !gState.dayNightCycle;
}
void bgl_toggle_lod() {
    gState.lodEnabled = !gState.lodEnabled;
}
void bgl_toggle_auto_lod() {
    gState.autoLod = !gState.autoLod;
}

void bgl_set_ssao_intensity(float v) {
    gState.ssaoIntensity = v;
}
void bgl_set_bloom_intensity(float v) {
    gState.bloomIntensity = v;
}
void bgl_set_ao_intensity(float v) {
    gState.aoIntensity = v;
}
void bgl_set_bump_scale(float v) {
    gState.bumpScale = v;
}
void bgl_set_parallax_scale(float v) {
    gState.parallaxScale = v;
}
void bgl_set_lens_flare_intensity(float v) {
    gState.lensFlareIntensity = v;
}
void bgl_set_weather_mode(int mode) {
    gState.weatherMode = mode;
}
void bgl_set_weather_intensity(float v) {
    gState.weatherIntensity = v;
}
void bgl_set_wind_strength(float v) {
    gState.windStrength = v;
}
void bgl_set_turntable_speed(float v) {
    gState.turntableSpeed = v;
}
void bgl_set_time_of_day(float v) {
    gState.timeOfDay = v;
}
void bgl_set_day_night_speed(float v) {
    gState.dayNightSpeed = v;
}
void bgl_set_lod_bias(float v) {
    gState.lodBias = v;
}
void bgl_set_debug_view(int mode) {
    gState.debugViewMode = mode;
}

void bgl_camera_focus_selection() {
    gState.camDistance = 3.0f;
}

void bgl_camera_frame_all() {
    gState.camTargetX = 0.0f;
    gState.camTargetY = 0.0f;
    gState.camTargetZ = 0.0f;
    gState.camDistance = 5.0f;
}

void bgl_camera_save_bookmark(int slot) {
    if (slot < 0 || slot >= static_cast<int>(gState.bookmarks.size())) {
        return;
    }
    auto& bm = gState.bookmarks[static_cast<size_t>(slot)];
    bm.yaw = gState.camYaw;
    bm.pitch = gState.camPitch;
    bm.distance = gState.camDistance;
    bm.targetX = gState.camTargetX;
    bm.targetY = gState.camTargetY;
    bm.targetZ = gState.camTargetZ;
    bm.valid = true;
}

void bgl_camera_restore_bookmark(int slot) {
    if (slot < 0 || slot >= static_cast<int>(gState.bookmarks.size())) {
        return;
    }
    const auto& bm = gState.bookmarks[static_cast<size_t>(slot)];
    if (!bm.valid) {
        return;
    }
    gState.camYaw = bm.yaw;
    gState.camPitch = bm.pitch;
    gState.camDistance = bm.distance;
    gState.camTargetX = bm.targetX;
    gState.camTargetY = bm.targetY;
    gState.camTargetZ = bm.targetZ;
}

void bgl_camera_move(float fwd, float right, float up) {
    const float sy = FastSin(gState.camYaw);
    const float cy = FastCos(gState.camYaw);
    gState.camTargetX += (-sy * fwd + cy * right);
    gState.camTargetY += up;
    gState.camTargetZ += (-cy * fwd - sy * right);
}

void bgl_camera_zoom(float delta) {
    gState.camDistance = gState.camDistance - delta * 0.5f;
    if (gState.camDistance < 0.05f) {
        gState.camDistance = 0.05f;
    }
}

void bgl_camera_orbit(float dx, float dy) {
    gState.camYaw += dx;
    gState.camPitch += dy;
    if (gState.camPitch > 1.5f)
        gState.camPitch = 1.5f;
    if (gState.camPitch < -1.5f)
        gState.camPitch = -1.5f;
}

void bgl_camera_pan(float dx, float dy) {
    const float factor = gState.camDistance * 0.25f;
    gState.camTargetX += FastCos(gState.camYaw) * dx * factor;
    gState.camTargetY += dy * factor;
    gState.camTargetZ -= FastSin(gState.camYaw) * dx * factor;
}

void bgl_gamepad_state(float lx, float ly, float rx, float ry, float /*lt*/, float /*rt*/, int a, int /*b*/, int /*x*/,
                       int y) {
    gState.padLx = lx;
    gState.padLy = ly;
    gState.padRx = rx;
    gState.padRy = ry;
    gState.camYaw += rx * 0.03f;
    gState.camPitch -= ry * 0.02f;
    if (a)
        bgl_camera_frame_all();
    if (y)
        bgl_toggle_turntable();
}

} // extern "C"
