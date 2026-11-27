/**
 * @file ApplicationUI.cpp
 * @brief Native in-engine studio overlay panels, telemetry sparklines, model selector, and loading progress UI.
 */
#include "viewer/Application.hpp"
#include <algorithm>
#include <array>
#include <format>
#include <utility>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl::app {

void Application::UpdateSettingsUI() {
    int winW = 1280;
    int winH = 720;
#if defined(BGL_PLATFORM_NATIVE)
    if (nativeWindow_) {
        SDL_GetWindowSize(nativeWindow_, &winW, &winH);
    }
#endif
    const uint32_t vpW = static_cast<uint32_t>(std::max(1, winW));
    const uint32_t vpH = static_cast<uint32_t>(std::max(1, winH));
    settingsUi_.BeginFrame(uiInput_, vpW, vpH, lastDeltaTime_);

    const bool showLoadingUi = IsLoadingScene() || (loadingBannerTimer_ > 0.0f);
    if (showLoadingUi) {
        const float fadeAlpha = (!IsLoadingScene() && loadingBannerTimer_ < 0.45f)
                                    ? std::clamp(loadingBannerTimer_ / 0.45f, 0.0f, 1.0f)
                                    : 1.0f;
        settingsUi_.DrawLoadingBanner(loadingTitle_, loadingVisualProgress_, loadingStatus_, loadingHasError_,
                                      fadeAlpha);
    }

    if (!showSettingsUi_) {
        settingsUi_.EndFrame();
        return;
    }

    settingsUi_.Style().accent = glm::vec4{uiAccentColor_, 1.0f};
    settingsUi_.Style().sparklineFill = glm::vec4{uiAccentColor_, 0.24f};

    DrawSelectionFloatingBadge(vpW, vpH);

    settingsUi_.BeginWindow("bgl Engine Studio", std::max(16.0f, static_cast<float>(vpW) - 356.0f), 14.0f, 340.0f);

    bgl::ui::UI::BeginGroup("Telemetry", true);
    const double totalMb = static_cast<double>(lastVramBytes_) / (1024.0 * 1024.0);
    const double texMb = static_cast<double>(lastTextureVramBytes_) / (1024.0 * 1024.0);
    bgl::ui::UI::Label(std::format("VRAM: {:.1f} MB (Tex: {:.1f} MB)", totalMb, texMb),
                       glm::vec4{0.72f, 0.86f, 0.98f, 1.0f});
    if (fpsHistoryCount_ >= 2) {
        bgl::ui::UI::Sparkline("Frame Rate (FPS)", std::span<const float>{fpsHistory_.data(), fpsHistoryCount_}, 0.0f,
                               0.0f, 34.0f, glm::vec4{uiAccentColor_, 1.0f});
    }
    bgl::ui::UI::EndGroup();

    DrawModelSelectorSection(showLoadingUi);
    bgl::ui::UI::Separator();

    const float sectionsMaxH = std::max(240.0f, static_cast<float>(vpH) - 240.0f);
    bgl::ui::UI::BeginScrollArea("studio_sections_scroll", sectionsMaxH);
    DrawCameraSceneSection();
    DrawRayTracingSection();
    DrawPostProcessSection();
    DrawAtmosphereWeatherSection();
    DrawEngineThemeSection();
    bgl::ui::UI::EndScrollArea();

    settingsUi_.EndWindow();
    settingsUi_.EndFrame();
}

void Application::DrawSelectionFloatingBadge(uint32_t vpW, uint32_t vpH) {
    if (!showSelectionAnchors_ || !selectedNode_ ||
        selectedNode_->worldAABB.minBound.x > selectedNode_->worldAABB.maxBound.x) {
        return;
    }
    const glm::vec3 center = selectedNode_->worldAABB.Center();
    const glm::vec4 clip = camera_.GetViewProjectionMatrix() * glm::vec4{center, 1.0f};
    if (clip.w <= 0.001f) {
        return;
    }
    const glm::vec3 ndc = glm::vec3{clip} / clip.w;
    if (ndc.z < 0.0f || ndc.z > 1.0f || std::abs(ndc.x) > 0.95f || std::abs(ndc.y) > 0.95f) {
        return;
    }
    const float sx = (ndc.x * 0.5f + 0.5f) * static_cast<float>(vpW);
    const float sy = (0.5f - ndc.y * 0.5f) * static_cast<float>(vpH);
    const float dist = glm::length(camera_.GetPosition() - center);
    const std::string nodeTitle = selectedNode_->name.empty() ? "Selected" : selectedNode_->name;
    settingsUi_.DrawFloatingBadge(sx + 8.0f, sy - 24.0f, std::format("{} ({:.1f}m)", nodeTitle, dist),
                                  glm::vec4{1.0f, 0.72f, 0.12f, 0.95f});
}

void Application::DrawModelSelectorSection(bool showLoadingUi) {
    const auto presets = GetBuiltinModelPresets();
    std::vector<const char*> modelLabels;
    modelLabels.reserve(presets.size() + 1);
    for (const auto& preset : presets) {
        modelLabels.push_back(preset.label);
    }
    if (!customModelLabel_.empty()) {
        modelLabels.push_back(customModelLabel_.c_str());
    }

    if (bgl::ui::UI::Dropdown("glTF Model", modelLabels, selectedModelOption_)) {
        if (selectedModelOption_ < presets.size()) {
            const auto& preset = presets[selectedModelOption_];
            const std::string uri = ResolveModelPresetUri(preset);
            if (!uri.empty()) {
                LoadSceneFromUriAsync(uri, preset.label ? std::string_view{preset.label} : std::string_view{});
            }
        }
    }

    if (showLoadingUi) {
        const glm::vec4 progressColor =
            loadingHasError_
                ? glm::vec4{0.92f, 0.28f, 0.34f, 1.0f}
                : ((loadingVisualProgress_ >= 0.999f) ? glm::vec4{0.22f, 0.82f, 0.52f, 1.0f} : glm::vec4{0.0f});
        bgl::ui::UI::ProgressBar(loadingVisualProgress_, loadingStatus_, 18.0f, progressColor);
    }
}

void Application::DrawCameraSceneSection() {
    if (!bgl::ui::UI::CollapsingHeader("Camera & Scene", true)) {
        return;
    }
    static constexpr const char* kViewModeNames[] = {
        "Lit PBR", "LOD Heatmap", "Normals", "Metal/Rough/AO", "UV Checker", "Clay + Wireframe", "Toon / Cel",
    };
    size_t viewIdx = static_cast<size_t>(std::to_underlying(pipelineManager_.GetDebugViewMode()));
    if (bgl::ui::UI::Dropdown("Shading View", kViewModeNames, viewIdx)) {
        pipelineManager_.SetDebugViewMode(static_cast<gpu::DebugViewMode>(viewIdx));
    }

    bgl::ui::UI::BeginContainer(
        "cam_quick_actions",
        bgl::ui::ContainerOptions{
            .layout = bgl::ui::ContainerLayout::Horizontal, .padding = 0.0f, .spacing = 6.0f, .columns = 2});
    if (bgl::ui::UI::Button("Frame All [F]")) {
        FrameAll();
    }
    bgl::ui::UI::Tooltip("Reset camera to fit the entire scene");
    if (bgl::ui::UI::Button(turntableActive_ ? "Stop Orbit [T]" : "Turntable [T]")) {
        turntableActive_ = !turntableActive_;
    }
    bgl::ui::UI::Tooltip("Toggle automatic orbital turntable camera");
    bgl::ui::UI::EndContainer();

    bool wf = wireframeMode_;
    if (bgl::ui::UI::ToggleBox("Wireframe Overlay [F1]", wf)) {
        wireframeMode_ = wf;
    }
    bool aabbs = showBoundingBoxes_;
    if (bgl::ui::UI::Checkbox("Bounding Boxes [B]", aabbs)) {
        showBoundingBoxes_ = aabbs;
    }
    bool anchors = showSelectionAnchors_;
    if (bgl::ui::UI::Checkbox("3D Selection Badge", anchors)) {
        showSelectionAnchors_ = anchors;
    }

    if (activeScene_ && !activeScene_->allNodes.empty()) {
        bgl::ui::UI::Label("Scene Nodes (Scroll):", settingsUi_.Style().textMuted);
        bgl::ui::UI::BeginScrollArea("scene_hierarchy_scroll", 104.0f);
        size_t shownCount = 0;
        for (size_t ni = 0; ni < activeScene_->allNodes.size() && shownCount < 48; ++ni) {
            const auto& node = activeScene_->allNodes[ni];
            if (!node || node->GetPrimitives().empty()) {
                continue;
            }
            ++shownCount;
            const bool isSel = (selectedNodeIndex_ == static_cast<int>(ni));
            const std::string btnText =
                std::format("{}{} [{}]", isSel ? "* " : "", node->name.empty() ? "MeshNode" : node->name, ni);
            if (bgl::ui::UI::Button(btnText)) {
                SelectNodeByIndex(static_cast<int>(ni));
            }
        }
        bgl::ui::UI::EndScrollArea();
    }
}

void Application::DrawRayTracingSection() {
    if (!bgl::ui::UI::CollapsingHeader("Desktop Ray Tracing", true)) {
        return;
    }
    if (!pipelineManager_.IsDesktopRayTracingSupported()) {
        bgl::ui::UI::Label("Unavailable on this WebGPU adapter", settingsUi_.Style().textMuted);
        return;
    }

    const auto rtStats = pipelineManager_.GetRtPassStats();
    bgl::ui::UI::Label(std::format("BVH: {} tris, {} BLAS, {:.2f} ms", rtStats.sceneStats.triangleCount,
                                   rtStats.sceneStats.blasCount, rtStats.lastGpuMs),
                       glm::vec4{0.42f, 0.88f, 0.98f, 1.0f});

    bool rtOn = pipelineManager_.IsRayTracingEnabled();
    if (bgl::ui::UI::Checkbox("Enable Hybrid Ray Tracing [Ctrl+R]", rtOn)) {
        pipelineManager_.SetRayTracingEnabled(rtOn);
    }
    if (!rtOn) {
        return;
    }

    bool ptOn = pipelineManager_.IsRtPathTracingEnabled();
    if (bgl::ui::UI::Checkbox("Path Tracer GI Mode [Ctrl+P]", ptOn)) {
        pipelineManager_.SetRtPathTracingEnabled(ptOn);
    }
    bgl::ui::UI::Tooltip("Progressive multi-bounce compute path tracer (accumulates when camera is still)");

    if (ptOn) {
        bgl::ui::UI::Label(std::format("Path Samples: {} spp ({})", rtStats.pathSamples,
                                       rtStats.bindlessActive ? "Bindless Tex" : "Material Factors"),
                           glm::vec4{0.35f, 0.92f, 0.72f, 1.0f});
        float bouncesF = static_cast<float>(pipelineManager_.GetRtPathBounces());
        if (bgl::ui::UI::SliderFloat("Max Bounces", bouncesF, 1.0f, 6.0f, "{:.0f}")) {
            pipelineManager_.SetRtPathBounces(static_cast<uint32_t>(std::lround(bouncesF)));
        }
        if (bgl::ui::UI::Button("Reset Path Accumulation")) {
            pipelineManager_.ResetRtPathAccumulation();
        }
        return;
    }

    bool rtHalf = pipelineManager_.IsRtHalfResEnabled();
    if (bgl::ui::UI::Checkbox("Half-Res Trace + Bilateral Denoise", rtHalf)) {
        pipelineManager_.SetRtHalfResEnabled(rtHalf);
    }

    bool rtShd = pipelineManager_.IsRtShadowsEnabled();
    if (bgl::ui::UI::Checkbox("Ray-Traced Soft Shadows", rtShd)) {
        pipelineManager_.SetRtShadowsEnabled(rtShd);
    }
    if (rtShd) {
        float soft = pipelineManager_.GetRtShadowSoftness();
        if (bgl::ui::UI::SliderFloat("Sun Penumbra Softness", soft, 0.0f, 0.12f, "{:.3f}")) {
            pipelineManager_.SetRtShadowSoftness(soft);
        }
    }

    bool rtAo = pipelineManager_.IsRtAoEnabled();
    if (bgl::ui::UI::Checkbox("Ray-Traced Ambient Occlusion", rtAo)) {
        pipelineManager_.SetRtAoEnabled(rtAo);
    }
    if (rtAo) {
        float rad = pipelineManager_.GetRtAoRadius();
        if (bgl::ui::UI::SliderFloat("RTAO World Radius", rad, 0.1f, 3.0f, "{:.2f}m")) {
            pipelineManager_.SetRtAoRadius(rad);
        }
    }

    bool rtRefl = pipelineManager_.IsRtReflectionsEnabled();
    if (bgl::ui::UI::Checkbox("Ray-Traced GGX Reflections", rtRefl)) {
        pipelineManager_.SetRtReflectionsEnabled(rtRefl);
    }
    if (rtRefl) {
        float rInt = pipelineManager_.GetRtReflectionIntensity();
        if (bgl::ui::UI::SliderFloat("RT Reflection Intensity", rInt, 0.0f, 2.0f, "{:.2f}")) {
            pipelineManager_.SetRtReflectionIntensity(rInt);
        }
    }
}

void Application::DrawPostProcessSection() {
    if (!bgl::ui::UI::CollapsingHeader("Post-Processing & Lens", false)) {
        return;
    }
    auto& pp = pipelineManager_.GetPostProcessPass();

    static constexpr const char* kTonemapNames[] = {"Neutral", "AgX", "ACES"};
    size_t tmIdx = static_cast<size_t>(std::to_underlying(pp.GetToneMapOperator()));
    if (bgl::ui::UI::OptionGroup("Tone Mapper", kTonemapNames, tmIdx, true)) {
        pp.SetToneMapOperator(static_cast<gpu::ToneMapOperator>(tmIdx));
    }

    static constexpr const char* kGradingNames[] = {"Neutral", "Warm Cinema", "Cool Sci-Fi", "Bleach Bypass", "Noir"};
    size_t gradeIdx = static_cast<size_t>(std::to_underlying(pp.GetColorGradingPreset()));
    if (bgl::ui::UI::Dropdown("Color Grading", kGradingNames, gradeIdx)) {
        pp.SetColorGradingPreset(static_cast<gpu::ColorGradingPreset>(gradeIdx));
    }

    float exposure = pp.GetExposure();
    if (bgl::ui::UI::SliderFloat("Exposure (EV)", exposure, 0.2f, 4.0f, "{:.2f}")) {
        pp.SetExposure(exposure);
    }
    bool autoExp = pp.IsAutoExposureEnabled();
    if (bgl::ui::UI::ToggleBox("GPU Auto-Exposure", autoExp, "Histogram luminance adaptation")) {
        pp.SetAutoExposureEnabled(autoExp);
    }
    bool fxaa = pp.IsFxaaEnabled();
    if (bgl::ui::UI::ToggleBox("FXAA Edge AA", fxaa)) {
        pp.SetFxaaEnabled(fxaa);
    }

    bool bloom = pp.IsBloomEnabled();
    if (bgl::ui::UI::ToggleBox("5-Mip HDR Bloom [Z]", bloom)) {
        pp.SetBloomEnabled(bloom);
    }
    if (bloom) {
        float bloomInt = pp.GetBloomIntensity();
        if (bgl::ui::UI::SliderFloat("Bloom Intensity", bloomInt, 0.0f, 2.5f, "{:.2f}")) {
            pp.SetBloomIntensity(bloomInt);
        }
    }

    bool gtao = pp.IsSsaoEnabled();
    if (bgl::ui::UI::Checkbox("Depth GTAO Occlusion [G]", gtao)) {
        pp.SetSsaoEnabled(gtao);
    }
    if (gtao) {
        float gtaoInt = pp.GetSsaoIntensity();
        if (bgl::ui::UI::SliderFloat("GTAO Strength", gtaoInt, 0.0f, 1.5f, "{:.2f}")) {
            pp.SetSsaoIntensity(gtaoInt);
        }
    }

    bool ssr = pp.IsSsrEnabled();
    if (bgl::ui::UI::Checkbox("Screen-Space Reflections", ssr)) {
        pp.SetSsrEnabled(ssr);
    }
    bool rays = pp.IsGodRaysEnabled();
    if (bgl::ui::UI::Checkbox("Volumetric Sun God Rays", rays)) {
        pp.SetGodRaysEnabled(rays);
    }
    bool dof = pp.IsDoFEnabled();
    if (bgl::ui::UI::Checkbox("Bokeh Depth of Field", dof)) {
        pp.SetDoFEnabled(dof);
    }
    if (dof) {
        float fDist = pp.GetDoFFocusDistance();
        if (bgl::ui::UI::SliderFloat("DoF Focus Dist", fDist, 0.5f, 40.0f, "{:.1f}m")) {
            pp.SetDoFFocusDistance(fDist);
        }
        float fRange = pp.GetDoFFocusRange();
        if (bgl::ui::UI::SliderFloat("DoF Range", fRange, 1.0f, 20.0f, "{:.1f}m")) {
            pp.SetDoFFocusRange(fRange);
        }
    }

    float vign = pp.GetVignetteIntensity();
    if (bgl::ui::UI::SliderFloat("Vignette", vign, 0.0f, 1.0f, "{:.2f}")) {
        pp.SetVignetteIntensity(vign);
    }
}

void Application::DrawAtmosphereWeatherSection() {
    if (!bgl::ui::UI::CollapsingHeader("Atmosphere & Weather", false)) {
        return;
    }
    const gpu::EnvironmentPreset activePreset = pipelineManager_.GetEnvironmentPreset();
    if (activePreset == gpu::EnvironmentPreset::CustomHdr) {
        static constexpr const char* kEnvNamesCustom[] = {"Mediterranean Courtyard", "Sunset Studio",
                                                          "Overcast Atelier", "Night Neon", "Custom HDR"};
        size_t envIdx = 4;
        if (bgl::ui::UI::Dropdown("Environment Preset", kEnvNamesCustom, envIdx) && envIdx < 4) {
            pipelineManager_.SetEnvironmentPreset(static_cast<gpu::EnvironmentPreset>(envIdx), activeScene_.get());
            skybox_.SetEnvironmentCubemap(pipelineManager_.GetEnvironmentCubemapView(),
                                          pipelineManager_.GetDefaultSampler());
        }
    } else {
        static constexpr const char* kEnvNames[] = {"Mediterranean Courtyard", "Sunset Studio", "Overcast Atelier",
                                                    "Night Neon"};
        size_t envIdx = std::min(static_cast<size_t>(std::to_underlying(activePreset)), size_t{3});
        if (bgl::ui::UI::Dropdown("Environment Preset", kEnvNames, envIdx)) {
            pipelineManager_.SetEnvironmentPreset(static_cast<gpu::EnvironmentPreset>(envIdx), activeScene_.get());
            skybox_.SetEnvironmentCubemap(pipelineManager_.GetEnvironmentCubemapView(),
                                          pipelineManager_.GetDefaultSampler());
        }
    }

    float tod = timeOfDay_;
    if (bgl::ui::UI::SliderFloat("Time of Day", tod, 0.0f, 24.0f, "{:.1f}h")) {
        SetTimeOfDay(tod);
    }
    bool dn = dayNightCycleActive_;
    if (bgl::ui::UI::Checkbox("Animate Sun Cycle", dn)) {
        dayNightCycleActive_ = dn;
    }

    static constexpr const char* kWeatherNames[] = {"Clear Sky", "Rain + Puddles", "Snowfall", "Dense Fog",
                                                    "Volumetric Smoke"};
    size_t wIdx = static_cast<size_t>(std::to_underlying(pipelineManager_.GetWeatherMode()));
    if (bgl::ui::UI::Dropdown("Weather Mode", kWeatherNames, wIdx)) {
        pipelineManager_.SetWeatherMode(static_cast<gpu::WeatherMode>(wIdx));
    }
    float wInt = pipelineManager_.GetWeatherIntensity();
    if (bgl::ui::UI::SliderFloat("Weather Intensity", wInt, 0.0f, 1.5f, "{:.2f}")) {
        pipelineManager_.SetWeatherIntensity(wInt);
    }
}

void Application::DrawEngineThemeSection() {
    if (!bgl::ui::UI::CollapsingHeader("Engine & UI Theme", false)) {
        return;
    }
    static constexpr const char* kKtxOptions[] = {
        "Auto (Hardware)", "BC7 (Desktop)", "ASTC 4x4 (Mobile)", "ETC2 RGBA", "RGBA32 (Uncompressed)",
    };
    if (bgl::ui::UI::Dropdown("KTX2 Compression", kKtxOptions, selectedKtxOption_)) {
        scene::KtxTranscodeTarget target = scene::KtxTranscodeTarget::Rgba32;
        if (selectedKtxOption_ == 0) {
            if (gpuContext_.SupportsBC()) {
                target = scene::KtxTranscodeTarget::Bc7;
            } else if (gpuContext_.SupportsASTC()) {
                target = scene::KtxTranscodeTarget::Astc4x4;
            } else if (gpuContext_.SupportsETC2()) {
                target = scene::KtxTranscodeTarget::Etc2Rgba;
            }
        } else {
            static constexpr std::array<scene::KtxTranscodeTarget, 4> kExplicitTargets{
                scene::KtxTranscodeTarget::Bc7, scene::KtxTranscodeTarget::Astc4x4, scene::KtxTranscodeTarget::Etc2Rgba,
                scene::KtxTranscodeTarget::Rgba32};
            if (selectedKtxOption_ - 1 < kExplicitTargets.size()) {
                target = kExplicitTargets[selectedKtxOption_ - 1];
            }
        }
        SetKtxCompressionTarget(target);
    }

    bool mipmaps = pipelineManager_.IsMipmappingEnabled();
    if (bgl::ui::UI::Checkbox("Anisotropic Mipmapping", mipmaps)) {
        ToggleMipmapping();
    }
    bgl::ui::UI::Tooltip("Filter distant textures with trilinear + anisotropic mipmaps");
    bool shadows = pipelineManager_.IsShadowsEnabled();
    if (bgl::ui::UI::Checkbox("Soft Poisson Shadows", shadows)) {
        ToggleShadows();
    }
    bgl::ui::UI::Tooltip("Directional 2048x2048 shadow map with 12-tap rotated Poisson PCF");
    bool hoverEffects = settingsUi_.Style().enableHoverEffects;
    if (bgl::ui::UI::ToggleBox("UI Hover Effects", hoverEffects, "Smooth hover highlights across widgets")) {
        settingsUi_.Style().enableHoverEffects = hoverEffects;
    }
    bgl::ui::UI::Tooltip("Toggle animated hover highlights across all UI widgets");
    static constexpr const char* kFontLabels[]{"Embedded (Mono)", "System Sans", "System Mono", "System Serif"};
    static constexpr const char* kFontQueries[]{"embedded", "sans", "mono", "serif"};
    size_t candidateFontIdx = selectedUiFontIdx_;
    if (bgl::ui::UI::Dropdown("UI Font", kFontLabels, candidateFontIdx)) {
        if (candidateFontIdx < std::size(kFontQueries) &&
            settingsUi_.SetDefaultFont(kFontQueries[candidateFontIdx], 24.0f)) {
            selectedUiFontIdx_ = candidateFontIdx;
        }
    }
    bgl::ui::UI::Tooltip("Discover and apply installed OS fonts via FontLibrary");
    bgl::ui::UI::ColorEdit3("UI Accent Color", uiAccentColor_);
    if (bgl::ui::UI::Button("UI Modal Demo")) {
        constexpr const std::string_view kModalButtons[]{"Close", "Toggle Hover"};
        settingsUi_.ShowMessageBox(
            "viewer_ui_info", "Frosted-Glass Modal",
            "ImmediateUI supports animated hover highlights, cross-platform font discovery, and frosted-glass modals.",
            bgl::ui::MessageBoxType::Info, kModalButtons, [this](int btn) {
                if (btn == 1) {
                    settingsUi_.Style().enableHoverEffects = !settingsUi_.Style().enableHoverEffects;
                }
            });
    }
    bgl::ui::UI::Tooltip("Open a sample frosted-glass modal dialog");
}

} // namespace bgl::app
