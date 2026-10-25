/**
 * @file TextRenderer.hpp
 * @brief WebGPU 2D Batched Quad and Text Renderer with alpha blending and 4x MSAA support.
 */
#pragma once

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include "text/FontAtlas.hpp"
#include "text/TextShaper.hpp"
#include <glm/glm.hpp>
#include <span>
#include <string_view>
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::text {

/**
 * @struct TextVertex
 * @brief 2D vertex layout for batched text and UI quads.
 */
struct TextVertex {
    glm::vec2 pos{0.0f};   ///< Location 0: Screen position (X, Y in pixels)
    glm::vec2 uv{0.0f};    ///< Location 1: UV coordinates in Font Atlas
    glm::vec4 color{1.0f}; ///< Location 2: RGBA color tint
};

/**
 * @class TextRenderer
 * @brief Batched WebGPU 2D text and UI renderer with orthographic projection.
 */
class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    /**
     * @brief Initializes WebGPU shaders, bind group layouts, and render pipeline.
     * @param device WebGPU device handle.
     * @param queue WebGPU queue handle.
     * @param surfaceFormat Presentation surface texture format.
     * @param sampleCount Multisample count for the presentation pass.
     * @param fontAtlas Reference to active font atlas.
     * @return Result indicating success or error string.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat,
                                  uint32_t sampleCount, FontAtlas& fontAtlas);

    /**
     * @brief Enqueues a solid or translucent rectangle.
     * @param x Top-left X coordinate in pixels.
     * @param y Top-left Y coordinate in pixels.
     * @param width Width in pixels.
     * @param height Height in pixels.
     * @param color RGBA color vector.
     */
    void DrawRect(float x, float y, float width, float height, const glm::vec4& color);

    /**
     * @brief Enqueues a framed rectangle with outline border.
     * @param x Top-left X coordinate in pixels.
     * @param y Top-left Y coordinate in pixels.
     * @param width Width in pixels.
     * @param height Height in pixels.
     * @param fillColor Interior RGBA color vector.
     * @param borderColor Outline border RGBA color vector.
     * @param borderWidth Border line width in pixels.
     */
    void DrawRectWithBorder(float x, float y, float width, float height, const glm::vec4& fillColor,
                            const glm::vec4& borderColor, float borderWidth = 1.0f);

    /**
     * @brief Enqueues a span of pre-shaped text quads into the current vertex/index batch.
     * @param quads Span of shaped text quads with positions, UVs, and colors.
     */
    void DrawQuads(std::span<const TextQuad> quads);

    /**
     * @brief Enqueues shaped text using HarfBuzz and FreeType font atlas.
     * @param text UTF-8 text string to shape and enqueue.
     * @param x Top-left baseline X coordinate in pixels.
     * @param y Top-left baseline Y coordinate in pixels.
     * @param color Text RGBA color vector.
     * @param scale Uniform scale factor (default 1.0).
     */
    void DrawText(std::string_view text, float x, float y, const glm::vec4& color = glm::vec4{1.0f},
                  float scale = 1.0f);

    /**
     * @brief Flushes batched quads and renders them in the current render pass encoder.
     * @param pass Active render pass encoder targeting the presentation surface.
     * @param viewportWidth Target surface width in pixels.
     * @param viewportHeight Target surface height in pixels.
     */
    void Render(WGPURenderPassEncoder pass, uint32_t viewportWidth, uint32_t viewportHeight);

    [[nodiscard]] FontAtlas& GetFontAtlas() noexcept { return *fontAtlas_; }

private:
    void EnsureBuffers(size_t vertexCount, size_t indexCount);

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat surfaceFormat_{WGPUTextureFormat_BGRA8Unorm};
    uint32_t sampleCount_{4};
    FontAtlas* fontAtlas_{nullptr};

    gpu::BindGroupLayoutHandle bindGroupLayout_{};
    gpu::RenderPipelineHandle pipeline_{};
    gpu::BufferHandle uniformBuffer_{};
    gpu::BindGroupHandle bindGroup_{};

    gpu::BufferHandle vertexBuffer_{};
    gpu::BufferHandle indexBuffer_{};
    size_t vertexBufferCapacity_{0};
    size_t indexBufferCapacity_{0};

    std::vector<TextVertex> vertexBatch_{};
    std::vector<uint32_t> indexBatch_{};
};

} // namespace bgl::text
