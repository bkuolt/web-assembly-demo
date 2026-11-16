/**
 * @file FastGltfParser.cpp
 * @brief glTF asset stage parsers: textures, materials, lights, meshes/LODs, hierarchy, skins, and animations.
 */
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/ThreadPool.hpp"
#include "scene/FastGltfInternal.hpp"
#include "scene/TextureDecoder.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <utility>

namespace bgl::scene::detail {

namespace {

[[nodiscard]] std::span<const uint8_t> ExtractSafeSlice(const void* basePtr, size_t totalSize, size_t offset,
                                                        size_t length) noexcept {
    if (!basePtr || offset > totalSize || length > (totalSize - offset)) {
        return {};
    }
    return std::span<const uint8_t>{static_cast<const uint8_t*>(basePtr) + offset, length};
}

[[nodiscard]] std::string MimeTypeToString(fastgltf::MimeType mime) {
    switch (mime) {
    case fastgltf::MimeType::JPEG:
        return "image/jpeg";
    case fastgltf::MimeType::PNG:
        return "image/png";
    case fastgltf::MimeType::KTX2:
        return "image/ktx2";
    case fastgltf::MimeType::GltfBuffer:
    case fastgltf::MimeType::OctetStream:
    default:
        return {};
    }
}

[[nodiscard]] std::pair<std::span<const uint8_t>, std::string>
ExtractContiguousSourceBytes(const fastgltf::DataSource& data) {
    if (std::holds_alternative<fastgltf::sources::Array>(data)) {
        const auto& arr = std::get<fastgltf::sources::Array>(data);
        return {{reinterpret_cast<const uint8_t*>(arr.bytes.data()), arr.bytes.size()}, MimeTypeToString(arr.mimeType)};
    }
    if (std::holds_alternative<fastgltf::sources::ByteView>(data)) {
        const auto& bv = std::get<fastgltf::sources::ByteView>(data);
        return {{reinterpret_cast<const uint8_t*>(bv.bytes.data()), bv.bytes.size()}, MimeTypeToString(bv.mimeType)};
    }
    if (std::holds_alternative<fastgltf::sources::Vector>(data)) {
        const auto& vec = std::get<fastgltf::sources::Vector>(data);
        return {{reinterpret_cast<const uint8_t*>(vec.bytes.data()), vec.bytes.size()}, MimeTypeToString(vec.mimeType)};
    }
    return {};
}

void ComputeFallbackNormals(MeshPrimitive& meshPrim) {
    auto safeFaceNormal = [](const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2) noexcept {
        const glm::vec3 cp{glm::cross(p1 - p0, p2 - p0)};
        const float lenSq{glm::dot(cp, cp)};
        return (lenSq > 1e-12f) ? (cp * (1.0f / std::sqrt(lenSq))) : glm::vec3{0.0f, 1.0f, 0.0f};
    };

    if (!meshPrim.indices.empty()) {
        for (size_t i{0}; i + 2 < meshPrim.indices.size(); i += 3) {
            const uint32_t i0{meshPrim.indices[i]};
            const uint32_t i1{meshPrim.indices[i + 1]};
            const uint32_t i2{meshPrim.indices[i + 2]};
            if (i0 < meshPrim.vertices.size() && i1 < meshPrim.vertices.size() && i2 < meshPrim.vertices.size()) {
                const glm::vec3 fn{safeFaceNormal(meshPrim.vertices[i0].position, meshPrim.vertices[i1].position,
                                                  meshPrim.vertices[i2].position)};
                meshPrim.vertices[i0].normal += fn;
                meshPrim.vertices[i1].normal += fn;
                meshPrim.vertices[i2].normal += fn;
            }
        }
        for (auto& v : meshPrim.vertices) {
            const float len{glm::length(v.normal)};
            v.normal = (len > 0.0001f) ? (v.normal / len) : glm::vec3{0.0f, 1.0f, 0.0f};
        }
        return;
    }

    for (size_t i{0}; i + 2 < meshPrim.vertices.size(); i += 3) {
        const glm::vec3 fn{safeFaceNormal(meshPrim.vertices[i].position, meshPrim.vertices[i + 1].position,
                                          meshPrim.vertices[i + 2].position)};
        meshPrim.vertices[i].normal = fn;
        meshPrim.vertices[i + 1].normal = fn;
        meshPrim.vertices[i + 2].normal = fn;
    }
}

[[nodiscard]] MeshPrimitive ParseMeshPrimitive(const fastgltf::Asset& asset, const fastgltf::Primitive& prim,
                                               bool& outNeedsTangents) {
    MeshPrimitive meshPrim;
    if (prim.materialIndex.has_value()) {
        meshPrim.materialIndex = static_cast<int>(*prim.materialIndex);
    }

    if (auto posIt = prim.findAttribute("POSITION"); posIt != prim.attributes.end()) {
        const auto& posAccessor = asset.accessors[posIt->second];
        meshPrim.vertices.resize(posAccessor.count);
        fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, posAccessor, [&](glm::vec3 pos, size_t idx) {
            meshPrim.vertices[idx].position = pos;
            meshPrim.boundingBox.Expand(pos);
        });
    }

    auto normIt = prim.findAttribute("NORMAL");
    if (normIt != prim.attributes.end()) {
        const auto& normAccessor = asset.accessors[normIt->second];
        fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, normAccessor, [&](glm::vec3 norm, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                meshPrim.vertices[idx].normal = norm;
            }
        });
    }

    if (auto uvIt = prim.findAttribute("TEXCOORD_0"); uvIt != prim.attributes.end()) {
        const auto& uvAccessor = asset.accessors[uvIt->second];
        fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, uvAccessor, [&](glm::vec2 uv, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                meshPrim.vertices[idx].uv = uv;
            }
        });
    }

    auto tanIt = prim.findAttribute("TANGENT");
    const bool hasExplicitTangents = (tanIt != prim.attributes.end());
    if (hasExplicitTangents) {
        const auto& tanAccessor = asset.accessors[tanIt->second];
        fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, tanAccessor, [&](glm::vec4 tan, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                meshPrim.vertices[idx].tangent = tan;
            }
        });
    }

    if (auto jointsIt = prim.findAttribute("JOINTS_0"); jointsIt != prim.attributes.end()) {
        const auto& jAccessor = asset.accessors[jointsIt->second];
        fastgltf::iterateAccessorWithIndex<glm::uvec4>(asset, jAccessor, [&](glm::uvec4 j, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                meshPrim.vertices[idx].joints = j;
            }
        });
    }

    if (auto weightsIt = prim.findAttribute("WEIGHTS_0"); weightsIt != prim.attributes.end()) {
        const auto& wAccessor = asset.accessors[weightsIt->second];
        fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, wAccessor, [&](glm::vec4 w, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                const float sum = w.x + w.y + w.z + w.w;
                if (sum > 1e-6f) {
                    w /= sum;
                }
                meshPrim.vertices[idx].weights = w;
            }
        });
    }

    if (!prim.targets.empty()) {
        meshPrim.morphTargets.resize(prim.targets.size());
        for (size_t tIdx = 0; tIdx < prim.targets.size(); ++tIdx) {
            auto& mt = meshPrim.morphTargets[tIdx];
            mt.positionDeltas.resize(meshPrim.vertices.size(), glm::vec3{0.0f});
            mt.normalDeltas.resize(meshPrim.vertices.size(), glm::vec3{0.0f});
            mt.tangentDeltas.resize(meshPrim.vertices.size(), glm::vec3{0.0f});

            auto loadDelta = [&](const char* attr, std::vector<glm::vec3>& dst) {
                if (auto it = prim.findTargetAttribute(tIdx, attr); it != prim.targets[tIdx].cend()) {
                    const auto& acc = asset.accessors[it->second];
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, acc, [&](glm::vec3 d, size_t idx) {
                        if (idx < dst.size()) {
                            dst[idx] = d;
                        }
                    });
                }
            };
            loadDelta("POSITION", mt.positionDeltas);
            loadDelta("NORMAL", mt.normalDeltas);
            loadDelta("TANGENT", mt.tangentDeltas);
        }
        meshPrim.baseVertices = meshPrim.vertices;
    }

    if (prim.indicesAccessor.has_value()) {
        const auto& idxAccessor = asset.accessors[*prim.indicesAccessor];
        meshPrim.indices.resize(idxAccessor.count);
        const size_t vertexCount{meshPrim.vertices.size()};
        bool hasInvalidIndices{false};
        fastgltf::iterateAccessorWithIndex<uint32_t>(asset, idxAccessor, [&](uint32_t val, size_t idx) {
            if (vertexCount > 0 && val >= vertexCount) {
                hasInvalidIndices = true;
                meshPrim.indices[idx] = 0;
            } else {
                meshPrim.indices[idx] = val;
            }
        });
        if (hasInvalidIndices) {
            BGL_LOG_WARN("[glTF Loader] Mesh primitive contains out-of-bounds index references; clamped to 0 "
                         "for GPU buffer safety");
        }
    }

    if (normIt == prim.attributes.end() && !meshPrim.vertices.empty()) {
        ComputeFallbackNormals(meshPrim);
    }

    outNeedsTangents = !hasExplicitTangents;
    if (hasExplicitTangents && !meshPrim.vertices.empty()) {
        const auto& firstTan{meshPrim.vertices.front().tangent};
        const glm::vec3 tVec{firstTan.x, firstTan.y, firstTan.z};
        if (glm::dot(tVec, tVec) < 0.25f || std::abs(firstTan.w) < 0.1f) {
            outNeedsTangents = true;
        }
    }

    return meshPrim;
}

} // namespace

void DecodeImagesAndTextures(const fastgltf::Asset& asset, std::string_view baseUri, AssetManager& assetManager,
                             Scene& scene, ProgressReporter* reporter) {
    const size_t numImages = asset.images.size();
    const size_t numTextures = asset.textures.size();
    BGL_LOG_INFO("[glTF Loader] --- Images & Textures ({} images, {} textures) ---", numImages, numTextures);
    scene.gpuTextures.resize(numTextures);
    scene.cpuImages.resize(numTextures);

    BGL_LOG_INFO("[glTF Loader] Launching parallel image decoding via centralized ThreadPool...");

    std::vector<std::optional<CpuImage>> decodedImages(numImages);
    std::atomic<size_t> completedImages{0};
    core::ThreadPool::Instance().ParallelFor(0, numImages, [&](size_t imgIdx) {
        if (reporter && reporter->IsCancelled()) {
            return;
        }
        const auto& image = asset.images[imgIdx];
        std::span<const uint8_t> rawBytes{};
        std::vector<uint8_t> fetchedUriBytes{};
        std::string decodeHint{};
        size_t sourceBvIdx{0};

        if (std::holds_alternative<fastgltf::sources::BufferView>(image.data)) {
            const auto& bvSource = std::get<fastgltf::sources::BufferView>(image.data);
            decodeHint = MimeTypeToString(bvSource.mimeType);
            sourceBvIdx = bvSource.bufferViewIndex;
            if (bvSource.bufferViewIndex < asset.bufferViews.size()) {
                const auto& bufferView = asset.bufferViews[bvSource.bufferViewIndex];
                if (bufferView.bufferIndex < asset.buffers.size()) {
                    const auto [bufBytes, unusedHint] =
                        ExtractContiguousSourceBytes(asset.buffers[bufferView.bufferIndex].data);
                    (void)unusedHint;
                    rawBytes = ExtractSafeSlice(bufBytes.data(), bufBytes.size(), bufferView.byteOffset,
                                                bufferView.byteLength);
                }
            }
        } else if (auto [srcBytes, srcHint] = ExtractContiguousSourceBytes(image.data); !srcBytes.empty()) {
            rawBytes = srcBytes;
            decodeHint = std::move(srcHint);
        } else if (std::holds_alternative<fastgltf::sources::URI>(image.data)) {
            const auto& uriSource = std::get<fastgltf::sources::URI>(image.data);
            const std::string rawUriStr{uriSource.uri.string()};
            decodeHint = MimeTypeToString(uriSource.mimeType);
            if (decodeHint.empty()) {
                decodeHint = rawUriStr;
            }
            const std::string resolvedUri{core::NetworkFetcher::ResolveRelativeUri(baseUri, rawUriStr)};
            BGL_LOG_INFO("  * Image #{}: Fetching external URI '{}'...", imgIdx, resolvedUri);
            auto fetchRes =
                core::NetworkFetcher::FetchSync(resolvedUri, [reporter](const core::FetchProgress&) -> bool {
                    return !reporter || !reporter->IsCancelled();
                });
            if (fetchRes) {
                fetchedUriBytes = std::move(*fetchRes);
                const size_t offset{std::min(uriSource.fileByteOffset, fetchedUriBytes.size())};
                rawBytes = std::span<const uint8_t>{fetchedUriBytes.data() + offset, fetchedUriBytes.size() - offset};
            } else {
                BGL_LOG_ERROR("  * Image #{}: Failed to fetch external image URI '{}': {}", imgIdx, resolvedUri,
                              fetchRes.error());
            }
        }

        if (decodeHint.empty() && !image.name.empty()) {
            decodeHint = std::string{image.name};
        }

        const std::string imgName = image.name.empty()
                                        ? (decodeHint.empty() ? ("Image_" + std::to_string(imgIdx))
                                                              : core::NetworkFetcher::ExtractDisplayName(decodeHint))
                                        : std::string(image.name);

        if (rawBytes.empty()) {
            BGL_LOG_WARN("  * Image #{}: Raw byte stream is empty", imgIdx);
            return;
        }

        const uint64_t contentHash = Fnv1a64(rawBytes);
        if (auto cached = assetManager.GetCachedTexture(contentHash)) {
            decodedImages[imgIdx] = CpuImage{.width = cached->width,
                                             .height = cached->height,
                                             .rgba = cached->rgbaData,
                                             .isCompressed = cached->isCompressed,
                                             .compressionFormat = cached->compressionFormat,
                                             .mipLevels = cached->mipLevels};
            BGL_LOG_DEBUG("  * Image #{} ('{}') hit asset cache (content hash {:x})", imgIdx, imgName, contentHash);
            return;
        }

        auto decoded = TextureDecoder::DecodeAuto(rawBytes, decodeHint);
        if (!decoded) {
            BGL_LOG_ERROR("  * Image #{}: Failed to decode: {}", imgIdx, decoded.error());
            return;
        }

        auto sharedImg = std::make_shared<DecodedImage>(*decoded);
        assetManager.InsertTexture(contentHash, sharedImg);
        decodedImages[imgIdx] = CpuImage{.width = decoded->width,
                                         .height = decoded->height,
                                         .rgba = std::move(decoded->rgbaData),
                                         .isCompressed = decoded->isCompressed,
                                         .compressionFormat = decoded->compressionFormat,
                                         .mipLevels = std::move(decoded->mipLevels)};
        if (TextureDecoder::IsKtx2Data(rawBytes)) {
            decodedImages[imgIdx]->ktx2SourceBytes.assign(rawBytes.begin(), rawBytes.end());
        }
        BGL_LOG_DEBUG("  * Image #{} (BufferView #{}): '{}' -> Decoded {} {}x{} (Mips: {}, Payload: {:.2f} MB)", imgIdx,
                      sourceBvIdx, imgName, TextureDecoder::GetKtxTargetName(decodedImages[imgIdx]->compressionFormat),
                      decoded->width, decoded->height, std::max<size_t>(1, decodedImages[imgIdx]->mipLevels.size()),
                      static_cast<double>(decodedImages[imgIdx]->rgba.size()) / (1024.0 * 1024.0));
        if (reporter && numImages > 0) {
            const size_t done = completedImages.fetch_add(1, std::memory_order_relaxed) + 1;
            const float localFrac = 0.12f + (static_cast<float>(done) / static_cast<float>(numImages)) * 0.43f;
            (void)reporter->ReportLocal(localFrac, std::format("Decoding textures ({} / {})...", done, numImages));
        }
    });

    for (size_t texIdx{0}; texIdx < numTextures; ++texIdx) {
        const auto& texture = asset.textures[texIdx];
        fastgltf::Optional<size_t> resolvedImgIdx{texture.imageIndex};
        if (texture.basisuImageIndex.has_value()) {
            resolvedImgIdx = texture.basisuImageIndex;
        } else if (texture.webpImageIndex.has_value()) {
            resolvedImgIdx = texture.webpImageIndex;
        }
        if (resolvedImgIdx.has_value() && *resolvedImgIdx < decodedImages.size()) {
            const size_t imgIdx{*resolvedImgIdx};
            if (const auto& decodedOpt = decodedImages[imgIdx]; decodedOpt.has_value()) {
                scene.cpuImages[texIdx] = *decodedOpt;
            }
        }
    }
}

void ParseMeshesAndGenerateLods(const fastgltf::Asset& asset, Scene& scene, ProgressReporter* reporter) {
    std::vector<std::shared_ptr<Mesh>> parsedMeshes(asset.meshes.size());
    std::vector<std::vector<uint8_t>> meshPrimNeedsTangents(asset.meshes.size());

    core::ThreadPool::Instance().ParallelFor(0, asset.meshes.size(), [&](size_t mIdx) {
        if (reporter && reporter->IsCancelled()) {
            return;
        }
        const auto& mesh = asset.meshes[mIdx];
        auto sharedMesh = std::make_shared<Mesh>();
        sharedMesh->name = mesh.name.empty() ? ("Mesh_" + std::to_string(mIdx)) : std::string(mesh.name);
        sharedMesh->primitives.reserve(mesh.primitives.size());
        meshPrimNeedsTangents[mIdx].reserve(mesh.primitives.size());

        for (const auto& prim : mesh.primitives) {
            if (prim.type != fastgltf::PrimitiveType::Triangles) {
                continue;
            }
            bool needsTangents{false};
            MeshPrimitive meshPrim = ParseMeshPrimitive(asset, prim, needsTangents);
            meshPrimNeedsTangents[mIdx].push_back(needsTangents ? 1u : 0u);
            sharedMesh->primitives.push_back(std::move(meshPrim));
        }
        parsedMeshes[mIdx] = std::move(sharedMesh);
    });

    struct PrimitivePostTask {
        MeshPrimitive* prim{nullptr};
        bool computeTangents{false};
    };
    std::vector<PrimitivePostTask> postTasks{};
    for (size_t mIdx{0}; mIdx < parsedMeshes.size(); ++mIdx) {
        const auto& sharedMesh = parsedMeshes[mIdx];
        if (!sharedMesh) {
            continue;
        }
        for (size_t pIdx{0}; pIdx < sharedMesh->primitives.size(); ++pIdx) {
            const bool needTan = pIdx < meshPrimNeedsTangents[mIdx].size() && meshPrimNeedsTangents[mIdx][pIdx] != 0;
            postTasks.push_back(PrimitivePostTask{&sharedMesh->primitives[pIdx], needTan});
        }
    }
    if (!postTasks.empty()) {
        std::atomic<size_t> completedPrims{0};
        const size_t totalPrims = postTasks.size();
        core::ThreadPool::Instance().ParallelFor(
            0, totalPrims,
            [&](size_t tIdx) {
                if (reporter && reporter->IsCancelled()) {
                    return;
                }
                const PrimitivePostTask task = postTasks[tIdx];
                if (task.computeTangents) {
                    task.prim->ComputeTangents();
                }
                task.prim->GenerateLODs();
                if (reporter) {
                    const size_t done = completedPrims.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done == totalPrims || (done % 4u) == 0u || totalPrims <= 16u) {
                        const float localFrac =
                            0.58f + (static_cast<float>(done) / static_cast<float>(totalPrims)) * 0.34f;
                        (void)reporter->ReportLocal(
                            localFrac, std::format("Building meshes & LODs ({} / {})...", done, totalPrims));
                    }
                }
            },
            1);
    }
    scene.meshes = std::move(parsedMeshes);
}

} // namespace bgl::scene::detail
