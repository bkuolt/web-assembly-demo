/**
 * @file ImmediateUILayout.cpp
 * @brief Layout containers, panels, visual groups, and toggle/option widgets for ImmediateUI.
 */
#include "ui/ImmediateUI.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace bgl::ui {

namespace {

[[nodiscard]] constexpr glm::vec4 WithAlpha(const glm::vec4& c, float alpha) noexcept {
    return glm::vec4{c.r, c.g, c.b, alpha};
}

} // namespace

void ImmediateUI::PatchBoxGeometry(size_t vOff, float rx, float ry, float rw, float rh, float pad, float radius,
                                   float param4, const glm::vec4* fillOverride, const glm::vec4* borderOverride) {
    if (vOff + 4 > vertices_.size()) {
        return;
    }
    const float hw = std::max(0.5f, rw * 0.5f);
    const float hh = std::max(0.5f, rh * 0.5f);
    const glm::vec4 sdfVec{hw, hh, radius, param4};
    vertices_[vOff + 0].pos = {rx - pad, ry - pad};
    vertices_[vOff + 1].pos = {rx + rw + pad, ry - pad};
    vertices_[vOff + 2].pos = {rx + rw + pad, ry + rh + pad};
    vertices_[vOff + 3].pos = {rx - pad, ry + rh + pad};
    vertices_[vOff + 0].uv = {-hw - pad, -hh - pad};
    vertices_[vOff + 1].uv = {hw + pad, -hh - pad};
    vertices_[vOff + 2].uv = {hw + pad, hh + pad};
    vertices_[vOff + 3].uv = {-hw - pad, hh + pad};
    for (size_t i = 0; i < 4; ++i) {
        vertices_[vOff + i].sdfRect = sdfVec;
        if (fillOverride) {
            vertices_[vOff + i].color = *fillOverride;
        }
        if (borderOverride) {
            vertices_[vOff + i].auxColor = *borderOverride;
        }
    }
}

void ImmediateUI::AdvanceCursor(float itemHeight) {
    if (containerStack_.empty()) {
        cursorY_ += itemHeight;
        return;
    }

    ActiveContainer& c = containerStack_.back();
    c.maxExtentY = std::max(c.maxExtentY, cursorY_ + itemHeight);
    ++c.itemCount;

    if (c.options.layout == ContainerLayout::Vertical) {
        cursorY_ += itemHeight;
        c.startContentY = cursorY_;
        return;
    }

    const float pad = c.options.padding;
    const float innerW = std::max(24.0f, c.boxRect.w - 2.0f * pad);
    const uint32_t cols = (c.options.columns > 0) ? c.options.columns : 2u;
    const float slotW =
        (c.options.itemWidth > 0.0f)
            ? c.options.itemWidth
            : std::max(24.0f, (innerW - static_cast<float>(cols - 1) * c.options.spacing) / static_cast<float>(cols));

    const bool canContinueRow =
        (c.options.columns > 0) ? ((c.itemCount % cols) != 0u)
                                : ((cursorX_ + slotW + c.options.spacing + slotW) <= (c.contentStartX + innerW + 0.5f));

    if (canContinueRow) {
        cursorX_ += slotW + c.options.spacing;
        cursorY_ = c.startContentY;
        contentWidth_ = slotW;
    } else {
        cursorX_ = c.contentStartX;
        c.startContentY = c.maxExtentY;
        cursorY_ = c.startContentY;
        contentWidth_ = slotW;
    }
}

void ImmediateUI::BeginContainer(std::string_view id, const ContainerOptions& options) {
    ActiveContainer c;
    c.id = HashId(id.empty() ? "Container" : id) ^ 0x434E54ULL;
    c.options = options;
    c.savedCursorX = cursorX_;
    c.savedCursorY = cursorY_;
    c.savedContentWidth = contentWidth_;

    const float outerW = (options.width > 0.0f) ? options.width : contentWidth_;
    c.boxRect = UiRect{cursorX_, cursorY_, outerW, std::max(24.0f, options.height)};
    c.hasBackground = options.showBackground || options.showBorder || options.hoverable;
    if (c.hasBackground) {
        const float radius = (options.cornerRadius >= 0.0f) ? options.cornerRadius : style_.widgetRadius;
        const glm::vec4 bg = (options.backgroundColor.a > 0.001f)
                                 ? options.backgroundColor
                                 : (options.showBackground ? style_.groupBg : glm::vec4{0.0f});
        const glm::vec4 border = (options.borderColor.a > 0.001f)
                                     ? options.borderColor
                                     : (options.showBorder ? style_.windowBorder : glm::vec4{0.0f});
        c.bgVertexOffset = vertices_.size();
        EmitRoundedBox(vertices_, indices_, c.boxRect.x, c.boxRect.y, c.boxRect.w, c.boxRect.h, radius, bg, border,
                       options.showBorder ? 1.0f : 0.0f, false);
    }

    const float pad = options.padding;
    const float innerW = std::max(24.0f, outerW - 2.0f * pad);
    c.contentStartX = c.boxRect.x + pad;
    c.contentStartY = c.boxRect.y + pad;
    c.startContentY = c.contentStartY;
    c.maxExtentY = c.contentStartY;

    cursorX_ = c.contentStartX;
    cursorY_ = c.contentStartY;
    if (options.layout == ContainerLayout::Horizontal) {
        const uint32_t cols = (options.columns > 0) ? options.columns : 2u;
        contentWidth_ =
            (options.itemWidth > 0.0f)
                ? options.itemWidth
                : std::max(24.0f, (innerW - static_cast<float>(cols - 1) * options.spacing) / static_cast<float>(cols));
    } else {
        contentWidth_ = innerW;
    }

    containerStack_.push_back(c);
}

void ImmediateUI::EndContainer() {
    if (containerStack_.empty()) {
        return;
    }

    ActiveContainer c = containerStack_.back();
    containerStack_.pop_back();

    const float pad = c.options.padding;
    const float bottom = (c.itemCount > 0) ? (c.maxExtentY - style_.itemSpacing) : c.contentStartY;
    const float finalH = std::max({c.options.height, 2.0f * pad + 4.0f, (bottom - c.boxRect.y) + pad});
    c.boxRect.h = finalH;

    cursorX_ = c.savedCursorX;
    cursorY_ = c.savedCursorY;
    contentWidth_ = c.savedContentWidth;

    if (c.hasBackground) {
        const HoverEval hv = EvalHover(c.id, c.boxRect, c.options.hoverable, false);
        const float glow = std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f);
        const glm::vec4 baseBg = (c.options.backgroundColor.a > 0.001f)
                                     ? c.options.backgroundColor
                                     : (c.options.showBackground ? style_.groupBg : glm::vec4{0.0f});
        const glm::vec4 baseBorder = (c.options.borderColor.a > 0.001f)
                                         ? c.options.borderColor
                                         : (c.options.showBorder ? style_.windowBorder : glm::vec4{0.0f});
        const glm::vec4 fill =
            (glow > 0.01f && c.options.showBackground) ? glm::mix(baseBg, style_.widgetBg, glow * 0.45f) : baseBg;
        const glm::vec4 border = (glow > 0.01f) ? glm::mix(baseBorder, style_.hoverBorder, glow) : baseBorder;
        const float borderW = (c.options.showBorder || glow > 0.01f) ? (1.0f + 0.35f * glow) : 0.0f;
        const float radius = (c.options.cornerRadius >= 0.0f) ? c.options.cornerRadius : style_.widgetRadius;
        PatchBoxGeometry(c.bgVertexOffset, c.boxRect.x, c.boxRect.y, c.boxRect.w, c.boxRect.h, 1.5f, radius, borderW,
                         &fill, &border);
    }

    lastItemRect_ = c.boxRect;
    const float parentSpacing = containerStack_.empty() ? style_.itemSpacing : containerStack_.back().options.spacing;
    AdvanceCursor(finalH + parentSpacing);
}

bool ImmediateUI::PushPanelInternal(std::string_view idKey, float x, float y, float width, float height,
                                    std::string_view title, const PanelOptions& options, bool standalone) {
    ActivePanel p;
    p.id = HashId(idKey.empty() ? (title.empty() ? "Panel" : title) : idKey) ^ 0x504E4CULL;
    p.options = options;
    p.standalone = standalone;
    p.savedCursorX = cursorX_;
    p.savedCursorY = cursorY_;
    p.savedContentWidth = contentWidth_;
    p.boxRect = UiRect{x, y, width, std::max(height, style_.headerHeight + options.padding)};
    p.minHeight = height;

    const float radius = (options.cornerRadius >= 0.0f) ? options.cornerRadius : style_.cornerRadius;
    const glm::vec4 bg = (options.backgroundColor.a > 0.001f) ? options.backgroundColor : style_.panelBg;
    const glm::vec4 border = (options.borderColor.a > 0.001f) ? options.borderColor : style_.windowBorder;

    if (options.showShadow) {
        p.shadowVertexOffset = vertices_.size();
        EmitShadowBox(vertices_, indices_, p.boxRect.x, p.boxRect.y + 3.0f, p.boxRect.w, p.boxRect.h, radius,
                      style_.shadowBlur, style_.shadowColor);
    }
    p.bgVertexOffset = vertices_.size();
    EmitRoundedBox(vertices_, indices_, p.boxRect.x, p.boxRect.y, p.boxRect.w, p.boxRect.h, radius, bg, border, 1.0f,
                   options.frosted);

    bool isOpen = true;
    if (!title.empty()) {
        const UiRect hdrRect{x + 1.0f, y + 1.0f, std::max(2.0f, width - 2.0f), style_.headerHeight - 1.0f};
        const HoverEval hdrHv = EvalHover(p.id ^ 0x484452ULL, hdrRect, options.collapsible, false);
        const glm::vec4 hdrFill =
            options.collapsible ? glm::mix(style_.headerBg, style_.widgetHover, hdrHv.mix * 0.65f) : style_.headerBg;
        EmitRoundedBox(vertices_, indices_, hdrRect.x, hdrRect.y, hdrRect.w, hdrRect.h, std::max(1.0f, radius - 1.0f),
                       hdrFill, glm::vec4{0.0f}, 0.0f, false);
        EmitRoundedBox(vertices_, indices_, x + 6.0f, y + 7.0f, 3.0f, style_.headerHeight - 14.0f, 1.5f, style_.accent,
                       glm::vec4{0.0f}, 0.0f, false);

        if (options.collapsible) {
            bool& persistedOpen = collapsingHeaders_.try_emplace(p.id, options.defaultOpen).first->second;
            if (hdrHv.hovered && input_.mouseReleased && activeDragWindowId_ == 0) {
                persistedOpen = !persistedOpen;
            }
            isOpen = persistedOpen;
            EmitText(vertices_, indices_, isOpen ? "v" : ">", x + 14.0f, y + 6.0f,
                     glm::mix(style_.textMuted, style_.accent, hdrHv.mix), style_.textScale * 0.9f);
        }

        const float titleX = x + (options.collapsible ? 28.0f : 15.0f);
        const std::string titleClipped =
            EllipsizeText(title, std::max(24.0f, width - (titleX - x) - 10.0f), style_.textScale);
        EmitText(vertices_, indices_, titleClipped, titleX, y + 6.0f, style_.textPrimary, style_.textScale);
        cursorY_ = y + style_.headerHeight + options.padding;
    } else {
        cursorY_ = y + options.padding;
    }

    p.open = isOpen;
    cursorX_ = x + options.padding;
    contentWidth_ = std::max(24.0f, width - 2.0f * options.padding);
    panelStack_.push_back(p);
    return isOpen;
}

bool ImmediateUI::BeginPanel(std::string_view title, const PanelOptions& options) {
    return PushPanelInternal(title, cursorX_, cursorY_, contentWidth_, 0.0f, title, options, false);
}

bool ImmediateUI::BeginPanel(std::string_view id, float x, float y, float width, float height, std::string_view title,
                             const PanelOptions& options) {
    return PushPanelInternal(id, x, y, width, height, title, options, true);
}

void ImmediateUI::EndPanel() {
    if (panelStack_.empty()) {
        return;
    }

    ActivePanel p = panelStack_.back();
    panelStack_.pop_back();

    const float pad = p.options.padding;
    const float finalH =
        p.open ? std::max({p.minHeight, style_.headerHeight, (cursorY_ - p.boxRect.y) + pad - style_.itemSpacing})
               : style_.headerHeight;
    p.boxRect.h = finalH;

    cursorX_ = p.savedCursorX;
    cursorY_ = p.savedCursorY;
    contentWidth_ = p.savedContentWidth;

    const HoverEval hv = EvalHover(p.id ^ 0x504E4C48ULL, p.boxRect, p.options.hoverable, p.standalone);
    const float glow = std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f);
    const float radius = (p.options.cornerRadius >= 0.0f) ? p.options.cornerRadius : style_.cornerRadius;
    const glm::vec4 baseBorder = (p.options.borderColor.a > 0.001f) ? p.options.borderColor : style_.windowBorder;
    const glm::vec4 border = (glow > 0.01f) ? glm::mix(baseBorder, style_.hoverBorder, glow) : baseBorder;

    if (p.options.showShadow) {
        PatchBoxGeometry(p.shadowVertexOffset, p.boxRect.x, p.boxRect.y + 3.0f, p.boxRect.w, p.boxRect.h,
                         style_.shadowBlur, radius, style_.shadowBlur);
    }
    PatchBoxGeometry(p.bgVertexOffset, p.boxRect.x, p.boxRect.y, p.boxRect.w, p.boxRect.h, 1.5f, radius,
                     1.0f + 0.35f * glow, nullptr, &border);

    lastItemRect_ = p.boxRect;
    if (p.standalone) {
        lastCompletedWindowRect_ = p.boxRect;
    } else {
        AdvanceCursor(finalH + style_.itemSpacing);
    }
}

void ImmediateUI::BeginGroup(std::string_view label, bool border) {
    ActiveGroup g;
    g.id = HashId(label.empty() ? "Group" : label) ^ 0x475250ULL;
    g.hasBorder = border;
    g.savedCursorX = cursorX_;
    g.savedCursorY = cursorY_;
    g.savedContentWidth = contentWidth_;
    g.boxRect = UiRect{cursorX_, cursorY_, contentWidth_, 32.0f};
    g.bgVertexOffset = vertices_.size();

    EmitRoundedBox(vertices_, indices_, g.boxRect.x, g.boxRect.y, g.boxRect.w, g.boxRect.h, style_.widgetRadius,
                   style_.groupBg, border ? style_.windowBorder : glm::vec4{0.0f}, border ? 1.0f : 0.0f, false);

    constexpr float kPad = 8.0f;
    if (!label.empty()) {
        const std::string titleClipped =
            EllipsizeText(label, std::max(24.0f, g.boxRect.w - 2.0f * kPad), style_.textScale * 0.9f);
        EmitText(vertices_, indices_, titleClipped, g.boxRect.x + kPad, g.boxRect.y + 5.0f, style_.accent,
                 style_.textScale * 0.9f);
        cursorY_ = g.boxRect.y + 24.0f;
    } else {
        cursorY_ = g.boxRect.y + kPad;
    }

    cursorX_ = g.boxRect.x + kPad;
    contentWidth_ = std::max(24.0f, g.boxRect.w - 2.0f * kPad);
    groupStack_.push_back(g);
}

void ImmediateUI::EndGroup() {
    if (groupStack_.empty()) {
        return;
    }

    ActiveGroup g = groupStack_.back();
    groupStack_.pop_back();

    const float finalH = std::max(26.0f, (cursorY_ - g.boxRect.y) + 8.0f - style_.itemSpacing);
    g.boxRect.h = finalH;

    cursorX_ = g.savedCursorX;
    cursorY_ = g.savedCursorY;
    contentWidth_ = g.savedContentWidth;

    const HoverEval hv = EvalHover(g.id, g.boxRect, true, false);
    const float glow = std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f);
    const glm::vec4 baseBorder = g.hasBorder ? style_.windowBorder : glm::vec4{0.0f};
    const glm::vec4 border = (glow > 0.01f) ? glm::mix(baseBorder, style_.hoverBorder, glow) : baseBorder;
    const float borderW = (g.hasBorder || glow > 0.01f) ? (1.0f + 0.30f * glow) : 0.0f;
    PatchBoxGeometry(g.bgVertexOffset, g.boxRect.x, g.boxRect.y, g.boxRect.w, g.boxRect.h, 1.5f, style_.widgetRadius,
                     borderW, nullptr, &border);

    lastItemRect_ = g.boxRect;
    AdvanceCursor(finalH + style_.itemSpacing);
}

bool ImmediateUI::ToggleBox(std::string_view label, bool& state, std::string_view description) {
    const bool hasDesc = !description.empty();
    const float cardH = hasDesc ? (style_.rowHeight + 22.0f) : (style_.rowHeight + 8.0f);
    const UiRect rect{cursorX_, cursorY_, contentWidth_, cardH};
    const uint64_t id = HashId(label) ^ 0x54474CULL;
    const HoverEval hv = EvalHover(id, rect);
    const bool clicked = hv.hovered && input_.mouseReleased && activeDragWindowId_ == 0;
    if (clicked) {
        state = !state;
    }

    float& anim = toggleAmounts_.try_emplace(id, state ? 1.0f : 0.0f).first->second;
    const float target = state ? 1.0f : 0.0f;
    anim += (target - anim) * std::clamp(frameDeltaTime_ * 16.0f, 0.35f, 1.0f);
    anim = std::clamp(anim, 0.0f, 1.0f);

    const float glow = std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f);
    const glm::vec4 cardFill = glm::mix(style_.widgetBg, style_.widgetHover, hv.mix * 0.85f);
    const glm::vec4 baseBorder = state ? WithAlpha(style_.accent, 0.72f) : style_.windowBorder;
    const glm::vec4 cardBorder = glm::mix(baseBorder, style_.hoverBorder, glow);
    EmitRoundedBox(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, style_.widgetRadius, cardFill, cardBorder,
                   1.0f + 0.35f * glow, false);

    constexpr float kPillW = 38.0f;
    constexpr float kPillH = 18.0f;
    constexpr float kKnobSize = 14.0f;
    const float pillX = rect.x + rect.w - kPillW - 8.0f;
    const float pillY = rect.y + (hasDesc ? 8.0f : (rect.h - kPillH) * 0.5f);
    const glm::vec4 trackFill = glm::mix(style_.sliderTrack, style_.accent, anim);
    const glm::vec4 trackBorder = glm::mix(style_.windowBorder, style_.accent, std::max(anim, glow));
    EmitRoundedBox(vertices_, indices_, pillX, pillY, kPillW, kPillH, kPillH * 0.5f, trackFill, trackBorder, 1.0f,
                   false);

    const float knobX = pillX + 2.0f + anim * (kPillW - kKnobSize - 4.0f);
    const float knobY = pillY + (kPillH - kKnobSize) * 0.5f;
    EmitRoundedBox(vertices_, indices_, knobX, knobY, kKnobSize, kKnobSize, kKnobSize * 0.5f, style_.textPrimary,
                   glm::vec4{0.0f}, 0.0f, false);

    const float textMaxW = std::max(24.0f, (pillX - rect.x) - 16.0f);
    const std::string titleClipped = EllipsizeText(label, textMaxW, style_.textScale);
    EmitText(vertices_, indices_, titleClipped, rect.x + 10.0f, rect.y + (hasDesc ? 6.0f : 8.0f), style_.textPrimary,
             style_.textScale);
    if (hasDesc) {
        const float descScale = style_.textScale * 0.85f;
        const std::string descClipped = EllipsizeText(description, rect.w - 20.0f, descScale);
        EmitText(vertices_, indices_, descClipped, rect.x + 10.0f, rect.y + 26.0f, style_.textMuted, descScale);
    }

    AdvanceCursor(cardH + style_.itemSpacing);
    return clicked;
}

bool ImmediateUI::EmitRadioRow(uint64_t id, const UiRect& rect, std::string_view label, bool selected) {
    const HoverEval hv = EvalHover(id, rect);
    const bool clicked = hv.hovered && input_.mouseReleased && activeDragWindowId_ == 0;

    if (selected || hv.mix > 0.01f) {
        const float glow = std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f);
        const glm::vec4 fill = selected ? glm::mix(WithAlpha(style_.accent, 0.16f), style_.widgetHover, glow * 0.5f)
                                        : WithAlpha(style_.hoverHighlight, style_.hoverHighlight.a * glow);
        const glm::vec4 rim =
            selected ? WithAlpha(style_.accent, 0.55f + 0.35f * glow) : WithAlpha(style_.hoverBorder, 0.55f * glow);
        EmitRoundedBox(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, style_.widgetRadius, fill, rim, 1.0f,
                       false);
    }

    constexpr float kRingSize = 14.0f;
    const float rx = rect.x + 8.0f;
    const float ry = rect.y + (rect.h - kRingSize) * 0.5f;
    const glm::vec4 ringBorder = selected ? style_.accent : glm::mix(style_.windowBorder, style_.accent, hv.mix);
    EmitRoundedBox(vertices_, indices_, rx, ry, kRingSize, kRingSize, kRingSize * 0.5f, style_.sliderTrack, ringBorder,
                   1.25f, false);
    if (selected) {
        constexpr float kDot = 6.0f;
        EmitRoundedBox(vertices_, indices_, rx + (kRingSize - kDot) * 0.5f, ry + (kRingSize - kDot) * 0.5f, kDot, kDot,
                       kDot * 0.5f, style_.accent, glm::vec4{0.0f}, 0.0f, false);
    }

    const float textMaxW = std::max(20.0f, rect.w - kRingSize - 22.0f);
    const std::string clipped = EllipsizeText(label, textMaxW, style_.textScale);
    EmitText(vertices_, indices_, clipped, rx + kRingSize + 8.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);
    return clicked;
}

bool ImmediateUI::Option(std::string_view label, bool selected) {
    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool clicked = EmitRadioRow(HashId(label) ^ 0x4F505449ULL, rect, label, selected);
    AdvanceCursor(style_.rowHeight + style_.itemSpacing);
    return clicked;
}

bool ImmediateUI::OptionGroup(std::string_view label, std::span<const std::string_view> options, size_t& selectedIndex,
                              bool horizontal) {
    if (options.empty()) {
        Label(label, style_.textMuted);
        return false;
    }
    if (selectedIndex >= options.size()) {
        selectedIndex = 0;
    }

    float curY = cursorY_;
    if (!label.empty()) {
        EmitText(vertices_, indices_, label, cursorX_, curY, style_.textMuted, style_.textScale * 0.9f);
        curY += 17.0f;
    }

    bool changed = false;
    bool anyHovered = false;
    const uint64_t groupHash = HashId(label.empty() ? "OptionGroup" : label) ^ 0x4F4752ULL;
    const float rowH = style_.rowHeight;
    if (horizontal) {
        constexpr float kGap = 4.0f;
        const float segW = std::max(24.0f, (contentWidth_ - kGap * static_cast<float>(options.size() - 1)) /
                                               static_cast<float>(options.size()));
        for (size_t i = 0; i < options.size(); ++i) {
            const UiRect segRect{cursorX_ + static_cast<float>(i) * (segW + kGap), curY, segW, rowH};
            const HoverEval hv = EvalHover(groupHash ^ (0x100ULL + i), segRect);
            anyHovered = anyHovered || hv.hovered;
            if (hv.hovered && input_.mouseReleased && activeDragWindowId_ == 0 && selectedIndex != i) {
                selectedIndex = i;
                changed = true;
            }
            const bool isSel = (selectedIndex == i);
            const float glow = std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f);
            const glm::vec4 fill = isSel ? glm::mix(style_.widgetActive, style_.widgetHover, glow * 0.35f)
                                         : glm::mix(style_.widgetBg, style_.widgetHover, hv.mix);
            const glm::vec4 border = isSel ? style_.accent : glm::mix(style_.windowBorder, style_.hoverBorder, glow);
            EmitRoundedBox(vertices_, indices_, segRect.x, segRect.y, segRect.w, segRect.h, style_.widgetRadius, fill,
                           border, 1.0f + 0.3f * glow, false);

            const std::string lbl = EllipsizeText(options[i], std::max(12.0f, segW - 8.0f), style_.textScale * 0.92f);
            const float lblW = MeasureTextWidth(lbl, style_.textScale * 0.92f);
            EmitText(vertices_, indices_, lbl, segRect.x + (segW - lblW) * 0.5f, segRect.y + 4.0f, style_.textPrimary,
                     style_.textScale * 0.92f);
        }
        curY += rowH;
    } else {
        for (size_t i = 0; i < options.size(); ++i) {
            const UiRect rowRect{cursorX_, curY, contentWidth_, rowH};
            const bool clicked = EmitRadioRow(groupHash ^ (0x200ULL + i), rowRect, options[i], selectedIndex == i);
            anyHovered = anyHovered || lastItemHovered_;
            if (clicked && selectedIndex != i) {
                selectedIndex = i;
                changed = true;
            }
            curY += rowH + 2.0f;
        }
        curY -= 2.0f;
    }

    const UiRect fullRect{cursorX_, cursorY_, contentWidth_, curY - cursorY_};
    lastItemRect_ = fullRect;
    lastItemHovered_ = anyHovered;
    AdvanceCursor((curY - cursorY_) + style_.itemSpacing);
    return changed;
}

bool ImmediateUI::OptionGroup(std::string_view label, std::span<const char* const> options, size_t& selectedIndex,
                              bool horizontal) {
    std::pmr::vector<std::string_view> views{&frameArena_};
    views.reserve(options.size());
    for (const char* opt : options) {
        views.emplace_back(opt ? std::string_view{opt} : std::string_view{});
    }
    return OptionGroup(label, std::span<const std::string_view>{views.data(), views.size()}, selectedIndex, horizontal);
}

} // namespace bgl::ui
