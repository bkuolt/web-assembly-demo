/**
 * @file FastGltfLoader.cpp
 * @brief glTF 2.0 / 2.1 ingestion orchestrator: fetching, buffer resolution, stage scheduling, and scene assembly.
 */
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"
#include "scene/FastGltfInternal.hpp"
#include <algorithm>
#include <filesystem>
#include <format>
#include <unordered_set>

namespace bgl::scene {

namespace {

core::Result<void> ResolveExternalBuffers(fastgltf::Asset& asset, std::string_view baseUri,
                                          std::vector<std::vector<uint8_t>>& externalBufferStorage,
                                          size_t& outTotalBufferBytes, detail::ProgressReporter* reporter) {
    externalBufferStorage.resize(asset.buffers.size());
    outTotalBufferBytes = 0;

    BGL_LOG_INFO("[glTF Loader] --- Buffers ({} total) ---", asset.buffers.size());
    for (size_t bIdx{0}; bIdx < asset.buffers.size(); ++bIdx) {
        if (reporter && reporter->IsCancelled()) {
            return std::unexpected("Loading cancelled");
        }
        auto& buf = asset.buffers[bIdx];
        outTotalBufferBytes += buf.byteLength;
        const char* typeStr{"Unknown"};
        if (std::holds_alternative<fastgltf::sources::Array>(buf.data)) {
            typeStr = "Array";
        } else if (std::holds_alternative<fastgltf::sources::ByteView>(buf.data)) {
            typeStr = "ByteView";
        } else if (std::holds_alternative<fastgltf::sources::Vector>(buf.data)) {
            typeStr = "Vector";
        } else if (std::holds_alternative<fastgltf::sources::URI>(buf.data)) {
            typeStr = "External URI";
            const auto& uriSource = std::get<fastgltf::sources::URI>(buf.data);
            const std::string rawUriStr{uriSource.uri.string()};
            auto resolvedRes{core::NetworkFetcher::ResolveExternalAssetUri(baseUri, rawUriStr)};
            if (!resolvedRes) {
                return std::unexpected("Blocked or invalid external glTF buffer URI '" + rawUriStr +
                                       "': " + resolvedRes.error());
            }
            const std::string resolvedUri{std::move(*resolvedRes)};
            BGL_LOG_INFO("  * Buffer #{}: Resolving external URI '{}' -> '{}'", bIdx, rawUriStr, resolvedUri);

            auto fetchRes =
                core::NetworkFetcher::FetchSync(resolvedUri, [reporter](const core::FetchProgress&) -> bool {
                    return !reporter || !reporter->IsCancelled();
                });
            if (!fetchRes) {
                return std::unexpected("Failed to fetch external glTF buffer '" + resolvedUri +
                                       "': " + fetchRes.error());
            }
            externalBufferStorage[bIdx] = std::move(*fetchRes);
            const size_t offset{std::min(uriSource.fileByteOffset, externalBufferStorage[bIdx].size())};
            const size_t avail{externalBufferStorage[bIdx].size() - offset};
            buf.data = fastgltf::sources::ByteView{
                .bytes =
                    fastgltf::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(externalBufferStorage[bIdx].data() + offset), avail},
                .mimeType = uriSource.mimeType};
        }

        BGL_LOG_DEBUG("  * Buffer #{}: {} bytes ({:.2f} MB), Source: {}", bIdx, buf.byteLength,
                      static_cast<double>(buf.byteLength) / (1024.0 * 1024.0), typeStr);
    }

    BGL_LOG_INFO("[glTF Loader] --- BufferViews ({} total) ---", asset.bufferViews.size());
    for (size_t bvIdx = 0; bvIdx < asset.bufferViews.size(); ++bvIdx) {
        const auto& bv = asset.bufferViews[bvIdx];
        const char* targetStr = "None";
        if (bv.target == fastgltf::BufferTarget::ArrayBuffer) {
            targetStr = "ArrayBuffer (Vertices)";
        } else if (bv.target == fastgltf::BufferTarget::ElementArrayBuffer) {
            targetStr = "ElementArrayBuffer (Indices)";
        }

        BGL_LOG_DEBUG("  * BufferView #{}: Buffer #{}, Offset={}, Length={}, Stride={}, Target={}", bvIdx,
                      bv.bufferIndex, bv.byteOffset, bv.byteLength, bv.byteStride.value_or(0), targetStr);
    }
    return {};
}

void LogSceneSummary(const fastgltf::Asset& asset, const Scene& scene, size_t totalBufferBytes) {
    BGL_LOG_INFO("--------------------------------------------------------------------------------");
    BGL_LOG_INFO("[glTF Loader] Scene Graph Tree Diagram (Root Scene):");
    std::unordered_set<const SceneNode*> visited{};
    constexpr size_t kMaxPrintDepth{128u};
    auto printTree = [&](auto& self, const std::shared_ptr<SceneNode>& n, const std::string& prefix, bool isLast,
                         size_t depth) -> void {
        if (!n || depth > kMaxPrintDepth || !visited.insert(n.get()).second) {
            return;
        }
        std::string nodeInfo{n->name.empty() ? "(Unnamed Node)" : n->name};
        auto prims = n->GetPrimitives();
        if (!prims.empty()) {
            size_t totalVerts{0};
            size_t totalIndices{0};
            for (const auto& p : prims) {
                totalVerts += p.vertices.size();
                totalIndices += p.indices.size();
            }
            nodeInfo += " [Mesh: " + std::to_string(prims.size()) + " prims, " + std::to_string(totalVerts) +
                        " verts, " + std::to_string(totalIndices / 3) + " tris]";
        }
        BGL_LOG_INFO("{}{}-- {}", prefix, isLast ? "`" : "|", nodeInfo);
        for (size_t c{0}; c < n->children.size(); ++c) {
            const bool childLast{c + 1 == n->children.size()};
            self(self, n->children[c], prefix + (isLast ? "   " : "|  "), childLast, depth + 1u);
        }
    };
    for (size_t r{0}; r < scene.rootNodes.size(); ++r) {
        printTree(printTree, scene.rootNodes[r], "  ", (r + 1 == scene.rootNodes.size()), 0u);
    }
    BGL_LOG_INFO("--------------------------------------------------------------------------------");

    const glm::vec3 minB = scene.sceneAABB.minBound;
    const glm::vec3 maxB = scene.sceneAABB.maxBound;
    const glm::vec3 center = scene.sceneAABB.Center();
    const glm::vec3 extents = scene.sceneAABB.Extents();
    const float radius = glm::length(extents);

    BGL_LOG_INFO("[glTF Loader] Ingestion Finished Successfully:");
    BGL_LOG_INFO("  * Total Nodes:        {}", asset.nodes.size());
    BGL_LOG_INFO("  * Total Meshes:       {}", asset.meshes.size());
    BGL_LOG_INFO("  * Total Materials:    {}", scene.materials.size());
    BGL_LOG_INFO("  * Total Textures:     {}", scene.cpuImages.size());
    BGL_LOG_INFO("  * Total Binary Size:  {} bytes ({:.2f} MB)", totalBufferBytes,
                 static_cast<double>(totalBufferBytes) / (1024.0 * 1024.0));
    BGL_LOG_INFO("  * Scene Bounds (min): [{:.4f}, {:.4f}, {:.4f}]", minB.x, minB.y, minB.z);
    BGL_LOG_INFO("  * Scene Bounds (max): [{:.4f}, {:.4f}, {:.4f}]", maxB.x, maxB.y, maxB.z);
    BGL_LOG_INFO("  * Scene Center:       [{:.4f}, {:.4f}, {:.4f}]", center.x, center.y, center.z);
    BGL_LOG_INFO("  * Scene Extents:      [{:.4f}, {:.4f}, {:.4f}] (Radius: {:.4f})", extents.x, extents.y, extents.z,
                 radius);
    BGL_LOG_INFO("================================================================================");
}

} // namespace

FastGltfLoader::FastGltfLoader(AssetManager& assetManager)
    : assetManager_(assetManager),
      parser_(fastgltf::Extensions::EXT_texture_webp | fastgltf::Extensions::KHR_texture_basisu |
              fastgltf::Extensions::KHR_texture_transform | fastgltf::Extensions::KHR_lights_punctual |
              fastgltf::Extensions::KHR_materials_clearcoat | fastgltf::Extensions::KHR_materials_transmission |
              fastgltf::Extensions::KHR_materials_sheen | fastgltf::Extensions::KHR_materials_specular |
              fastgltf::Extensions::KHR_materials_ior | fastgltf::Extensions::KHR_materials_iridescence |
              fastgltf::Extensions::KHR_materials_anisotropy | fastgltf::Extensions::KHR_materials_volume |
              fastgltf::Extensions::KHR_materials_emissive_strength | fastgltf::Extensions::KHR_materials_unlit |
              fastgltf::Extensions::EXT_mesh_gpu_instancing) {}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromMemory(std::span<const uint8_t> data,
                                                                    std::string_view baseUri) {
    return LoadFromMemory(data, baseUri, {}, 0.0f);
}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromMemory(std::span<const uint8_t> data,
                                                                    std::string_view baseUri,
                                                                    LoadProgressCallback progressCb,
                                                                    float baseProgress) {
    fastgltf::GltfDataBuffer dataBuffer{};
    if (!dataBuffer.copyBytes(data.data(), data.size())) {
        return std::unexpected("Failed to copy bytes into fastgltf::GltfDataBuffer");
    }
    return ParseAndAssembleScene(dataBuffer, baseUri, data.size(), std::move(progressCb), baseProgress);
}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::ParseAndAssembleScene(fastgltf::GltfDataBuffer& dataBuffer,
                                                                           std::string_view baseUri, size_t totalBytes,
                                                                           LoadProgressCallback progressCb,
                                                                           float baseProgress) {
    BGL_LOG_INFO("================================================================================");
    BGL_LOG_INFO("[glTF Loader] Starting Ingestion of Asset: '{}' (Total size: {} bytes / {:.2f} MB)",
                 baseUri.empty() ? "In-Memory Stream" : baseUri, totalBytes,
                 static_cast<double>(totalBytes) / (1024.0 * 1024.0));
    BGL_LOG_INFO("================================================================================");

    const float clampedBase = std::clamp(baseProgress, 0.0f, 0.9f);
    detail::ProgressReporter reporter{std::move(progressCb), clampedBase, 0.94f - clampedBase};
    if (!reporter.ReportLocal(0.02f, "Parsing glTF structure...")) {
        return std::unexpected("Loading cancelled");
    }

    // Keep external buffer loading disabled on fastgltf parser so every external URI passes through
    // ResolveExternalBuffers -> NetworkFetcher::ResolveExternalAssetUri() containment and SSRF validation.
    std::filesystem::path baseDir{std::filesystem::current_path()};
    const fastgltf::Options parseOptions{fastgltf::Options::LoadGLBBuffers | fastgltf::Options::DecomposeNodeMatrices};
    if (!baseUri.empty() && !core::NetworkFetcher::IsUrl(baseUri)) {
        const std::filesystem::path localParent{std::filesystem::path{baseUri}.parent_path()};
        if (!localParent.empty()) {
            baseDir = localParent;
        }
    }

    auto gltf = parser_.loadGltf(&dataBuffer, baseDir, parseOptions);
    if (gltf.error() != fastgltf::Error::None) {
        return std::unexpected(std::string{"fastgltf failed to parse glTF/GLB data: "} +
                               std::string{fastgltf::getErrorName(gltf.error())});
    }

    if (!reporter.ReportLocal(0.08f, "Resolving binary buffers...")) {
        return std::unexpected("Loading cancelled");
    }

    auto& asset = gltf.get();
    auto scene = std::make_shared<Scene>();

    std::vector<std::vector<uint8_t>> externalBufferStorage{};
    size_t totalBufferBytes{0};
    if (auto res = ResolveExternalBuffers(asset, baseUri, externalBufferStorage, totalBufferBytes, &reporter); !res) {
        return std::unexpected(res.error());
    }

    if (!reporter.ReportLocal(0.12f, std::format("Decoding {} texture(s)...", asset.images.size()))) {
        return std::unexpected("Loading cancelled");
    }
    detail::DecodeImagesAndTextures(asset, baseUri, assetManager_, *scene, &reporter);
    if (reporter.IsCancelled()) {
        return std::unexpected("Loading cancelled");
    }

    if (!reporter.ReportLocal(0.56f, "Parsing materials & lights...")) {
        return std::unexpected("Loading cancelled");
    }
    detail::ParseMaterialsAndLogStructure(asset, *scene);
    detail::ParsePunctualLights(asset, *scene);

    if (!reporter.ReportLocal(0.58f, std::format("Building {} mesh(es) & LODs...", asset.meshes.size()))) {
        return std::unexpected("Loading cancelled");
    }
    detail::ParseMeshesAndGenerateLods(asset, *scene, &reporter);
    if (reporter.IsCancelled()) {
        return std::unexpected("Loading cancelled");
    }

    if (!reporter.ReportLocal(0.94f, "Assembling scene hierarchy & animations...")) {
        return std::unexpected("Loading cancelled");
    }
    detail::BuildSceneHierarchy(asset, *scene);
    detail::ParseCamerasSkinsAndAnimations(asset, *scene);
    LogSceneSummary(asset, *scene, totalBufferBytes);
    (void)reporter.ReportLocal(1.0f, "Preparing GPU buffers & textures...");

    return scene;
}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromFile(std::string_view path) {
    return LoadFromFile(path, {});
}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromFile(std::string_view path,
                                                                  LoadProgressCallback progressCb) {
    if (!progressCb) {
        const std::string cleanPath{core::NetworkFetcher::SanitizeUri(path)};
        std::error_code ec{};
        if (!cleanPath.empty() && !core::NetworkFetcher::IsUrl(cleanPath) &&
            std::filesystem::is_regular_file(cleanPath, ec)) {
            constexpr uintmax_t kMaxLocalGltfBytes{2ULL * 1024ULL * 1024ULL * 1024ULL};
            const uintmax_t fileSize{std::filesystem::file_size(cleanPath, ec)};
            if (ec || fileSize == 0 || fileSize > kMaxLocalGltfBytes) {
                return std::unexpected("glTF file is empty or exceeds maximum allowed local asset size");
            }
            fastgltf::GltfDataBuffer dataBuffer{};
            if (dataBuffer.loadFromFile(std::filesystem::path(cleanPath))) {
                auto sp = static_cast<fastgltf::span<std::byte>>(dataBuffer);
                return ParseAndAssembleScene(dataBuffer, cleanPath, sp.size());
            }
        }
        auto fetchRes = core::NetworkFetcher::FetchSync(path);
        if (!fetchRes) {
            return std::unexpected("Failed to fetch glTF file: " + fetchRes.error());
        }
        return LoadFromMemory(*fetchRes, path);
    }

    const bool isRemote = core::NetworkFetcher::IsHttpUrl(path);
    const float fetchSpan = isRemote ? 0.45f : 0.15f;
    const std::string displayName = core::NetworkFetcher::ExtractDisplayName(path);

    auto fetchRes = core::NetworkFetcher::FetchSync(path, [&](const core::FetchProgress& fp) -> bool {
        const float localFrac =
            (fp.totalBytes > 0)
                ? std::clamp(static_cast<float>(fp.bytesReceived) / static_cast<float>(fp.totalBytes), 0.0f, 1.0f)
                : 0.35f;
        const double rxMb = static_cast<double>(fp.bytesReceived) / (1024.0 * 1024.0);
        std::string status;
        if (isRemote) {
            if (fp.totalBytes > 0) {
                const double totMb = static_cast<double>(fp.totalBytes) / (1024.0 * 1024.0);
                const int pct = static_cast<int>(localFrac * 100.0f);
                status = std::format("Downloading {}: {:.1f} / {:.1f} MB ({}%)", displayName, rxMb, totMb, pct);
            } else {
                status = std::format("Downloading {}: {:.1f} MB...", displayName, rxMb);
            }
        } else {
            status = std::format("Reading {}: {:.1f} MB...", displayName, rxMb);
        }
        return progressCb(LoadProgressInfo{.fraction = localFrac * fetchSpan, .status = std::move(status)});
    });
    if (!fetchRes) {
        return std::unexpected("Failed to fetch glTF file: " + fetchRes.error());
    }
    return LoadFromMemory(*fetchRes, path, std::move(progressCb), fetchSpan);
}

} // namespace bgl::scene
