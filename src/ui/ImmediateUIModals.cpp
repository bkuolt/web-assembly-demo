/**
 * @file ImmediateUIModals.cpp
 * @brief Frosted-glass modal MessageBox, fullscreen LoadingScreen, LoadingBanner, and FloatingBadge components.
 */
#include "ui/ImmediateUI.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace bgl::ui {

glm::vec4 ImmediateUI::GetMessageAccentColor(MessageBoxType type) const noexcept {
    switch (type) {
    case MessageBoxType::Warning:
        return glm::vec4{0.98f, 0.72f, 0.16f, 1.0f};
    case MessageBoxType::Error:
        return glm::vec4{0.92f, 0.28f, 0.34f, 1.0f};
    case MessageBoxType::Confirm:
        return glm::vec4{0.22f, 0.80f, 0.62f, 1.0f};
    case MessageBoxType::Info:
    default:
        return style_.accent;
    }
}

void ImmediateUI::DrawLoadingBanner(std::string_view title, float fraction, std::string_view detailText, bool isError,
                                    float alphaMultiplier) {
    const float alpha = std::clamp(alphaMultiplier, 0.0f, 1.0f);
    if (alpha <= 0.01f) {
        return;
    }

    const float vpW = static_cast<float>(viewportWidth_);
    const float vpH = static_cast<float>(viewportHeight_);
    const float cardW = std::clamp(vpW - 36.0f, 240.0f, 440.0f);
    const float cardH = 68.0f;
    const float cardX = (vpW - cardW) * 0.5f;
    const float cardY = std::max(12.0f, vpH - cardH - 22.0f);
    const UiRect cardRect{cardX, cardY, cardW, cardH};

    const HoverEval hv = EvalHover(HashId("LoadingBanner"), cardRect, true, false);

    const float clamped = (fraction < 0.0f) ? -1.0f : std::clamp(fraction, 0.0f, 1.0f);
    glm::vec4 barAccent = style_.accent;
    if (isError) {
        barAccent = glm::vec4{0.92f, 0.28f, 0.34f, 1.0f};
    } else if (clamped >= 0.999f) {
        barAccent = glm::vec4{0.22f, 0.82f, 0.52f, 1.0f};
    }
    barAccent.a *= alpha;

    const glm::vec4 bg = WithAlpha(style_.popupBg, style_.popupBg.a * alpha);
    const glm::vec4 primaryText = WithAlpha(style_.textPrimary, style_.textPrimary.a * alpha);
    const glm::vec4 mutedText = WithAlpha(style_.textMuted, style_.textMuted.a * alpha);

    EmitPanelCard(overlayVertices_, overlayIndices_, cardRect, style_.cornerRadius, bg, barAccent, true,
                  1.15f + 0.35f * hv.mix);

    const std::string titleClipped = EllipsizeText(title, cardW - 76.0f, style_.textScale);
    EmitText(overlayVertices_, overlayIndices_, titleClipped, cardX + 14.0f, cardY + 9.0f, primaryText,
             style_.textScale);

    char rightBuf[24]{};
    if (isError) {
        std::snprintf(rightBuf, sizeof(rightBuf), "Error");
    } else if (clamped < 0.0f) {
        std::snprintf(rightBuf, sizeof(rightBuf), "...");
    } else {
        std::snprintf(rightBuf, sizeof(rightBuf), "%d%%", static_cast<int>(std::lround(clamped * 100.0f)));
    }
    const float rightScale = style_.textScale * 0.92f;
    const float rightW = MeasureTextWidth(rightBuf, rightScale);
    EmitText(overlayVertices_, overlayIndices_, rightBuf, cardX + cardW - 14.0f - rightW, cardY + 9.0f, barAccent,
             rightScale);

    const UiRect trackRect{cardX + 14.0f, cardY + 31.0f, cardW - 28.0f, 10.0f};
    EmitProgressTrack(overlayVertices_, overlayIndices_, trackRect, clamped, barAccent, alpha, hv.mix);

    if (!detailText.empty()) {
        const float detailScale = style_.textScale * 0.84f;
        const std::string detailClipped = EllipsizeText(detailText, trackRect.w, detailScale);
        EmitText(overlayVertices_, overlayIndices_, detailClipped, trackRect.x, cardY + 46.0f, mutedText, detailScale);
    }
}

bool ImmediateUI::DrawLoadingScreen(std::string_view title, float fraction, std::string_view statusText, bool isError,
                                    bool cancellable, float alphaMultiplier) {
    if (std::clamp(alphaMultiplier, 0.0f, 1.0f) > 0.01f) {
        immediateModalDrawnThisFrame_ = true;
    }
    return DrawLoadingScreenInternal(title, fraction, statusText, isError, cancellable, alphaMultiplier);
}

bool ImmediateUI::DrawLoadingScreenInternal(std::string_view title, float fraction, std::string_view statusText,
                                            bool isError, bool cancellable, float alphaMultiplier) {
    const float alpha = std::clamp(alphaMultiplier, 0.0f, 1.0f);
    if (alpha <= 0.01f) {
        return false;
    }

    modalDrawnThisFrame_ = true;

    const float vpW = static_cast<float>(viewportWidth_);
    const float vpH = static_cast<float>(viewportHeight_);
    const UiRect screenRect{0.0f, 0.0f, vpW, vpH};
    hitRects_.push_back(screenRect);

    EmitRect(overlayVertices_, overlayIndices_, 0.0f, 0.0f, vpW, vpH,
             WithAlpha(style_.modalBackdrop, style_.modalBackdrop.a * alpha));

    const float cardW = std::clamp(vpW - 48.0f, 300.0f, 480.0f);
    const float pad = 20.0f;
    const float innerW = cardW - 2.0f * pad;
    const float statusScale = style_.textScale * 0.90f;

    std::vector<std::string> wrappedLines;
    if (!statusText.empty()) {
        auto& slot = ActiveFontSlot();
        if (slot.ready && slot.atlas) {
            wrappedLines = text::TextShaper::WrapText(*slot.atlas, statusText, innerW, EffectiveTextScale(statusScale));
        } else {
            wrappedLines.emplace_back(statusText);
        }
        if (wrappedLines.size() > 3) {
            wrappedLines.resize(3);
            wrappedLines.back() = EllipsizeText(wrappedLines.back(), innerW, statusScale);
        }
    }

    const float lineStep = 18.0f;
    const float statusH = wrappedLines.empty() ? 0.0f : (static_cast<float>(wrappedLines.size()) * lineStep + 6.0f);
    const float cancelH = cancellable ? (style_.rowHeight + 12.0f) : 0.0f;
    const float cardH = 82.0f + statusH + cancelH;
    const float cardX = (vpW - cardW) * 0.5f;
    const float cardY = (vpH - cardH) * 0.5f;
    const UiRect cardRect{cardX, cardY, cardW, cardH};

    const float clamped = (fraction < 0.0f) ? -1.0f : std::clamp(fraction, 0.0f, 1.0f);
    glm::vec4 accentCol = style_.accent;
    if (isError) {
        accentCol = glm::vec4{0.92f, 0.28f, 0.34f, 1.0f};
    } else if (clamped >= 0.999f) {
        accentCol = glm::vec4{0.22f, 0.82f, 0.52f, 1.0f};
    }
    accentCol.a *= alpha;

    const HoverEval cardHover = EvalHover(HashId("LoadingScreenCard"), cardRect, true, false, true);
    EmitPanelCard(overlayVertices_, overlayIndices_, cardRect, style_.cornerRadius,
                  WithAlpha(style_.windowBg, style_.windowBg.a * alpha), accentCol, true, 1.25f);
    EmitRoundedBox(overlayVertices_, overlayIndices_, cardX + 2.0f, cardY + 2.0f, cardW - 4.0f, 3.0f, 1.5f, accentCol,
                   glm::vec4{0.0f}, 0.0f, false);

    const float titleScale = style_.textScale * 1.08f;
    const std::string titleClipped = EllipsizeText(title.empty() ? "Loading..." : title, innerW - 72.0f, titleScale);
    EmitText(overlayVertices_, overlayIndices_, titleClipped, cardX + pad, cardY + 14.0f,
             WithAlpha(style_.textPrimary, style_.textPrimary.a * alpha), titleScale);

    char rightBuf[24]{};
    if (isError) {
        std::snprintf(rightBuf, sizeof(rightBuf), "Error");
    } else if (clamped < 0.0f) {
        std::snprintf(rightBuf, sizeof(rightBuf), "Working");
    } else {
        std::snprintf(rightBuf, sizeof(rightBuf), "%d%%", static_cast<int>(std::lround(clamped * 100.0f)));
    }
    const float rightScale = style_.textScale * 0.92f;
    const float rightW = MeasureTextWidth(rightBuf, rightScale);
    EmitText(overlayVertices_, overlayIndices_, rightBuf, cardX + cardW - pad - rightW, cardY + 15.0f, accentCol,
             rightScale);

    const UiRect trackRect{cardX + pad, cardY + 46.0f, innerW, 14.0f};
    EmitProgressTrack(overlayVertices_, overlayIndices_, trackRect, clamped, accentCol, alpha, cardHover.mix);

    float curY = trackRect.y + trackRect.h + 12.0f;
    for (const auto& line : wrappedLines) {
        EmitText(overlayVertices_, overlayIndices_, line, cardX + pad, curY,
                 WithAlpha(style_.textMuted, style_.textMuted.a * alpha), statusScale);
        curY += lineStep;
    }

    bool cancelled = false;
    if (cancellable) {
        const float btnW = 108.0f;
        const float btnH = style_.rowHeight;
        const UiRect btnRect{cardX + (cardW - btnW) * 0.5f, cardY + cardH - pad - btnH + 4.0f, btnW, btnH};
        const HoverEval btnHover = EvalHover(HashId("LoadingScreenCancel"), btnRect, true, true, true);
        const bool pressed = btnHover.hovered && input_.mouseDown;
        cancelled = btnHover.hovered && input_.mouseReleased;

        const glm::vec4 btnFill =
            pressed ? style_.widgetActive : glm::mix(style_.widgetBg, style_.widgetHover, btnHover.mix);
        const glm::vec4 btnBorder = glm::mix(style_.windowBorder, accentCol, pressed ? 1.0f : btnHover.mix);
        EmitRoundedBox(overlayVertices_, overlayIndices_, btnRect.x, btnRect.y, btnRect.w, btnRect.h,
                       style_.widgetRadius, WithAlpha(btnFill, btnFill.a * alpha),
                       WithAlpha(btnBorder, btnBorder.a * alpha), 1.0f + 0.25f * btnHover.mix, false);
        const float lblW = MeasureTextWidth("Cancel", style_.textScale);
        EmitText(overlayVertices_, overlayIndices_, "Cancel", btnRect.x + (btnW - lblW) * 0.5f, btnRect.y + 4.0f,
                 WithAlpha(style_.textPrimary, style_.textPrimary.a * alpha), style_.textScale);
    }

    return cancelled;
}

void ImmediateUI::ShowLoadingScreen(std::string_view title, float fraction, std::string_view statusText, bool isError) {
    loadingState_.active = true;
    loadingState_.title = std::string{title};
    loadingState_.fraction = fraction;
    loadingState_.status = std::string{statusText};
    loadingState_.isError = isError;
}

void ImmediateUI::HideLoadingScreen() noexcept {
    loadingState_.active = false;
}

int ImmediateUI::DrawMessageBoxInternal(std::string_view id, std::string_view title, std::string_view message,
                                        std::span<const std::string_view> buttons, MessageBoxType type,
                                        bool dimBackground) {
    modalDrawnThisFrame_ = true;

    const float vpW = static_cast<float>(viewportWidth_);
    const float vpH = static_cast<float>(viewportHeight_);
    const UiRect screenRect{0.0f, 0.0f, vpW, vpH};
    hitRects_.push_back(screenRect);

    if (dimBackground) {
        EmitRect(overlayVertices_, overlayIndices_, 0.0f, 0.0f, vpW, vpH, style_.modalBackdrop);
    }

    static constexpr std::array<std::string_view, 1> kDefaultOk{"OK"};
    static constexpr std::array<std::string_view, 2> kDefaultConfirm{"Cancel", "OK"};
    std::span<const std::string_view> resolvedButtons = buttons;
    if (resolvedButtons.empty()) {
        resolvedButtons = (type == MessageBoxType::Confirm) ? std::span<const std::string_view>{kDefaultConfirm}
                                                            : std::span<const std::string_view>{kDefaultOk};
    }

    const float cardW = std::clamp(vpW - 48.0f, 300.0f, 460.0f);
    const float pad = 18.0f;
    const float innerW = cardW - 2.0f * pad;

    std::vector<std::string> bodyLines;
    if (!message.empty()) {
        auto& slot = ActiveFontSlot();
        if (slot.ready && slot.atlas) {
            bodyLines = text::TextShaper::WrapText(*slot.atlas, message, innerW, EffectiveTextScale(style_.textScale));
        } else {
            bodyLines.emplace_back(message);
        }
        if (bodyLines.size() > 10) {
            bodyLines.resize(10);
            bodyLines.back() = EllipsizeText(bodyLines.back(), innerW, style_.textScale);
        }
    }

    const float lineStep = 20.0f;
    const float bodyH = std::max(24.0f, static_cast<float>(bodyLines.size()) * lineStep);
    const float btnH = style_.rowHeight + 2.0f;
    const float cardH = style_.headerHeight + pad * 2.0f + bodyH + 14.0f + btnH;
    const float cardX = (vpW - cardW) * 0.5f;
    const float cardY = (vpH - cardH) * 0.5f;
    const UiRect cardRect{cardX, cardY, cardW, cardH};

    const glm::vec4 accent = GetMessageAccentColor(type);
    EmitPanelCard(overlayVertices_, overlayIndices_, cardRect, style_.cornerRadius, style_.popupBg,
                  WithAlpha(accent, 0.85f), true, 1.25f);
    EmitRoundedBox(overlayVertices_, overlayIndices_, cardX + 1.0f, cardY + 1.0f, cardW - 2.0f,
                   style_.headerHeight - 1.0f, std::max(1.0f, style_.cornerRadius - 1.0f), style_.headerBg,
                   glm::vec4{0.0f}, 0.0f, false);
    EmitRoundedBox(overlayVertices_, overlayIndices_, cardX + 6.0f, cardY + 7.0f, 3.0f, style_.headerHeight - 14.0f,
                   1.5f, accent, glm::vec4{0.0f}, 0.0f, false);

    const std::string titleClipped = EllipsizeText(title.empty() ? "Message" : title, cardW - 36.0f, style_.textScale);
    EmitText(overlayVertices_, overlayIndices_, titleClipped, cardX + pad, cardY + 6.0f, style_.textPrimary,
             style_.textScale);

    float textY = cardY + style_.headerHeight + pad - 2.0f;
    for (const auto& line : bodyLines) {
        EmitText(overlayVertices_, overlayIndices_, line, cardX + pad, textY, style_.textPrimary, style_.textScale);
        textY += lineStep;
    }

    const float sepY = cardY + cardH - pad - btnH - 10.0f;
    EmitRect(overlayVertices_, overlayIndices_, cardX + pad, sepY, innerW, 1.0f, WithAlpha(style_.windowBorder, 0.45f));

    const float btnGap = 8.0f;
    const float maxBtnW = std::clamp((innerW - btnGap * static_cast<float>(resolvedButtons.size() - 1)) /
                                         static_cast<float>(resolvedButtons.size()),
                                     36.0f, 128.0f);
    const float totalBtnsW = maxBtnW * static_cast<float>(resolvedButtons.size()) +
                             btnGap * static_cast<float>(resolvedButtons.size() > 0 ? resolvedButtons.size() - 1 : 0);
    float bx = cardX + cardW - pad - totalBtnsW;
    const float by = cardY + cardH - pad - btnH + 2.0f;

    int clickedIndex = -1;
    const uint64_t modalHash = HashId(id.empty() ? title : id);

    for (size_t i = 0; i < resolvedButtons.size(); ++i) {
        const UiRect btnRect{bx, by, maxBtnW, btnH};
        const HoverEval btnHv =
            EvalHover(modalHash ^ (0x4D42544EULL + static_cast<uint64_t>(i)), btnRect, true, true, true);
        const bool isPrimary = (i + 1 == resolvedButtons.size());
        const bool pressed = btnHv.hovered && input_.mouseDown;
        if (btnHv.hovered && input_.mouseReleased) {
            clickedIndex = static_cast<int>(i);
        }

        const glm::vec4 baseFill = isPrimary ? WithAlpha(accent, 0.28f) : style_.widgetBg;
        const glm::vec4 hoverFill = isPrimary ? WithAlpha(accent, 0.48f) : style_.widgetHover;
        const glm::vec4 fill = pressed ? style_.widgetActive : glm::mix(baseFill, hoverFill, btnHv.mix);
        const glm::vec4 border = glm::mix(isPrimary ? accent : style_.windowBorder, accent, btnHv.mix);
        EmitRoundedBox(overlayVertices_, overlayIndices_, btnRect.x, btnRect.y, btnRect.w, btnRect.h,
                       style_.widgetRadius, fill, border, 1.0f + 0.35f * btnHv.mix, false);

        const std::string lbl = EllipsizeText(resolvedButtons[i], btnRect.w - 10.0f, style_.textScale);
        const float lblW = MeasureTextWidth(lbl, style_.textScale);
        EmitText(overlayVertices_, overlayIndices_, lbl, btnRect.x + (btnRect.w - lblW) * 0.5f, btnRect.y + 5.0f,
                 style_.textPrimary, style_.textScale);

        bx += maxBtnW + btnGap;
    }

    lastItemRect_ = cardRect;
    return clickedIndex;
}

int ImmediateUI::MessageBox(std::string_view id, std::string_view title, std::string_view message,
                            std::span<const std::string_view> buttons, MessageBoxType type, bool dimBackground) {
    immediateModalDrawnThisFrame_ = true;
    return DrawMessageBoxInternal(id, title, message, buttons, type, dimBackground);
}

void ImmediateUI::ShowMessageBox(std::string_view id, std::string_view title, std::string_view message,
                                 MessageBoxType type, std::span<const std::string_view> buttons,
                                 std::function<void(int)> onResult, bool dimBackground) {
    const std::string resolvedKey = id.empty() ? std::string{title} : std::string{id};
    for (const auto& existing : modalQueue_) {
        if (existing.idKey == resolvedKey) {
            return;
        }
    }

    QueuedMessageBox entry;
    entry.idKey = resolvedKey;
    entry.title = std::string{title};
    entry.message = std::string{message};
    entry.type = type;
    entry.callback = std::move(onResult);
    entry.dimBackground = dimBackground;
    for (std::string_view b : buttons) {
        entry.buttons.emplace_back(b);
    }
    modalQueue_.push_back(std::move(entry));
}

void ImmediateUI::CloseMessageBox(std::string_view id) {
    if (modalQueue_.empty()) {
        return;
    }
    if (id.empty()) {
        modalQueue_.erase(modalQueue_.begin());
        return;
    }
    std::erase_if(modalQueue_, [id](const QueuedMessageBox& m) { return m.idKey == id; });
}

void ImmediateUI::DrawFloatingBadge(float x, float y, std::string_view text, const glm::vec4& accentColor) {
    if (text.empty()) {
        return;
    }
    const float scale = style_.textScale * 0.85f;
    const float textW = MeasureTextWidth(text, scale);
    const float badgeW = std::max(36.0f, textW + 18.0f);
    const float badgeH = 20.0f;
    const UiRect badgeRect{x, y, badgeW, badgeH};
    const HoverEval hv = EvalHover(HashId(text) ^ 0x424447ULL, badgeRect, true, false);
    const glm::vec4 bg = glm::mix(style_.popupBg, style_.widgetHover, hv.mix * 0.45f);
    EmitPanelCard(overlayVertices_, overlayIndices_, badgeRect, 5.0f, bg, accentColor, false, 1.25f + 0.35f * hv.mix);
    EmitRoundedBox(overlayVertices_, overlayIndices_, x + 5.0f, y + 6.0f, 6.0f, 8.0f, 3.0f, accentColor,
                   glm::vec4{0.0f}, 0.0f, false);
    EmitText(overlayVertices_, overlayIndices_, text, x + 14.0f, y + 3.0f, style_.textPrimary, scale);
}

} // namespace bgl::ui
