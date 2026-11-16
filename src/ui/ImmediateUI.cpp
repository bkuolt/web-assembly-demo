/**
 * @file ImmediateUI.cpp
 * @brief Frame lifecycle, PMR arena management, SDF geometry builders, and color/text helpers for ImmediateUI.
 */
#include "ui/ImmediateUI.hpp"
#include <algorithm>
#include <cmath>

namespace bgl::ui {

ImmediateUI::ImmediateUI()
    : arenaBacking_(kArenaBytes), frameArena_(arenaBacking_.data(), arenaBacking_.size()), vertices_(&frameArena_),
      indices_(&frameArena_), overlayVertices_(&frameArena_), overlayIndices_(&frameArena_), hitRects_(&frameArena_),
      drawCmds_(&frameArena_) {}

ImmediateUI::~ImmediateUI() {
    Shutdown();
}

uint64_t ImmediateUI::HashId(std::string_view label) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : label) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h == 0 ? 1 : h;
}

void ImmediateUI::BeginFrame(const UiInputState& input, uint32_t viewportWidth, uint32_t viewportHeight) {
    // Release PMR monotonic buffer back to 0 offset in O(1) without freeing backing storage
    vertices_.clear();
    indices_.clear();
    overlayVertices_.clear();
    overlayIndices_.clear();
    hitRects_.clear();
    frameArena_.release();

    // Rebind pmr vectors after arena release and pre-reserve inside the frame arena
    vertices_ = std::pmr::vector<UiVertex>{&frameArena_};
    indices_ = std::pmr::vector<uint32_t>{&frameArena_};
    overlayVertices_ = std::pmr::vector<UiVertex>{&frameArena_};
    overlayIndices_ = std::pmr::vector<uint32_t>{&frameArena_};
    hitRects_ = std::pmr::vector<UiRect>{&frameArena_};
    drawCmds_ = std::pmr::vector<DrawCommand>{&frameArena_};
    vertices_.reserve(512);
    indices_.reserve(1024);
    hitRects_.reserve(64);
    drawCmds_.reserve(8);
    batchStartIndex_ = 0;

    input_ = input;
    viewportWidth_ = std::max(1u, viewportWidth);
    viewportHeight_ = std::max(1u, viewportHeight);
    inWindow_ = false;
    inScrollArea_ = false;
    hasPendingOverlay_ = false;
    hasPendingColorOverlay_ = false;
    dropdownToggledThisFrame_ = false;
    colorToggledThisFrame_ = false;
    popupConsumedInputThisFrame_ = false;
    wantsMouse_ = false;

    if (!input_.mouseDown) {
        activeDragWindowId_ = 0;
        activeSliderId_ = 0;
        activeScrollbarId_ = 0;
        activeDropdownScrollId_ = 0;
        activeColorSvId_ = 0;
        activeColorHueId_ = 0;
    }

    UI::SetContext(this);
}

void ImmediateUI::FlushDrawCommand(bool hasScissor, const UiRect& scissorRect) {
    const uint32_t currentIdxCount = static_cast<uint32_t>(indices_.size());
    if (currentIdxCount <= batchStartIndex_) {
        return;
    }
    drawCmds_.push_back(DrawCommand{.firstIndex = batchStartIndex_,
                                    .indexCount = currentIdxCount - batchStartIndex_,
                                    .hasScissor = hasScissor,
                                    .scissorRect = scissorRect});
    batchStartIndex_ = currentIdxCount;
}

void ImmediateUI::EndFrame() {
    if (inWindow_) {
        EndWindow();
    }

    // Merge floating badges & loading banner into main vertex stream BEFORE popups so popups stack above
    if (!overlayVertices_.empty() && !overlayIndices_.empty()) {
        const uint32_t baseVertex = static_cast<uint32_t>(vertices_.size());
        vertices_.insert(vertices_.end(), overlayVertices_.begin(), overlayVertices_.end());
        for (uint32_t idx : overlayIndices_) {
            indices_.push_back(baseVertex + idx);
        }
    }

    EmitColorPickerOverlay();
    EmitDropdownOverlay();

    FlushDrawCommand(false, UiRect{});

    wantsMouse_ = (openDropdownId_ != 0) || (openColorId_ != 0) || (activeDragWindowId_ != 0) ||
                  (activeSliderId_ != 0) || (activeScrollbarId_ != 0) || (activeDropdownScrollId_ != 0);
    if (!wantsMouse_) {
        for (const auto& r : hitRects_) {
            if (r.Contains(input_.mouseX, input_.mouseY)) {
                wantsMouse_ = true;
                break;
            }
        }
    }
    UI::SetContext(nullptr);
}

void ImmediateUI::EmitQuad(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                           float w, float h, const glm::vec2& uvMin, const glm::vec2& uvMax, const glm::vec4& color,
                           float isSdf, const glm::vec4& sdfRect, const glm::vec4& auxColor) const {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    verts.push_back(UiVertex{{x, y}, {uvMin.x, uvMin.y}, color, sdfRect, auxColor, isSdf});
    verts.push_back(UiVertex{{x + w, y}, {uvMax.x, uvMin.y}, color, sdfRect, auxColor, isSdf});
    verts.push_back(UiVertex{{x + w, y + h}, {uvMax.x, uvMax.y}, color, sdfRect, auxColor, isSdf});
    verts.push_back(UiVertex{{x, y + h}, {uvMin.x, uvMax.y}, color, sdfRect, auxColor, isSdf});

    idxs.push_back(base + 0);
    idxs.push_back(base + 1);
    idxs.push_back(base + 2);
    idxs.push_back(base + 0);
    idxs.push_back(base + 2);
    idxs.push_back(base + 3);
}

void ImmediateUI::EmitRect(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                           float w, float h, const glm::vec4& color) const {
    const glm::vec2 whiteUV = fontReady_ ? fontAtlas_.GetWhitePixelUV() : glm::vec2{0.0f, 0.0f};
    EmitQuad(verts, idxs, x, y, w, h, whiteUV, whiteUV, color, 0.0f);
}

void ImmediateUI::EmitRectWithBorder(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x,
                                     float y, float w, float h, const glm::vec4& fill, const glm::vec4& border,
                                     float borderWidth) const {
    EmitRoundedBox(verts, idxs, x, y, w, h, 0.0f, fill, border, borderWidth, false);
}

void ImmediateUI::EmitRoundedBox(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                                 float w, float h, float radius, const glm::vec4& fill, const glm::vec4& border,
                                 float borderWidth, bool frostedGlass) const {
    const float hw = std::max(0.5f, w * 0.5f);
    const float hh = std::max(0.5f, h * 0.5f);
    const float pad = 1.5f;
    const glm::vec4 sdfRect{hw, hh, radius, borderWidth};
    EmitQuad(verts, idxs, x - pad, y - pad, w + 2.0f * pad, h + 2.0f * pad, {-hw - pad, -hh - pad},
             {hw + pad, hh + pad}, fill, frostedGlass ? 4.0f : 2.0f, sdfRect, border);
}

void ImmediateUI::EmitShadowBox(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                                float w, float h, float radius, float blurRadius, const glm::vec4& shadowColor) const {
    const float hw = std::max(0.5f, w * 0.5f);
    const float hh = std::max(0.5f, h * 0.5f);
    const float pad = std::max(2.0f, blurRadius);
    const glm::vec4 sdfRect{hw, hh, radius, pad};
    EmitQuad(verts, idxs, x - pad, y - pad, w + 2.0f * pad, h + 2.0f * pad, {-hw - pad, -hh - pad},
             {hw + pad, hh + pad}, shadowColor, 3.0f, sdfRect, glm::vec4{0.0f});
}

void ImmediateUI::EmitLineSegment(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs,
                                  const glm::vec2& a, const glm::vec2& b, float thickness,
                                  const glm::vec4& color) const {
    const glm::vec2 delta = b - a;
    const float len = glm::length(delta);
    if (len < 1e-4f) {
        return;
    }
    const glm::vec2 dir = delta / len;
    const glm::vec2 perp{-dir.y, dir.x};
    const glm::vec2 offset = perp * (thickness * 0.5f);

    const uint32_t base = static_cast<uint32_t>(verts.size());
    verts.push_back(UiVertex{a - offset, {0.0f, 0.0f}, color, glm::vec4{0.0f}, glm::vec4{0.0f}, 0.0f});
    verts.push_back(UiVertex{b - offset, {1.0f, 0.0f}, color, glm::vec4{0.0f}, glm::vec4{0.0f}, 0.0f});
    verts.push_back(UiVertex{b + offset, {1.0f, 1.0f}, color, glm::vec4{0.0f}, glm::vec4{0.0f}, 0.0f});
    verts.push_back(UiVertex{a + offset, {0.0f, 1.0f}, color, glm::vec4{0.0f}, glm::vec4{0.0f}, 0.0f});
    idxs.push_back(base + 0);
    idxs.push_back(base + 1);
    idxs.push_back(base + 2);
    idxs.push_back(base + 0);
    idxs.push_back(base + 2);
    idxs.push_back(base + 3);
}

float ImmediateUI::MeasureTextWidth(std::string_view text, float scale) {
    if (text.empty()) {
        return 0.0f;
    }
    if (!fontReady_) {
        return static_cast<float>(text.size()) * 9.0f * scale;
    }
    return text::TextShaper::Shape(fontAtlas_, text, 0.0f, 0.0f, style_.textPrimary, scale).bounds.x;
}

std::string ImmediateUI::EllipsizeText(std::string_view text, float maxWidth, float scale) {
    if (text.empty() || maxWidth <= 8.0f || MeasureTextWidth(text, scale) <= maxWidth) {
        return std::string{text};
    }
    size_t lo = 0;
    size_t hi = text.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo + 1) / 2;
        std::string probe{text.substr(0, mid)};
        probe.append("...");
        if (MeasureTextWidth(probe, scale) <= maxWidth) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    while (lo > 0 && (static_cast<unsigned char>(text[lo]) & 0xC0u) == 0x80u) {
        --lo;
    }
    if (lo == 0) {
        return "...";
    }
    std::string out{text.substr(0, lo)};
    out.append("...");
    return out;
}

void ImmediateUI::EmitText(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, std::string_view text,
                           float x, float y, const glm::vec4& color, float scale) {
    if (!fontReady_ || text.empty()) {
        return;
    }
    const text::TextLayout layout = text::TextShaper::Shape(fontAtlas_, text, x, y, color, scale);
    for (const auto& q : layout.quads) {
        EmitQuad(verts, idxs, q.posMin.x, q.posMin.y, q.posMax.x - q.posMin.x, q.posMax.y - q.posMin.y, q.uvMin,
                 q.uvMax, q.color, 1.0f);
    }
}

glm::vec3 ImmediateUI::RgbToHsv(const glm::vec3& rgb) noexcept {
    const float r = std::clamp(rgb.r, 0.0f, 1.0f);
    const float g = std::clamp(rgb.g, 0.0f, 1.0f);
    const float b = std::clamp(rgb.b, 0.0f, 1.0f);
    const float maxC = std::max({r, g, b});
    const float minC = std::min({r, g, b});
    const float d = maxC - minC;

    float h = 0.0f;
    if (d > 1e-5f) {
        if (maxC == r) {
            h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        } else if (maxC == g) {
            h = (b - r) / d + 2.0f;
        } else {
            h = (r - g) / d + 4.0f;
        }
        h /= 6.0f;
    }
    const float s = (maxC > 1e-5f) ? (d / maxC) : 0.0f;
    return {h, s, maxC};
}

glm::vec3 ImmediateUI::HsvToRgb(const glm::vec3& hsv) noexcept {
    const float h = hsv.x - std::floor(hsv.x);
    const float s = std::clamp(hsv.y, 0.0f, 1.0f);
    const float v = std::clamp(hsv.z, 0.0f, 1.0f);
    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(i) % 6) {
    case 0:
        return {v, t, p};
    case 1:
        return {q, v, p};
    case 2:
        return {p, v, t};
    case 3:
        return {p, q, v};
    case 4:
        return {t, p, v};
    default:
        return {v, p, q};
    }
}

} // namespace bgl::ui
