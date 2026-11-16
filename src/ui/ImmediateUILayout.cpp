/**
 * @file ImmediateUILayout.cpp
 * @brief Layout containers, glassmorphic panels, visual groups, animated toggle boxes,
 *        and radio/segmented option controls with unified hover effects.
 */
#include "ui/ImmediateUI.hpp"
#include <algorithm>
#include <cmath>

namespace bgl::ui {

namespace {

[[nodiscard]] constexpr glm::vec4 WithAlpha(const glm::vec4& c, float alpha) noexcept {
    return glm::vec4{c.r, c.g, c.b, alpha};
}

} // namespace

void ImmediateUI::AdvanceCursor(float itemHeight) {
    if (!containerStack_.empty() && containerStack_.back().options.layout == ContainerLayout::Horizontal) {
        auto& c = containerStack_.back();
        c.maxExtentY = std::max(c.maxExtentY, cursorY_ + itemHeight);
        ++c.itemCount;
        if (c.options.columns > 0 && (c.itemCount % c.options.columns) == 0) {
            cursorX_ = c.contentStartX;
            c.contentStartY = c.maxExtentY;
            cursorY_ = c.contentStartY;
        } else {
            cursorX_ += contentWidth_ + c.options.spacing;
            cursorY_ = c.contentStartY;
        }
        return;
    }
    cursorY_ += itemHeight;
}

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

void ImmediateUI::BeginContainer(std::string_view id, const ContainerOptions& options) {
    const uint64_t hash = HashId(id) ^ 0x434E54ULL ^ (static_cast<uint64_t>(containerStack_.size()) * 0x9E3779B97F4A7C15ULL);
    const float boxW = (options.width > 0.0f) ? options.width : contentWidth_;
    const float pad = options.showBackground || options.showBorder ? std::max(0.0f, options.padding) : 0.0f;
    const float innerW = std::max(20.0f, boxW - 2.0f * pad);
    const float radius = (options.cornerRadius >= 0.0f) ? options.cornerRadius : style_.widgetRadius;

    ActiveContainer c{};
    c.id = hash;
    c.options = options;
    c.options.padding = pad;
    c.boxRect = UiRect{cursorX_, cursorY_, boxW, std::max(options.height, 2.0f * pad)};
    c.savedCursorX = cursorX_;
    c.savedCursorY = cursorY_;
    c.savedContentWidth = contentWidth_;
    c.contentStartX = cursorX_ + pad;
    c.contentStartY = cursorY_ + pad;
    c.startContentY = c.contentStartY;
    c.maxExtentY = c.contentStartY;
    c.hasBackground = options.showBackground || options.showBorder || options.hoverable;

    if (c.hasBackground) {
        const glm::vec4 bg =
            options.showBackground ? (options.backgroundColor.a > 0.001f ? options.backgroundColor : style_.groupBg)
                                   : glm::vec4{0.0f};
        const glm::vec4 border =
            options.showBorder ? (options.borderColor.a > 0.001f ? options.borderColor : WithAlpha(style_.windowBorder, 0.55f))
                               : glm::vec4{0.0f};
        c.bgVertexOffset = vertices_.size();
        EmitRoundedBox(vertices_, indices_, c.boxRect.x, c.boxRect.y, c.boxRect.w, c.boxRect.h, radius, bg, border,
                       options.showBorder ? 1.0f : 0.0f, false);
    }

    cursorX_ = c.contentStartX;
    cursorY_ = c.contentStartY;
    if (options.layout == ContainerLayout::Horizontal) {
        if (options.itemWidth > 0.0f) {
            contentWidth_ = options.itemWidth;
        } else {
            const uint32_t cols = (options.columns > 0) ? options.columns : 2u;
            contentWidth_ = std::max(24.0f, (innerW - static_cast<float>(cols - 1) * options.spacing) /
                                                static_cast<float>(cols));
        }
    } else {
        contentWidth_ = innerW;
    }

    containerStack_.push_back(c);
}

void ImmediateUI::EndContainer() {
    if (containerStack_.empty()) {
        return;
    }
    const ActiveContainer c = containerStack_.back();
    containerStack_.pop_back();

    float contentBottom = c.startContentY;
    if (c.options.layout == ContainerLayout::Horizontal) {
        contentBottom = (c.itemCount > 0) ? std::max(c.startContentY, c.maxExtentY - style_.itemSpacing) : c.startContentY;
    } else {
        contentBottom = (cursorY_ > c.startContentY) ? std::max(c.startContentY, cursorY_ - style_.itemSpacing) : c.startContentY;
    }

    const float boxH = std::max(c.options.height, (contentBottom - c.startContentY) + 2.0f * c.options.padding);
    const UiRect finalRect{c.boxRect.x, c.boxRect.y, c.boxRect.w, boxH};

    const HoverEval hv = EvalHover(c.id, finalRect, c.options.hoverable, false);

    if (c.hasBackground) {
        const float radius = (c.options.cornerRadius >= 0.0f) ? c.options.cornerRadius : style_.widgetRadius;
        const glm::vec4 baseBg =
            c.options.showBackground ? (c.options.backgroundColor.a > 0.001f ? c.options.backgroundColor : style_.groupBg)
                                     : glm::vec4{0.0f};
        const glm::vec4 baseBorder =
            c.options.showBorder
                ? (c.options.borderColor.a > 0.001f ? c.options.borderColor : WithAlpha(style_.windowBorder, 0.55f))
                : glm::vec4{0.0f};
        const float glow = c.options.hoverable ? std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f) : 0.0f;
        const glm::vec4 finalBg =
            (glow > 0.01f) ? glm::mix(baseBg, WithAlpha(style_.hoverHighlight, std::max(baseBg.a, style_.hoverHighlight.a)), glow)
                           : baseBg;
        const glm::vec4 finalBorder =
            (glow > 0.01f) ? glm::mix(baseBorder, style_.hoverBorder, glow) : baseBorder;
        const float borderW = (c.options.showBorder || glow > 0.01f) ? (1.0f + 0.35f * glow) : 0.0f;
        PatchBoxGeometry(c.bgVertexOffset, finalRect.x, finalRect.y, finalRect.w, finalRect.h, 1.5f, radius, borderW,
                         &finalBg, &finalBorder);
    }

    cursorX_ = c.savedCursorX;
    cursorY_ = c.savedCursorY;
    contentWidth_ = c.savedContentWidth;
    lastItemRect_ = finalRect;
    lastItemHovered_ = hv.hovered;

    AdvanceCursor(boxH + style_.itemSpacing);
}

bool ImmediateUI::BeginPanel(std::string_view title, const PanelOptions& options) {
    const float w = contentWidth_;
    return BeginPanel(title, cursorX_, cursorY_, w, 0.0f, title, options);
}

bool ImmediateUI::BeginPanel(std::string_view id, float x, float y, float width, float height, std::string_view title,
                             const PanelOptions& options) {
    const bool standalone = !inWindow_ || (x != cursorX_ || y != cursorY_ || width != contentWidth_ || height > 0.0f);
    const uint64_t hash = HashId(id.empty() ? title : id) ^ 0x504E4CULL;
    const float radius = (options.cornerRadius >= 0.0f) ? options.cornerRadius : style_.cornerRadius;
    const float pad = std::max(0.0f, options.padding);
    const bool hasHeader = !title.empty();
    const float headerH = hasHeader ? style_.headerHeight : 0.0f;

    bool isOpen = true;
    if (options.collapsible) {
        auto [it, inserted] = collapsingHeaders_.emplace(hash ^ 0x4F504EULL, options.defaultOpen);
        isOpen = it->second;
    }

    ActivePanel p{};
    p.id = hash;
    p.options = options;
    p.boxRect = UiRect{x, y, std::max(40.0f, width), std::max(height, headerH + (isOpen ? pad : 0.0f))};
    p.savedCursorX = cursorX_;
    p.savedCursorY = cursorY_;
    p.savedContentWidth = contentWidth_;
    p.minHeight = height;
    p.standalone = standalone;
    p.open = isOpen;

    const glm::vec4 bg = (options.backgroundColor.a > 0.001f) ? options.backgroundColor : style_.panelBg;
    const glm::vec4 border = (options.borderColor.a > 0.001f) ? options.borderColor : style_.windowBorder;

    if (options.showShadow) {
        p.shadowVertexOffset = vertices_.size();
        EmitShadowBox(vertices_, indices_, p.boxRect.x, p.boxRect.y + 3.0f, p.boxRect.w, p.boxRect.h, radius,
                      style_.shadowBlur, WithAlpha(style_.shadowColor, style_.shadowColor.a * bg.a));
    }

    p.bgVertexOffset = vertices_.size();
    EmitRoundedBox(vertices_, indices_, p.boxRect.x, p.boxRect.y, p.boxRect.w, p.boxRect.h, radius, bg, border, 1.0f,
                   options.frosted);

    if (hasHeader) {
        const UiRect hdrRect{x + 1.0f, y + 1.0f, std::max(20.0f, p.boxRect.w - 2.0f), headerH - 1.0f};
        const HoverEval hdrHover = EvalHover(hash ^ 0x484452ULL, hdrRect, options.collapsible || options.hoverable, false);
        if (options.collapsible && hdrHover.hovered && input_.mouseReleased && activeDragWindowId_ == 0) {
            isOpen = !isOpen;
            collapsingHeaders_[hash ^ 0x4F504EULL] = isOpen;
            p.open = isOpen;
        }
        const glm::vec4 hdrFill = glm::mix(style_.headerBg, style_.widgetHover, hdrHover.mix * 0.45f);
        EmitRoundedBox(vertices_, indices_, hdrRect.x, hdrRect.y, hdrRect.w, hdrRect.h, std::max(1.0f, radius - 1.0f),
                       hdrFill, hdrHover.mix > 0.01f ? WithAlpha(style_.accent, 0.40f * hdrHover.mix) : glm::vec4{0.0f},
                       hdrHover.mix > 0.01f ? 1.0f : 0.0f, false);
        EmitRoundedBox(vertices_, indices_, x + 6.0f, y + 7.0f, 3.0f, headerH - 14.0f, 1.5f, style_.accent,
                       glm::vec4{0.0f}, 0.0f, false);
        float textX = x + pad + 4.0f;
        if (options.collapsible) {
            EmitText(vertices_, indices_, isOpen ? "v" : ">", textX, y + 6.0f, style_.accent, style_.textScale * 0.9f);
            textX += 14.0f;
        }
        EmitText(vertices_, indices_, title, textX, y + 6.0f, style_.textPrimary, style_.textScale);
    }

    cursorX_ = x + pad;
    cursorY_ = y + headerH + (isOpen ? pad : 0.0f);
    contentWidth_ = std::max(20.0f, p.boxRect.w - 2.0f * pad);

    panelStack_.push_back(p);
    return isOpen;
}

void ImmediateUI::EndPanel() {
    if (panelStack_.empty()) {
        return;
    }
    const ActivePanel p = panelStack_.back();
    panelStack_.pop_back();

    const float pad = std::max(0.0f, p.options.padding);
    const float radius = (p.options.cornerRadius >= 0.0f) ? p.options.cornerRadius : style_.cornerRadius;

    float finalH = p.boxRect.h;
    if (p.open) {
        const float rawH = (cursorY_ - p.boxRect.y) + pad - style_.itemSpacing;
        finalH = std::max({p.minHeight, style_.headerHeight, rawH});
    } else {
        finalH = std::max(style_.headerHeight, p.minHeight > 0.0f ? style_.headerHeight : style_.headerHeight);
    }

    const UiRect finalRect{p.boxRect.x, p.boxRect.y, p.boxRect.w, finalH};
    const HoverEval hv = EvalHover(p.id, finalRect, p.options.hoverable, p.standalone);

    const glm::vec4 baseBg = (p.options.backgroundColor.a > 0.001f) ? p.options.backgroundColor : style_.panelBg;
    const glm::vec4 baseBorder = (p.options.borderColor.a > 0.001f) ? p.options.borderColor : style_.windowBorder;
    const float glow = p.options.hoverable ? std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f) : 0.0f;
    const glm::vec4 finalBorder = (glow > 0.01f) ? glm::mix(baseBorder, style_.hoverBorder, glow) : baseBorder;

    if (p.options.showShadow) {
        PatchBoxGeometry(p.shadowVertexOffset, finalRect.x, finalRect.y + 3.0f, finalRect.w, finalRect.h,
                         style_.shadowBlur, radius, style_.shadowBlur);
    }
    PatchBoxGeometry(p.bgVertexOffset, finalRect.x, finalRect.y, finalRect.w, finalRect.h, 1.5f, radius,
                     1.0f + 0.4f * glow, &baseBg, &finalBorder);

    if (p.standalone) {
        hitRects_.push_back(finalRect);
    }

    cursorX_ = p.savedCursorX;
    cursorY_ = p.savedCursorY;
    contentWidth_ = p.savedContentWidth;
    lastItemRect_ = finalRect;
    lastItemHovered_ = hv.hovered;

    if (!p.standalone) {
        AdvanceCursor(finalRect.h + style_.itemSpacing);
    }
}

void ImmediateUI::BeginGroup(std::string_view label, bool border) {
    const uint64_t hash = HashId(label.empty() ? "group" : label) ^ 0x475250ULL ^
                          (static_cast<uint64_t>(groupStack_.size()) * 0x9E3779B97F4A7C15ULL);
    const float pad = 8.0f;
    const bool hasLabel = !label.empty();

    ActiveGroup g{};
    g.id = hash;
    g.boxRect = UiRect{cursorX_, cursorY_, contentWidth_, pad * 2.0f + (hasLabel ? 18.0f : 0.0f)};
    g.savedCursorX = cursorX_;
    g.savedCursorY = cursorY_;
    g.savedContentWidth = contentWidth_;
    g.hasBorder = border;
    g.bgVertexOffset = vertices_.size();

    EmitRoundedBox(vertices_, indices_, g.boxRect.x, g.boxRect.y, g.boxRect.w, g.boxRect.h, style_.widgetRadius,
                   style_.groupBg, border ? WithAlpha(style_.windowBorder, 0.55f) : glm::vec4{0.0f},
                   border ? 1.0f : 0.0f, false);

    cursorX_ += pad;
    cursorY_ += pad;
    contentWidth_ = std::max(20.0f, contentWidth_ - 2.0f * pad);

    if (hasLabel) {
        EmitText(vertices_, indices_, label, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.9f);
        cursorY_ += 18.0f;
    }

    groupStack_.push_back(g);
}

void ImmediateUI::EndGroup() {
    if (groupStack_.empty()) {
        return;
    }
    const ActiveGroup g = groupStack_.back();
    groupStack_.pop_back();

    const float pad = 8.0f;
    const float rawH = (cursorY_ - g.boxRect.y) + pad - style_.itemSpacing;
    const float finalH = std::max(pad * 2.0f, rawH);
    const UiRect finalRect{g.boxRect.x, g.boxRect.y, g.boxRect.w, finalH};

    const HoverEval hv = EvalHover(g.id, finalRect, true, false);
    const float glow = std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f);
    const glm::vec4 baseBorder = g.hasBorder ? WithAlpha(style_.windowBorder, 0.55f) : glm::vec4{0.0f};
    const glm::vec4 finalBorder = (glow > 0.01f) ? glm::mix(baseBorder, style_.hoverBorder, glow) : baseBorder;
    const float borderW = (g.hasBorder || glow > 0.01f) ? (1.0f + 0.35f * glow) : 0.0f;

    PatchBoxGeometry(g.bgVertexOffset, finalRect.x, finalRect.y, finalRect.w, finalRect.h, 1.5f, style_.widgetRadius,
                     borderW, &style_.groupBg, &finalBorder);

    cursorX_ = g.savedCursorX;
    cursorY_ = g.savedCursorY;
    contentWidth_ = g.savedContentWidth;
    lastItemRect_ = finalRect;
    lastItemHovered_ = hv.hovered;

    AdvanceCursor(finalH + style_.itemSpacing);
}

bool ImmediateUI::ToggleBox(std::string_view label, bool& state, std::string_view description) {
    const uint64_t id = HashId(label) ^ 0x544F47ULL;
    const bool hasDesc = !description.empty();
    const float boxH = hasDesc ? 42.0f : style_.rowHeight;
    const UiRect rect{cursorX_, cursorY_, contentWidth_, boxH};
    const HoverEval hv = EvalHover(id, rect);

    const bool clicked = hv.hovered && input_.mouseReleased && activeDragWindowId_ == 0;
    if (clicked) {
        state = !state;
    }

    float& toggleMix = toggleAmounts_[id];
    const float step = std::clamp(frameDeltaTime_ * style_.hoverTransitionSpeed, 0.45f, 1.0f);
    const float target = state ? 1.0f : 0.0f;
    toggleMix += (target - toggleMix) * step;
    if (std::abs(target - toggleMix) < 0.005f) {
        toggleMix = target;
    }

    const float glow = std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f);
    const glm::vec4 cardBg = glm::mix(style_.widgetBg, style_.widgetHover, glow * 0.75f);
    const glm::vec4 cardBorder =
        glm::mix(WithAlpha(style_.windowBorder, 0.65f), style_.hoverBorder, std::max(glow, state ? 0.45f : 0.0f));
    EmitRoundedBox(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, style_.widgetRadius, cardBg, cardBorder,
                   1.0f + 0.35f * glow, false);

    const float switchW = 34.0f;
    const float switchH = 18.0f;
    const float switchX = rect.x + rect.w - switchW - 8.0f;
    const float switchY = rect.y + (rect.h - switchH) * 0.5f;
    const glm::vec4 trackFill = glm::mix(style_.sliderTrack, style_.accent, toggleMix);
    const glm::vec4 trackBorder =
        glm::mix(style_.windowBorder, style_.accent, std::max(toggleMix, glow * 0.85f));
    EmitRoundedBox(vertices_, indices_, switchX, switchY, switchW, switchH, switchH * 0.5f, trackFill, trackBorder,
                   1.0f, false);

    const float knobSize = 12.0f;
    const float knobPad = (switchH - knobSize) * 0.5f;
    const float knobX = switchX + knobPad + toggleMix * (switchW - knobSize - 2.0f * knobPad);
    const float knobY = switchY + knobPad;
    EmitRoundedBox(vertices_, indices_, knobX, knobY, knobSize, knobSize, knobSize * 0.5f, style_.textPrimary,
                   glm::vec4{0.0f}, 0.0f, false);

    const float textMaxW = std::max(24.0f, switchX - rect.x - 16.0f);
    const std::string titleClipped = EllipsizeText(label, textMaxW, style_.textScale);
    if (hasDesc) {
        EmitText(vertices_, indices_, titleClipped, rect.x + 8.0f, rect.y + 5.0f, style_.textPrimary, style_.textScale);
        const std::string descClipped = EllipsizeText(description, textMaxW, style_.textScale * 0.84f);
        EmitText(vertices_, indices_, descClipped, rect.x + 8.0f, rect.y + 22.0f, style_.textMuted,
                 style_.textScale * 0.84f);
    } else {
        EmitText(vertices_, indices_, titleClipped, rect.x + 8.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);
    }

    AdvanceCursor(boxH + style_.itemSpacing);
    return clicked;
}

bool ImmediateUI::Option(std::string_view label, bool selected) {
    const uint64_t id = HashId(label) ^ 0x4F5054ULL;
    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const HoverEval hv = EvalHover(id, rect);
    const bool clicked = hv.hovered && input_.mouseReleased && activeDragWindowId_ == 0;

    if (hv.mix > 0.01f || selected) {
        const float highlightMix = selected ? std::max(0.45f, hv.mix) : hv.mix;
        EmitHoverHighlight(vertices_, indices_, UiRect{rect.x - 2.0f, rect.y, rect.w + 4.0f, rect.h}, highlightMix,
                           style_.widgetRadius);
    }

    const float outerSize = 14.0f;
    const float outerY = rect.y + (rect.h - outerSize) * 0.5f;
    const glm::vec4 ringFill = glm::mix(style_.widgetBg, style_.widgetHover, hv.mix);
    const glm::vec4 ringBorder = glm::mix(style_.windowBorder, style_.accent, selected ? 1.0f : hv.mix);
    EmitRoundedBox(vertices_, indices_, rect.x + 2.0f, outerY, outerSize, outerSize, outerSize * 0.5f, ringFill,
                   ringBorder, selected || hv.mix > 0.25f ? 1.35f : 1.0f, false);

    if (selected) {
        const float innerSize = 6.0f;
        const float innerOffset = (outerSize - innerSize) * 0.5f;
        EmitRoundedBox(vertices_, indices_, rect.x + 2.0f + innerOffset, outerY + innerOffset, innerSize, innerSize,
                       innerSize * 0.5f, style_.accent, glm::vec4{0.0f}, 0.0f, false);
    }

    const std::string clipped = EllipsizeText(label, std::max(20.0f, rect.w - outerSize - 14.0f), style_.textScale);
    EmitText(vertices_, indices_, clipped, rect.x + outerSize + 10.0f, rect.y + 4.0f,
             selected ? style_.textPrimary : glm::mix(style_.textMuted, style_.textPrimary, 0.55f + 0.45f * hv.mix),
             style_.textScale);

    AdvanceCursor(style_.rowHeight + style_.itemSpacing);
    return clicked;
}

bool ImmediateUI::OptionGroup(std::string_view label, std::span<const std::string_view> options, size_t& selectedIndex,
                              bool horizontal) {
    if (options.empty()) {
        if (!label.empty()) {
            Label(label, style_.textMuted);
        }
        return false;
    }
    if (selectedIndex >= options.size()) {
        selectedIndex = 0;
    }

    if (!label.empty()) {
        EmitText(vertices_, indices_, label, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.9f);
        cursorY_ += 17.0f;
    }

    bool changed = false;
    const uint64_t groupId = HashId(label.empty() ? "OptionGroup" : label) ^ 0x4F4752ULL;

    if (!horizontal) {
        BeginGroup({}, true);
        for (size_t i = 0; i < options.size(); ++i) {
            if (Option(options[i], selectedIndex == i)) {
                if (selectedIndex != i) {
                    selectedIndex = i;
                    changed = true;
                }
            }
        }
        EndGroup();
        return changed;
    }

    const float barH = style_.rowHeight + 4.0f;
    const UiRect barRect{cursorX_, cursorY_, contentWidth_, barH};
    EmitRoundedBox(vertices_, indices_, barRect.x, barRect.y, barRect.w, barRect.h, style_.widgetRadius, style_.groupBg,
                   WithAlpha(style_.windowBorder, 0.65f), 1.0f, false);

    const float pad = 2.0f;
    const float gap = 2.0f;
    const float segW =
        std::max(16.0f, (barRect.w - 2.0f * pad - gap * static_cast<float>(options.size() - 1)) /
                            static_cast<float>(options.size()));
    const float segH = barRect.h - 2.0f * pad;

    bool anyHovered = false;
    for (size_t i = 0; i < options.size(); ++i) {
        const float sx = barRect.x + pad + static_cast<float>(i) * (segW + gap);
        const UiRect segRect{sx, barRect.y + pad, segW, segH};
        const HoverEval hv = EvalHover(groupId ^ (0x100ULL + static_cast<uint64_t>(i)), segRect);
        anyHovered = anyHovered || hv.hovered;

        if (hv.hovered && input_.mouseReleased && activeDragWindowId_ == 0) {
            if (selectedIndex != i) {
                selectedIndex = i;
                changed = true;
            }
        }

        const bool isSel = (i == selectedIndex);
        const float glow = std::clamp(hv.mix * style_.hoverGlowStrength, 0.0f, 1.0f);
        if (isSel || glow > 0.01f) {
            const glm::vec4 segFill =
                isSel ? glm::mix(style_.widgetActive, style_.accent, glow * 0.45f)
                      : WithAlpha(style_.widgetHover, style_.widgetHover.a * glow);
            const glm::vec4 segBorder =
                isSel ? style_.accent : (glow > 0.1f ? WithAlpha(style_.hoverBorder, glow * 0.7f) : glm::vec4{0.0f});
            EmitRoundedBox(vertices_, indices_, segRect.x, segRect.y, segRect.w, segRect.h,
                           std::max(2.0f, style_.widgetRadius - 1.0f), segFill, segBorder,
                           (isSel || glow > 0.1f) ? 1.0f : 0.0f, false);
        }

        const float textScale = style_.textScale * 0.92f;
        const std::string optText = EllipsizeText(options[i], std::max(8.0f, segRect.w - 8.0f), textScale);
        const float textW = MeasureTextWidth(optText, textScale);
        const float tx = segRect.x + std::max(4.0f, (segRect.w - textW) * 0.5f);
        const glm::vec4 textCol = isSel ? style_.textPrimary : glm::mix(style_.textMuted, style_.textPrimary, glow);
        EmitText(vertices_, indices_, optText, tx, segRect.y + 3.0f, textCol, textScale);
    }

    lastItemRect_ = barRect;
    lastItemHovered_ = anyHovered;
    AdvanceCursor(barH + style_.itemSpacing);
    return changed;
}

bool ImmediateUI::OptionGroup(std::string_view label, std::span<const char* const> options, size_t& selectedIndex,
                              bool horizontal) {
    std::pmr::vector<std::string_view> views{&frameArena_};
    views.reserve(options.size());
    for (const char* opt : options) {
        views.push_back(opt ? std::string_view{opt} : std::string_view{});
    }
    return OptionGroup(label, std::span<const std::string_view>{views.data(), views.size()}, selectedIndex, horizontal);
}

} // namespace bgl::ui
