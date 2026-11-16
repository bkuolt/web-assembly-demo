/**
 * @file ImmediateUI.cpp
 * @brief Frame lifecycle, PMR arena management, multi-font slots, hover animation evaluator,
 *        tooltip overlay, and SDF geometry builders for ImmediateUI.
 */
#include "ui/ImmediateUI.hpp"
#include "text/EmbeddedFont.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace bgl::ui {

namespace {

[[nodiscard]] constexpr glm::vec4 WithAlpha(const glm::vec4& c, float alpha) noexcept {
    return glm::vec4{c.r, c.g, c.b, alpha};
}

} // namespace

ImmediateUI::ImmediateUI()
    : arenaBacking_(kArenaBytes), frameArena_(arenaBacking_.data(), arenaBacking_.size()), vertices_(&frameArena_),
      indices_(&frameArena_), overlayVertices_(&frameArena_), overlayIndices_(&frameArena_), hitRects_(&frameArena_),
      drawCmds_(&frameArena_) {
    EnsureDefaultFontSlot();
}

ImmediateUI::~ImmediateUI() {
    Shutdown();
}

void ImmediateUI::EnsureDefaultFontSlot() {
    if (fontSlots_.empty()) {
        FontSlot slot;
        slot.name = "default";
        slot.atlas = std::make_unique<text::FontAtlas>();
        slot.basePixelSize = 24.0f;
        fontSlots_.push_back(std::move(slot));
    }
}

ImmediateUI::FontSlot& ImmediateUI::ActiveFontSlot() noexcept {
    EnsureDefaultFontSlot();
    const uint32_t idx = (activeFontSlot_ < fontSlots_.size()) ? activeFontSlot_ : 0u;
    return fontSlots_[idx];
}

const ImmediateUI::FontSlot& ImmediateUI::ActiveFontSlot() const noexcept {
    const uint32_t idx = (activeFontSlot_ < fontSlots_.size()) ? activeFontSlot_ : 0u;
    return fontSlots_[idx];
}

bool ImmediateUI::IsSdfFontReady() const noexcept {
    return !fontSlots_.empty() && fontSlots_[0].ready;
}

float ImmediateUI::EffectiveTextScale(float relativeScale) const noexcept {
    if (fontSlots_.empty()) {
        return relativeScale;
    }
    const auto& slot = ActiveFontSlot();
    const float atlasSize = slot.atlas && slot.atlas->GetPixelSize() > 0
                                ? static_cast<float>(slot.atlas->GetPixelSize())
                                : slot.basePixelSize;
    return relativeScale * (24.0f / std::max(8.0f, atlasSize));
}

uint64_t ImmediateUI::HashId(std::string_view label) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : label) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h == 0 ? 1 : h;
}

bool ImmediateUI::HasOpenModal() const noexcept {
    return immediateModalDrawnThisFrame_ || !modalQueue_.empty() || loadingState_.active;
}

void ImmediateUI::PushHoverEnabled(bool enabled) {
    hoverOverrideStack_.push_back(enabled);
}

void ImmediateUI::PopHoverEnabled() {
    if (!hoverOverrideStack_.empty()) {
        hoverOverrideStack_.pop_back();
    }
}

bool ImmediateUI::AreHoverEffectsEnabled() const noexcept {
    return hoverOverrideStack_.empty() ? style_.enableHoverEffects : hoverOverrideStack_.back();
}

ImmediateUI::HoverEval ImmediateUI::EvalHover(uint64_t id, const UiRect& rect, bool allowInteraction, bool trackHit,
                                              bool ignoreModalBlock) {
    const bool blocked = ignoreModalBlock ? popupConsumedInputThisFrame_ : (IsBlockedByPopup() || !IsMouseInClipArea());
    const bool hovered = allowInteraction && !blocked && rect.Contains(input_.mouseX, input_.mouseY);

    lastItemRect_ = rect;
    lastItemHovered_ = hovered;
    if (trackHit) {
        hitRects_.push_back(rect);
    }

    if (!AreHoverEffectsEnabled()) {
        hoverAmounts_.erase(id);
        return HoverEval{.hovered = hovered, .mix = 0.0f};
    }

    float& amount = hoverAmounts_[id];
    const float rise = std::clamp(frameDeltaTime_ * style_.hoverTransitionSpeed, 0.60f, 1.0f);
    const float fall = std::clamp(frameDeltaTime_ * style_.hoverTransitionSpeed * 0.7f, 0.25f, 1.0f);
    amount = hovered ? std::min(1.0f, amount + rise) : std::max(0.0f, amount - fall);
    return HoverEval{.hovered = hovered, .mix = amount};
}

void ImmediateUI::EmitHoverHighlight(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs,
                                     const UiRect& rect, float hoverMix, float radius) const {
    if (hoverMix <= 0.01f) {
        return;
    }
    const glm::vec4 tint = WithAlpha(style_.hoverHighlight, style_.hoverHighlight.a * hoverMix);
    const glm::vec4 rim = WithAlpha(style_.accent, 0.45f * hoverMix);
    EmitRoundedBox(verts, idxs, rect.x, rect.y, rect.w, rect.h, radius, tint, rim, 1.0f, false);
}

void ImmediateUI::EmitPanelCard(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, const UiRect& rect,
                                float radius, const glm::vec4& bg, const glm::vec4& border, bool frosted,
                                float borderWidth) const {
    EmitShadowBox(verts, idxs, rect.x, rect.y + 3.0f, rect.w, rect.h, radius, style_.shadowBlur,
                  WithAlpha(style_.shadowColor, style_.shadowColor.a * (bg.a > 0.0f ? bg.a : 1.0f)));
    EmitRoundedBox(verts, idxs, rect.x, rect.y, rect.w, rect.h, radius, bg, border, borderWidth, frosted);
}

void ImmediateUI::Tooltip(std::string_view text) {
    if (lastItemHovered_ && !text.empty()) {
        pendingTooltip_.assign(text);
    }
}

void ImmediateUI::EmitTooltipOverlay() {
    if (pendingTooltip_.empty()) {
        return;
    }
    const float scale = style_.textScale * 0.88f;
    const float padX = 10.0f;
    const float padY = 6.0f;
    const float textW = std::max(24.0f, MeasureTextWidth(pendingTooltip_, scale));
    const float tipW = std::min(textW + 2.0f * padX, std::max(120.0f, static_cast<float>(viewportWidth_) - 16.0f));
    const float tipH = 26.0f;

    float tx = input_.mouseX + 14.0f;
    float ty = input_.mouseY + 16.0f;
    if (tx + tipW + 6.0f > static_cast<float>(viewportWidth_)) {
        tx = std::max(6.0f, input_.mouseX - tipW - 10.0f);
    }
    if (ty + tipH + 6.0f > static_cast<float>(viewportHeight_)) {
        ty = std::max(6.0f, input_.mouseY - tipH - 10.0f);
    }

    EmitPanelCard(overlayVertices_, overlayIndices_, UiRect{tx, ty, tipW, tipH}, style_.widgetRadius, style_.popupBg,
                  WithAlpha(style_.accent, 0.78f), true, 1.0f);
    const std::string clipped = EllipsizeText(pendingTooltip_, tipW - 2.0f * padX, scale);
    EmitText(overlayVertices_, overlayIndices_, clipped, tx + padX, ty + padY, style_.textPrimary, scale);
}

std::vector<text::FontDescriptor> ImmediateUI::GetAvailableFonts() const {
    return text::FontLibrary::Discover();
}

core::Result<void> ImmediateUI::LoadFont(std::string_view slotName, const text::FontQuery& query, float pixelSize,
                                         bool allowEmbeddedFallback) {
    if (slotName.empty()) {
        return std::unexpected("Font slot name cannot be empty");
    }

    const uint32_t clampedPx = static_cast<uint32_t>(std::clamp(pixelSize, 8.0f, 96.0f));
    auto newAtlas = std::make_unique<text::FontAtlas>();
    if (auto res = newAtlas->InitializeWithQuery(query, clampedPx, 1024, true, allowEmbeddedFallback); !res) {
        return res;
    }

    EnsureDefaultFontSlot();
    for (auto& slot : fontSlots_) {
        if (slot.name == slotName) {
            slot.atlas = std::move(newAtlas);
            slot.basePixelSize = static_cast<float>(clampedPx);
            slot.bindGroup.Reset();
            slot.boundGeneration = 0;
            slot.ready = true;
            if (gpuInitialized_) {
                SyncFontBindGroup(slot);
            }
            return {};
        }
    }

    FontSlot slot;
    slot.name = std::string{slotName};
    slot.atlas = std::move(newAtlas);
    slot.basePixelSize = static_cast<float>(clampedPx);
    slot.ready = true;
    fontSlots_.push_back(std::move(slot));
    if (gpuInitialized_) {
        SyncFontBindGroup(fontSlots_.back());
    }
    return {};
}

core::Result<void> ImmediateUI::LoadFont(std::string_view slotName, std::string_view nameOrPath, float pixelSize) {
    return LoadFont(slotName, text::FontQuery{.nameOrPath = nameOrPath}, pixelSize, false);
}

core::Result<void> ImmediateUI::LoadFontFromMemory(std::string_view slotName, std::span<const uint8_t> fontData,
                                                   float pixelSize) {
    if (slotName.empty()) {
        return std::unexpected("Font slot name cannot be empty");
    }
    const uint32_t clampedPx = static_cast<uint32_t>(std::clamp(pixelSize, 8.0f, 96.0f));
    auto newAtlas = std::make_unique<text::FontAtlas>();
    if (auto res = newAtlas->InitializeFromMemory(fontData, clampedPx, 1024, true); !res) {
        return res;
    }

    EnsureDefaultFontSlot();
    for (auto& slot : fontSlots_) {
        if (slot.name == slotName) {
            slot.atlas = std::move(newAtlas);
            slot.basePixelSize = static_cast<float>(clampedPx);
            slot.bindGroup.Reset();
            slot.boundGeneration = 0;
            slot.ready = true;
            if (gpuInitialized_) {
                SyncFontBindGroup(slot);
            }
            return {};
        }
    }

    FontSlot slot;
    slot.name = std::string{slotName};
    slot.atlas = std::move(newAtlas);
    slot.basePixelSize = static_cast<float>(clampedPx);
    slot.ready = true;
    fontSlots_.push_back(std::move(slot));
    if (gpuInitialized_) {
        SyncFontBindGroup(fontSlots_.back());
    }
    return {};
}

bool ImmediateUI::SetDefaultFont(std::string_view nameOrPath, float pixelSize) {
    if (nameOrPath.empty() || nameOrPath == "embedded" || nameOrPath == "Embedded Liberation Mono") {
        return LoadFontFromMemory("default", text::GetEmbeddedFontData(), pixelSize).has_value();
    }
    return LoadFont("default", text::FontQuery{.nameOrPath = nameOrPath}, pixelSize, false).has_value();
}

bool ImmediateUI::PushFont(std::string_view slotName) {
    for (uint32_t i = 0; i < fontSlots_.size(); ++i) {
        if (fontSlots_[i].name == slotName && fontSlots_[i].ready) {
            fontStack_.push_back(activeFontSlot_);
            if (i != activeFontSlot_) {
                FlushDrawCommand(inScrollArea_, inScrollArea_ ? scrollRect_ : UiRect{});
                activeFontSlot_ = i;
                batchFontSlot_ = i;
            }
            return true;
        }
    }
    return false;
}

void ImmediateUI::PopFont() {
    if (fontStack_.empty()) {
        return;
    }
    const uint32_t prev = fontStack_.back();
    fontStack_.pop_back();
    if (prev != activeFontSlot_) {
        FlushDrawCommand(inScrollArea_, inScrollArea_ ? scrollRect_ : UiRect{});
        activeFontSlot_ = (prev < fontSlots_.size()) ? prev : 0u;
        batchFontSlot_ = activeFontSlot_;
    }
}

std::string_view ImmediateUI::GetActiveFontName() const noexcept {
    if (fontSlots_.empty()) {
        return "default";
    }
    return ActiveFontSlot().name;
}

void ImmediateUI::BeginFrame(const UiInputState& input, uint32_t viewportWidth, uint32_t viewportHeight,
                             float deltaTime) {
    vertices_.clear();
    indices_.clear();
    overlayVertices_.clear();
    overlayIndices_.clear();
    hitRects_.clear();
    drawCmds_.clear();
    frameArena_.release();

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

    fontStack_.clear();
    activeFontSlot_ = 0;
    batchFontSlot_ = 0;
    hoverOverrideStack_.clear();

    input_ = input;
    viewportWidth_ = std::max(1u, viewportWidth);
    viewportHeight_ = std::max(1u, viewportHeight);
    frameDeltaTime_ = std::clamp(deltaTime, 0.0005f, 0.25f);
    timeSec_ += frameDeltaTime_;

    inWindow_ = false;
    inScrollArea_ = false;
    hasPendingOverlay_ = false;
    hasPendingColorOverlay_ = false;
    dropdownToggledThisFrame_ = false;
    colorToggledThisFrame_ = false;
    popupConsumedInputThisFrame_ = false;
    modalDrawnThisFrame_ = false;
    immediateModalDrawnThisFrame_ = false;
    lastItemHovered_ = false;
    lastItemRect_ = UiRect{};
    pendingTooltip_.clear();
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
                                    .fontSlot = batchFontSlot_,
                                    .hasScissor = hasScissor,
                                    .scissorRect = scissorRect});
    batchStartIndex_ = currentIdxCount;
}

void ImmediateUI::EndFrame() {
    if (inWindow_) {
        EndWindow();
    }
    while (!fontStack_.empty()) {
        PopFont();
    }
    if (activeFontSlot_ != 0) {
        FlushDrawCommand(false, UiRect{});
        activeFontSlot_ = 0;
        batchFontSlot_ = 0;
    }

    if (loadingState_.active) {
        DrawLoadingScreenInternal(loadingState_.title, loadingState_.fraction, loadingState_.status,
                                  loadingState_.isError, false, 1.0f);
    }

    if (!modalQueue_.empty()) {
        QueuedMessageBox modal = modalQueue_.front();
        std::vector<std::string_view> btnViews;
        btnViews.reserve(modal.buttons.size());
        for (const auto& b : modal.buttons) {
            btnViews.push_back(b);
        }
        const int clicked =
            DrawMessageBoxInternal(modal.idKey, modal.title, modal.message, btnViews, modal.type, modal.dimBackground);
        if (clicked >= 0) {
            auto cb = std::move(modal.callback);
            modalQueue_.erase(modalQueue_.begin());
            if (cb) {
                cb(clicked);
            }
        }
    }

    EmitTooltipOverlay();

    // Merge floating badges, tooltips, loading screens & modals BEFORE dropdown/color overlays so popups stack crisp
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

    wantsMouse_ = modalDrawnThisFrame_ || (openDropdownId_ != 0) || (openColorId_ != 0) || (activeDragWindowId_ != 0) ||
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
    const auto& slot = ActiveFontSlot();
    const glm::vec2 whiteUV = (slot.ready && slot.atlas) ? slot.atlas->GetWhitePixelUV() : glm::vec2{0.0f, 0.0f};
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
    auto& slot = ActiveFontSlot();
    if (!slot.ready || !slot.atlas) {
        return static_cast<float>(text.size()) * 9.0f * scale;
    }
    return text::TextShaper::Measure(*slot.atlas, text, EffectiveTextScale(scale)).x;
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
    auto& slot = ActiveFontSlot();
    if (!slot.ready || !slot.atlas || text.empty()) {
        return;
    }
    const text::TextLayout layout = text::TextShaper::Shape(*slot.atlas, text, x, y, color, EffectiveTextScale(scale));
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
