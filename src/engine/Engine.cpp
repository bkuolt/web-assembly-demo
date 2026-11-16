/**
 * @file Engine.cpp
 * @brief Public bgl::Engine facade implementation delegating to Engine::Impl.
 */
#include "bgl/Engine.hpp"
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"
#include "engine/EngineImpl.hpp"
#include "engine/GuiInternal.hpp"
#include "scene/TextureDecoder.hpp"
#include <utility>

namespace bgl {

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() {
    if (impl_) {
        impl_->Shutdown();
    }
}

Result<std::unique_ptr<Engine>> Engine::Create(const EngineConfig& config) {
    auto engine = std::unique_ptr<Engine>(new Engine());
    engine->impl_->config_ = config;
    engine->impl_->settings_ = config.render;

    engine::EngineCoreConfig coreCfg{};
    coreCfg.width = config.width;
    coreCfg.height = config.height;
    coreCfg.windowTitle = config.title;

    if (auto res = engine->impl_->Initialize(coreCfg); !res) {
        return std::unexpected(res.error());
    }

    auto& ctx = engine->impl_->GetContext();
    if (auto guiRes = engine->impl_->gui_.Initialize(ctx.GetDevice(), ctx.GetQueue(), ctx.GetPreferredFormat(), 1);
        !guiRes) {
        BGL_LOG_WARN("Could not initialize ImmediateUI: {}", guiRes.error());
    }

    engine->impl_->ApplySettings();
    engine::g_registeredEngineGui = &engine->impl_->gui_;
    return engine;
}

bool Engine::PollEvents() {
    return impl_->PollEvents();
}

void Engine::Update(float deltaTime) {
    impl_->Update(deltaTime);
}

void Engine::Render() {
    impl_->Render();
}

bool Engine::IsRunning() const noexcept {
    return impl_->IsRunning();
}

void Engine::Quit() noexcept {
    impl_->RequestClose();
}

Result<void> Engine::LoadModel(std::string_view pathOrUrl) {
    return impl_->LoadSceneFromUri(pathOrUrl);
}

Result<void> Engine::LoadModel(std::span<const uint8_t> data, std::string_view name) {
    return impl_->LoadScene(data, name);
}

Camera& Engine::GetCamera() noexcept {
    return impl_->GetCamera();
}

const Camera& Engine::GetCamera() const noexcept {
    return impl_->GetCamera();
}

void Engine::FrameAll() noexcept {
    impl_->FrameAll();
}

RenderSettings& Engine::Settings() noexcept {
    return impl_->settings_;
}

const RenderSettings& Engine::Settings() const noexcept {
    return impl_->settings_;
}

uint32_t Engine::Width() const noexcept {
    return impl_->GetContext().GetWidth();
}

uint32_t Engine::Height() const noexcept {
    return impl_->GetContext().GetHeight();
}

void Engine::SetResizeCallback(std::function<void(uint32_t width, uint32_t height)> callback) {
    impl_->resizeCb_ = std::move(callback);
}

void Engine::SetFileDropCallback(std::function<void(std::string_view path)> callback) {
    impl_->dropCb_ = std::move(callback);
}

void Engine::BeginGui() {
    impl_->BeginGui();
}

void Engine::EndGui() {
    impl_->EndGui();
}

Result<std::shared_ptr<SpriteTexture>> Engine::LoadSpriteTexture(std::string_view pathOrUrl, bool nearestFilter) {
    if (pathOrUrl.empty()) {
        return std::unexpected("Sprite texture path or URL cannot be empty");
    }
    auto bytesRes = core::NetworkFetcher::FetchSync(pathOrUrl);
    if (!bytesRes) {
        return std::unexpected(bytesRes.error());
    }
    return LoadSpriteTexture(*bytesRes, pathOrUrl, nearestFilter);
}

Result<std::shared_ptr<SpriteTexture>> Engine::LoadSpriteTexture(std::span<const uint8_t> data,
                                                                 std::string_view formatHint, bool nearestFilter) {
    if (data.empty()) {
        return std::unexpected("Sprite texture byte buffer cannot be empty");
    }
    auto imgRes = scene::TextureDecoder::DecodeAuto(data, formatHint);
    if (!imgRes) {
        return std::unexpected(imgRes.error());
    }
    scene::DecodedImage img = std::move(*imgRes);
    if (img.isCompressed) {
        auto rgbaRes = scene::TextureDecoder::DecodeKTX2(data, scene::KtxTranscodeTarget::Rgba32);
        if (!rgbaRes) {
            return std::unexpected(rgbaRes.error());
        }
        img = std::move(*rgbaRes);
    }
    auto tex = core::SpriteTexture::FromRGBA8(img.rgbaData, img.width, img.height, nearestFilter, true);
    if (!tex) {
        return std::unexpected("Decoded sprite texture has invalid dimensions");
    }
    return tex;
}

void Engine::DrawSprite(const Sprite& sprite) {
    impl_->SubmitSprite(sprite);
}

void Engine::DrawBillboard(const Billboard& billboard) {
    impl_->SubmitBillboard(billboard);
}

} // namespace bgl
