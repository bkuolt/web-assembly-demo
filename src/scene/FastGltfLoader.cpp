/**
 * @file FastGltfLoader.cpp
 * @brief Implementation of glTF 2.1 loader with fastgltf accessor parsing, embedded image decoding, and detailed logging.
 */
#include "scene/FastGltfLoader.hpp"
#include "scene/TextureDecoder.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/ThreadPool.hpp"
#include "core/Log.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>

namespace bgl::scene {

static const char* AccessorTypeToString(fastgltf::AccessorType type) {
    switch (type) {
        case fastgltf::AccessorType::Scalar: return "Scalar";
        case fastgltf::AccessorType::Vec2: return "Vec2";
        case fastgltf::AccessorType::Vec3: return "Vec3";
        case fastgltf::AccessorType::Vec4: return "Vec4";
        case fastgltf::AccessorType::Mat2: return "Mat2";
        case fastgltf::AccessorType::Mat3: return "Mat3";
        case fastgltf::AccessorType::Mat4: return "Mat4";
        default: return "Unknown";
    }
}

static const char* ComponentTypeToString(fastgltf::ComponentType comp) {
    switch (comp) {
        case fastgltf::ComponentType::Byte: return "Byte (i8)";
        case fastgltf::ComponentType::UnsignedByte: return "UByte (u8)";
        case fastgltf::ComponentType::Short: return "Short (i16)";
        case fastgltf::ComponentType::UnsignedShort: return "UShort (u16)";
        case fastgltf::ComponentType::Int: return "Int (i32)";
        case fastgltf::ComponentType::UnsignedInt: return "UInt (u32)";
        case fastgltf::ComponentType::Float: return "Float (f32)";
        case fastgltf::ComponentType::Double: return "Double (f64)";
        default: return "Unknown";
    }
}

FastGltfLoader::FastGltfLoader(AssetManager& assetManager)
    : assetManager_(assetManager),
      parser_(fastgltf::Extensions::EXT_texture_webp |
              fastgltf::Extensions::KHR_texture_basisu |
              fastgltf::Extensions::KHR_texture_transform |
              fastgltf::Extensions::KHR_lights_punctual |
              fastgltf::Extensions::KHR_materials_clearcoat |
              fastgltf::Extensions::KHR_materials_transmission |
              fastgltf::Extensions::KHR_materials_sheen |
              fastgltf::Extensions::KHR_materials_specular |
              fastgltf::Extensions::KHR_materials_ior |
              fastgltf::Extensions::KHR_materials_iridescence |
              fastgltf::Extensions::KHR_materials_volume |
              fastgltf::Extensions::KHR_materials_emissive_strength |
              fastgltf::Extensions::KHR_materials_unlit) {}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromMemory(std::span<const uint8_t> data, std::string_view baseUri) {
    fastgltf::GltfDataBuffer dataBuffer{};
    if (!dataBuffer.copyBytes(data.data(), data.size())) {
        return std::unexpected("Failed to copy bytes into fastgltf::GltfDataBuffer");
    }
    return ParseAndAssembleScene(dataBuffer, baseUri, data.size());
}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::ParseAndAssembleScene(
    fastgltf::GltfDataBuffer& dataBuffer,
    std::string_view baseUri,
    size_t totalBytes) {
    BGL_LOG_INFO("================================================================================");
    BGL_LOG_INFO("[glTF Loader] Starting Ingestion of Asset: '{}' (Total size: {} bytes / {:.2f} MB)",
                 baseUri.empty() ? "In-Memory Stream" : baseUri, totalBytes, totalBytes / (1024.0 * 1024.0));
    BGL_LOG_INFO("================================================================================");

    std::filesystem::path baseDir{std::filesystem::current_path()};
    fastgltf::Options parseOptions{
        fastgltf::Options::LoadGLBBuffers |
        fastgltf::Options::DecomposeNodeMatrices
    };
    if (!baseUri.empty() && !core::NetworkFetcher::IsUrl(baseUri)) {
        const std::filesystem::path localParent{std::filesystem::path{baseUri}.parent_path()};
        if (!localParent.empty()) {
            baseDir = localParent;
        }
        if (std::filesystem::exists(baseDir)) {
            parseOptions |= fastgltf::Options::LoadExternalBuffers;
        }
    }

    auto gltf = parser_.loadGltf(&dataBuffer, baseDir, parseOptions);
    if (gltf.error() != fastgltf::Error::None) {
        return std::unexpected(std::string{"fastgltf failed to parse glTF/GLB data: "} + std::string{fastgltf::getErrorName(gltf.error())});
    }

    auto& asset = gltf.get();
    auto scene = std::make_shared<Scene>();

    // 1. Resolve external URI buffers (local files or remote HTTP/HTTPS URLs via libcurl) & log Buffers
    std::vector<std::vector<uint8_t>> externalBufferStorage(asset.buffers.size());
    BGL_LOG_INFO("[glTF Loader] --- Buffers ({} total) ---", asset.buffers.size());
    size_t totalBufferBytes{0};
    for (size_t bIdx{0}; bIdx < asset.buffers.size(); ++bIdx) {
        auto& buf = asset.buffers[bIdx];
        totalBufferBytes += buf.byteLength;
        const char* typeStr{"Unknown"};
        if (std::holds_alternative<fastgltf::sources::Array>(buf.data)) typeStr = "Array";
        else if (std::holds_alternative<fastgltf::sources::ByteView>(buf.data)) typeStr = "ByteView";
        else if (std::holds_alternative<fastgltf::sources::Vector>(buf.data)) typeStr = "Vector";
        else if (std::holds_alternative<fastgltf::sources::URI>(buf.data)) {
            typeStr = "External URI";
            const auto& uriSource = std::get<fastgltf::sources::URI>(buf.data);
            const std::string rawUriStr{uriSource.uri.string()};
            const std::string resolvedUri{core::NetworkFetcher::ResolveRelativeUri(baseUri, rawUriStr)};
            BGL_LOG_INFO("  * Buffer #{}: Resolving external URI '{}' -> '{}'", bIdx, rawUriStr, resolvedUri);

            auto fetchRes = core::NetworkFetcher::FetchSync(resolvedUri);
            if (!fetchRes) {
                return std::unexpected("Failed to fetch external glTF buffer '" + resolvedUri + "': " + fetchRes.error());
            }
            externalBufferStorage[bIdx] = std::move(*fetchRes);
            const size_t offset{std::min(uriSource.fileByteOffset, externalBufferStorage[bIdx].size())};
            const size_t avail{externalBufferStorage[bIdx].size() - offset};
            buf.data = fastgltf::sources::ByteView{
                .bytes = fastgltf::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(externalBufferStorage[bIdx].data() + offset),
                    avail
                },
                .mimeType = uriSource.mimeType
            };
        }

        BGL_LOG_DEBUG("  * Buffer #{}: {} bytes ({:.2f} MB), Source: {}", bIdx, buf.byteLength, buf.byteLength / (1024.0 * 1024.0), typeStr);
    }

    BGL_LOG_INFO("[glTF Loader] --- BufferViews ({} total) ---", asset.bufferViews.size());
    for (size_t bvIdx = 0; bvIdx < asset.bufferViews.size(); ++bvIdx) {
        const auto& bv = asset.bufferViews[bvIdx];
        const char* targetStr = "None";
        if (bv.target == fastgltf::BufferTarget::ArrayBuffer) targetStr = "ArrayBuffer (Vertices)";
        else if (bv.target == fastgltf::BufferTarget::ElementArrayBuffer) targetStr = "ElementArrayBuffer (Indices)";

        BGL_LOG_DEBUG("  * BufferView #{}: Buffer #{}, Offset={}, Length={}, Stride={}, Target={}",
                      bvIdx, bv.bufferIndex, bv.byteOffset, bv.byteLength, bv.byteStride.value_or(0), targetStr);
    }

    // 2. Decode Embedded & External Images into CPU buffers in parallel (deduplicated per unique image)
    const size_t numImages = asset.images.size();
    const size_t numTextures = asset.textures.size();
    BGL_LOG_INFO("[glTF Loader] --- Images & Textures ({} images, {} textures) ---", numImages, numTextures);
    scene->gpuTextures.resize(numTextures);
    scene->cpuImages.resize(numTextures);

    BGL_LOG_INFO("[glTF Loader] Launching parallel image decoding via centralized ThreadPool...");

    std::vector<std::optional<CpuImage>> decodedImages(numImages);
    core::ThreadPool::Instance().ParallelFor(0, numImages, [&](size_t imgIdx) {
        const auto& image = asset.images[imgIdx];
        std::span<const uint8_t> rawBytes{};
        std::vector<uint8_t> fetchedUriBytes{};
        std::string decodeHint{};
        size_t sourceBvIdx{0};

        auto extractSafeSlice = [](const void* basePtr, size_t totalSize, size_t offset, size_t length) -> std::span<const uint8_t> {
            if (!basePtr || offset > totalSize || length > (totalSize - offset)) {
                return {};
            }
            return std::span<const uint8_t>{static_cast<const uint8_t*>(basePtr) + offset, length};
        };

        auto mimeTypeToString = [](fastgltf::MimeType mime) -> std::string {
            switch (mime) {
                case fastgltf::MimeType::JPEG: return "image/jpeg";
                case fastgltf::MimeType::PNG:  return "image/png";
                case fastgltf::MimeType::KTX2: return "image/ktx2";
                case fastgltf::MimeType::GltfBuffer:
                case fastgltf::MimeType::OctetStream:
                default: return {};
            }
        };

        if (std::holds_alternative<fastgltf::sources::BufferView>(image.data)) {
            const auto& bvSource = std::get<fastgltf::sources::BufferView>(image.data);
            decodeHint = mimeTypeToString(bvSource.mimeType);
            sourceBvIdx = bvSource.bufferViewIndex;
            if (bvSource.bufferViewIndex < asset.bufferViews.size()) {
                const auto& bufferView = asset.bufferViews[bvSource.bufferViewIndex];
                if (bufferView.bufferIndex < asset.buffers.size()) {
                    const auto& buffer = asset.buffers[bufferView.bufferIndex];
                    if (std::holds_alternative<fastgltf::sources::Array>(buffer.data)) {
                        const auto& array = std::get<fastgltf::sources::Array>(buffer.data);
                        rawBytes = extractSafeSlice(array.bytes.data(), array.bytes.size(), bufferView.byteOffset, bufferView.byteLength);
                    } else if (std::holds_alternative<fastgltf::sources::ByteView>(buffer.data)) {
                        const auto& bv = std::get<fastgltf::sources::ByteView>(buffer.data);
                        rawBytes = extractSafeSlice(bv.bytes.data(), bv.bytes.size(), bufferView.byteOffset, bufferView.byteLength);
                    } else if (std::holds_alternative<fastgltf::sources::Vector>(buffer.data)) {
                        const auto& vec = std::get<fastgltf::sources::Vector>(buffer.data);
                        rawBytes = extractSafeSlice(vec.bytes.data(), vec.bytes.size(), bufferView.byteOffset, bufferView.byteLength);
                    }
                }
            }
        } else if (std::holds_alternative<fastgltf::sources::ByteView>(image.data)) {
            const auto& bv = std::get<fastgltf::sources::ByteView>(image.data);
            decodeHint = mimeTypeToString(bv.mimeType);
            rawBytes = std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(bv.bytes.data()), bv.bytes.size()};
        } else if (std::holds_alternative<fastgltf::sources::Vector>(image.data)) {
            const auto& vec = std::get<fastgltf::sources::Vector>(image.data);
            decodeHint = mimeTypeToString(vec.mimeType);
            rawBytes = std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(vec.bytes.data()), vec.bytes.size()};
        } else if (std::holds_alternative<fastgltf::sources::Array>(image.data)) {
            const auto& arr = std::get<fastgltf::sources::Array>(image.data);
            decodeHint = mimeTypeToString(arr.mimeType);
            rawBytes = std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(arr.bytes.data()), arr.bytes.size()};
        } else if (std::holds_alternative<fastgltf::sources::URI>(image.data)) {
            const auto& uriSource = std::get<fastgltf::sources::URI>(image.data);
            const std::string rawUriStr{uriSource.uri.string()};
            decodeHint = mimeTypeToString(uriSource.mimeType);
            if (decodeHint.empty()) {
                decodeHint = rawUriStr;
            }
            const std::string resolvedUri{core::NetworkFetcher::ResolveRelativeUri(baseUri, rawUriStr)};
            BGL_LOG_INFO("  * Image #{}: Fetching external URI '{}'...", imgIdx, resolvedUri);
            auto fetchRes = core::NetworkFetcher::FetchSync(resolvedUri);
            if (fetchRes) {
                fetchedUriBytes = std::move(*fetchRes);
                const size_t offset{std::min(uriSource.fileByteOffset, fetchedUriBytes.size())};
                rawBytes = std::span<const uint8_t>{fetchedUriBytes.data() + offset, fetchedUriBytes.size() - offset};
            } else {
                BGL_LOG_ERROR("  * Image #{}: Failed to fetch external image URI '{}': {}",
                              imgIdx, resolvedUri, fetchRes.error());
            }
        }

        if (decodeHint.empty() && !image.name.empty()) {
            decodeHint = std::string{image.name};
        }

        std::string imgName = image.name.empty()
            ? (decodeHint.empty() ? ("Image_" + std::to_string(imgIdx)) : core::NetworkFetcher::ExtractDisplayName(decodeHint))
            : std::string(image.name);

        if (!rawBytes.empty()) {
            const uint64_t contentHash = Fnv1a64(rawBytes);
            if (auto cached = assetManager_.GetCachedTexture(contentHash)) {
                decodedImages[imgIdx] = CpuImage{
                    .width = cached->width,
                    .height = cached->height,
                    .rgba = cached->rgbaData,
                    .isCompressed = cached->isCompressed,
                    .compressionFormat = cached->compressionFormat,
                    .mipLevels = cached->mipLevels
                };
                BGL_LOG_DEBUG("  * Image #{} ('{}') hit asset cache (content hash {:x})", imgIdx, imgName, contentHash);
            } else {
                auto decoded = TextureDecoder::DecodeAuto(rawBytes, decodeHint);
                if (decoded) {
                    auto sharedImg = std::make_shared<DecodedImage>(*decoded);
                    assetManager_.InsertTexture(contentHash, sharedImg);
                    decodedImages[imgIdx] = CpuImage{
                        .width = decoded->width,
                        .height = decoded->height,
                        .rgba = std::move(decoded->rgbaData),
                        .isCompressed = decoded->isCompressed,
                        .compressionFormat = decoded->compressionFormat,
                        .mipLevels = std::move(decoded->mipLevels)
                    };
                    BGL_LOG_DEBUG("  * Image #{} (BufferView #{}): '{}' -> Decoded {} {}x{} (Mips: {}, Payload: {:.2f} MB)",
                                 imgIdx, sourceBvIdx, imgName,
                                 TextureDecoder::GetKtxTargetName(decodedImages[imgIdx]->compressionFormat),
                                 decoded->width, decoded->height,
                                 std::max<size_t>(1, decodedImages[imgIdx]->mipLevels.size()),
                                 decodedImages[imgIdx]->rgba.size() / (1024.0 * 1024.0));
                } else {
                    BGL_LOG_ERROR("  * Image #{}: Failed to decode: {}", imgIdx, decoded.error());
                }
            }
        } else {
            BGL_LOG_WARN("  * Image #{}: Raw byte stream is empty", imgIdx);
        }
    });

    // Assign decoded images to glTF textures
    for (size_t texIdx{0}; texIdx < numTextures; ++texIdx) {
        const auto& texture = asset.textures[texIdx];
        fastgltf::Optional<size_t> resolvedImgIdx{texture.imageIndex};
        if (texture.basisuImageIndex.has_value()) {
            resolvedImgIdx = texture.basisuImageIndex;
        } else if (texture.webpImageIndex.has_value()) {
            resolvedImgIdx = texture.webpImageIndex;
        }
        if (resolvedImgIdx.has_value() && *resolvedImgIdx < decodedImages.size() && decodedImages[*resolvedImgIdx].has_value()) {
            scene->cpuImages[texIdx] = *decodedImages[*resolvedImgIdx];
        }
    }

    // 3. Decode Materials
    BGL_LOG_INFO("[glTF Loader] --- Materials ({} total) ---", asset.materials.size());
    scene->materials.reserve(asset.materials.size());
    for (size_t mIdx = 0; mIdx < asset.materials.size(); ++mIdx) {
        const auto& mat = asset.materials[mIdx];
        Material sceneMat;
        sceneMat.name = mat.name.empty() ? ("Material_" + std::to_string(mIdx)) : std::string(mat.name);
        sceneMat.baseColorFactor = glm::vec4(
            mat.pbrData.baseColorFactor[0],
            mat.pbrData.baseColorFactor[1],
            mat.pbrData.baseColorFactor[2],
            mat.pbrData.baseColorFactor[3]
        );
        sceneMat.metallicFactor = mat.pbrData.metallicFactor;
        sceneMat.roughnessFactor = mat.pbrData.roughnessFactor;

        if (mat.pbrData.baseColorTexture.has_value()) {
            sceneMat.baseColorTextureIndex = static_cast<int>(mat.pbrData.baseColorTexture->textureIndex);
        }

        // Metallic-Roughness Texture
        if (mat.pbrData.metallicRoughnessTexture.has_value()) {
            sceneMat.metallicRoughnessTextureIndex = static_cast<int>(mat.pbrData.metallicRoughnessTexture->textureIndex);
        }

        // Normal Texture
        if (mat.normalTexture.has_value()) {
            sceneMat.normalTextureIndex = static_cast<int>(mat.normalTexture->textureIndex);
            sceneMat.normalScale = mat.normalTexture->scale;
        }

        // Occlusion Texture
        if (mat.occlusionTexture.has_value()) {
            sceneMat.occlusionTextureIndex = static_cast<int>(mat.occlusionTexture->textureIndex);
            sceneMat.occlusionStrength = mat.occlusionTexture->strength;
        }

        // Emissive
        sceneMat.emissiveFactor = glm::vec3(mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]);
        if (mat.emissiveTexture.has_value()) {
            sceneMat.emissiveTextureIndex = static_cast<int>(mat.emissiveTexture->textureIndex);
        }
        sceneMat.emissiveStrength = mat.emissiveStrength;

        // Alpha Mode & Culling
        if (mat.alphaMode == fastgltf::AlphaMode::Mask) {
            sceneMat.alphaMode = AlphaMode::Mask;
        } else if (mat.alphaMode == fastgltf::AlphaMode::Blend) {
            sceneMat.alphaMode = AlphaMode::Blend;
        } else {
            sceneMat.alphaMode = AlphaMode::Opaque;
        }
        sceneMat.alphaCutoff = mat.alphaCutoff;
        sceneMat.doubleSided = mat.doubleSided;
        sceneMat.unlit = mat.unlit;

        // KHR_materials_clearcoat
        if (mat.clearcoat) {
            sceneMat.clearcoatFactor = mat.clearcoat->clearcoatFactor;
            sceneMat.clearcoatRoughnessFactor = mat.clearcoat->clearcoatRoughnessFactor;
            if (mat.clearcoat->clearcoatTexture.has_value()) {
                sceneMat.clearcoatTextureIndex = static_cast<int>(mat.clearcoat->clearcoatTexture->textureIndex);
            }
        }

        // KHR_materials_transmission & IOR
        if (mat.transmission) {
            sceneMat.transmissionFactor = mat.transmission->transmissionFactor;
            if (mat.transmission->transmissionTexture.has_value()) {
                sceneMat.transmissionTextureIndex = static_cast<int>(mat.transmission->transmissionTexture->textureIndex);
            }
        }
        sceneMat.ior = mat.ior;

        // KHR_materials_sheen
        if (mat.sheen) {
            sceneMat.sheenColorFactor = glm::vec3(mat.sheen->sheenColorFactor[0], mat.sheen->sheenColorFactor[1], mat.sheen->sheenColorFactor[2]);
            sceneMat.sheenRoughnessFactor = mat.sheen->sheenRoughnessFactor;
            if (mat.sheen->sheenColorTexture.has_value()) {
                sceneMat.sheenColorTextureIndex = static_cast<int>(mat.sheen->sheenColorTexture->textureIndex);
            }
        }

        // KHR_materials_specular
        if (mat.specular) {
            sceneMat.specularFactor = mat.specular->specularFactor;
            sceneMat.specularColorFactor = glm::vec3(mat.specular->specularColorFactor[0], mat.specular->specularColorFactor[1], mat.specular->specularColorFactor[2]);
            if (mat.specular->specularTexture.has_value()) {
                sceneMat.specularTextureIndex = static_cast<int>(mat.specular->specularTexture->textureIndex);
            }
        }

        // KHR_materials_volume
        if (mat.volume) {
            sceneMat.thicknessFactor = mat.volume->thicknessFactor;
            sceneMat.attenuationDistance = std::isfinite(mat.volume->attenuationDistance)
                ? mat.volume->attenuationDistance
                : 1e9f;
            sceneMat.attenuationColor = glm::vec3(
                mat.volume->attenuationColor[0],
                mat.volume->attenuationColor[1],
                mat.volume->attenuationColor[2]
            );
            if (mat.volume->thicknessTexture.has_value()) {
                sceneMat.thicknessTextureIndex = static_cast<int>(mat.volume->thicknessTexture->textureIndex);
            }
        }

        std::string extDesc = "";
        if (sceneMat.clearcoatFactor > 0.0f) extDesc += " Clearcoat(" + std::to_string(sceneMat.clearcoatFactor) + ")";
        if (sceneMat.transmissionFactor > 0.0f) extDesc += " Transmission(" + std::to_string(sceneMat.transmissionFactor) + ")";
        if (sceneMat.thicknessFactor > 0.0f) extDesc += " Volume(thick=" + std::to_string(sceneMat.thicknessFactor) + ")";
        if (glm::length(sceneMat.sheenColorFactor) > 0.0f) extDesc += " Sheen";
        if (sceneMat.unlit) extDesc += " Unlit";
        if (sceneMat.doubleSided) extDesc += " DoubleSided";

        BGL_LOG_DEBUG("  * Material #{}: '{}' | baseColorFactor=[{:.2f}, {:.2f}, {:.2f}, {:.2f}], metal={:.2f}, rough={:.2f}, baseColorTex={}, normalTex={}, emissiveTex={}{}",
                     mIdx, sceneMat.name,
                     sceneMat.baseColorFactor.r, sceneMat.baseColorFactor.g, sceneMat.baseColorFactor.b, sceneMat.baseColorFactor.a,
                     sceneMat.metallicFactor, sceneMat.roughnessFactor,
                     sceneMat.baseColorTextureIndex >= 0 ? std::to_string(sceneMat.baseColorTextureIndex) : "None",
                     sceneMat.normalTextureIndex >= 0 ? std::to_string(sceneMat.normalTextureIndex) : "None",
                     sceneMat.emissiveTextureIndex >= 0 ? std::to_string(sceneMat.emissiveTextureIndex) : "None",
                     extDesc);

        scene->materials.push_back(sceneMat);
    }

    // Accessors & Meshes Structural Inspection
    BGL_LOG_INFO("[glTF Loader] --- Accessors ({} total) ---", asset.accessors.size());
    for (size_t aIdx = 0; aIdx < asset.accessors.size(); ++aIdx) {
        const auto& acc = asset.accessors[aIdx];
        BGL_LOG_DEBUG("  * Accessor #{}: BufferView #{}, Count={}, Type={}, CompType={}",
                      aIdx, acc.bufferViewIndex.value_or(0), acc.count,
                      AccessorTypeToString(acc.type), ComponentTypeToString(acc.componentType));
    }

    BGL_LOG_INFO("[glTF Loader] --- Meshes & Primitives ({} meshes) ---", asset.meshes.size());
    for (size_t mIdx = 0; mIdx < asset.meshes.size(); ++mIdx) {
        const auto& mesh = asset.meshes[mIdx];
        std::string meshName = mesh.name.empty() ? ("Mesh_" + std::to_string(mIdx)) : std::string(mesh.name);
        BGL_LOG_DEBUG("  * Mesh #{}: '{}' ({} primitives)", mIdx, meshName, mesh.primitives.size());

        for (size_t pIdx = 0; pIdx < mesh.primitives.size(); ++pIdx) {
            const auto& prim = mesh.primitives[pIdx];
            int matIdx = prim.materialIndex.has_value() ? static_cast<int>(*prim.materialIndex) : -1;
            std::string matName = (matIdx >= 0 && matIdx < static_cast<int>(scene->materials.size())) ? scene->materials[matIdx].name : "Default";

            size_t vertCount = 0;
            auto posIt = prim.findAttribute("POSITION");
            if (posIt != prim.attributes.end()) {
                vertCount = asset.accessors[posIt->second].count;
            }

            size_t idxCount = prim.indicesAccessor.has_value() ? asset.accessors[*prim.indicesAccessor].count : 0;
            size_t triCount = idxCount > 0 ? (idxCount / 3) : (vertCount / 3);

            BGL_LOG_DEBUG("    - Prim #{}: Material #{} ('{}') | {} vertices, {} indices ({} triangles)",
                         pIdx, matIdx, matName, vertCount, idxCount, triCount);

            for (const auto& [attrName, accIdx] : prim.attributes) {
                const auto& acc = asset.accessors[accIdx];
                BGL_LOG_DEBUG("      + Attr {}: Acc #{}, {} {}, count={}",
                             attrName, accIdx, AccessorTypeToString(acc.type), ComponentTypeToString(acc.componentType), acc.count);
            }
        }
    }

    // 4. Decode KHR_lights_punctual Lights
    if (!asset.lights.empty()) {
        BGL_LOG_INFO("[glTF Loader] --- KHR_lights_punctual ({} lights) ---", asset.lights.size());
        scene->lights.reserve(asset.lights.size());
        for (size_t lIdx{0}; lIdx < asset.lights.size(); ++lIdx) {
            const auto& gltfLight = asset.lights[lIdx];
            PunctualLight pLight{};
            pLight.name = gltfLight.name.empty() ? ("Light_" + std::to_string(lIdx)) : std::string{gltfLight.name};
            switch (gltfLight.type) {
                case fastgltf::LightType::Directional: pLight.type = LightType::Directional; break;
                case fastgltf::LightType::Point:       pLight.type = LightType::Point; break;
                case fastgltf::LightType::Spot:        pLight.type = LightType::Spot; break;
            }
            pLight.color = glm::vec3{
                static_cast<float>(gltfLight.color[0]),
                static_cast<float>(gltfLight.color[1]),
                static_cast<float>(gltfLight.color[2])
            };
            pLight.intensity = static_cast<float>(gltfLight.intensity);
            pLight.range = gltfLight.range.has_value() ? static_cast<float>(*gltfLight.range) : 0.0f;
            pLight.innerConeAngle = gltfLight.innerConeAngle.has_value() ? static_cast<float>(*gltfLight.innerConeAngle) : 0.0f;
            pLight.outerConeAngle = gltfLight.outerConeAngle.has_value() ? static_cast<float>(*gltfLight.outerConeAngle) : 0.785398163f;

            BGL_LOG_INFO("  * Light #{}: '{}' Type={} Color=[{:.2f}, {:.2f}, {:.2f}] Intensity={:.2f} Range={:.2f}",
                         lIdx, pLight.name,
                         pLight.type == LightType::Directional ? "Directional" : (pLight.type == LightType::Point ? "Point" : "Spot"),
                         pLight.color.r, pLight.color.g, pLight.color.b, pLight.intensity, pLight.range);
            scene->lights.push_back(std::move(pLight));
        }
    }

    // 5. Parse glTF Meshes and primitives in parallel (shared zero-copy resources across instances)
    std::vector<std::shared_ptr<Mesh>> parsedMeshes(asset.meshes.size());
    core::ThreadPool::Instance().ParallelFor(0, asset.meshes.size(), [&](size_t mIdx) {
        const auto& mesh = asset.meshes[mIdx];
        auto sharedMesh = std::make_shared<Mesh>();
        sharedMesh->name = mesh.name.empty() ? ("Mesh_" + std::to_string(mIdx)) : std::string(mesh.name);
        sharedMesh->primitives.reserve(mesh.primitives.size());

        for (const auto& prim : mesh.primitives) {
            if (prim.type != fastgltf::PrimitiveType::Triangles) continue;

            MeshPrimitive meshPrim;
                        if (prim.materialIndex.has_value()) {
                            meshPrim.materialIndex = static_cast<int>(*prim.materialIndex);
                        }

                        // POSITION accessor
                        auto posIt = prim.findAttribute("POSITION");
                        if (posIt != prim.attributes.end()) {
                            const auto& posAccessor = asset.accessors[posIt->second];
                            meshPrim.vertices.resize(posAccessor.count);

                            fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, posAccessor, [&](glm::vec3 pos, size_t idx) {
                                meshPrim.vertices[idx].position = pos;
                                meshPrim.boundingBox.Expand(pos);
                            });
                        }

                        // NORMAL accessor
                        auto normIt = prim.findAttribute("NORMAL");
                        if (normIt != prim.attributes.end()) {
                            const auto& normAccessor = asset.accessors[normIt->second];
                            fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, normAccessor, [&](glm::vec3 norm, size_t idx) {
                                if (idx < meshPrim.vertices.size()) {
                                    meshPrim.vertices[idx].normal = norm;
                                }
                            });
                        }

                        // TEXCOORD_0 accessor
                        auto uvIt = prim.findAttribute("TEXCOORD_0");
                        if (uvIt != prim.attributes.end()) {
                            const auto& uvAccessor = asset.accessors[uvIt->second];
                            fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, uvAccessor, [&](glm::vec2 uv, size_t idx) {
                                if (idx < meshPrim.vertices.size()) {
                                    meshPrim.vertices[idx].uv = uv;
                                }
                            });
                        }

                        // TANGENT accessor
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

                        // Indices accessor (with bounds clamping for GPU memory safety)
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
                                BGL_LOG_WARN("[glTF Loader] Mesh primitive contains out-of-bounds index references; clamped to 0 for GPU buffer safety");
                            }
                        }

                        // Flat normals fallback if NORMAL attribute is absent
                        if (normIt == prim.attributes.end() && !meshPrim.vertices.empty()) {
                            if (!meshPrim.indices.empty()) {
                                for (size_t i{0}; i + 2 < meshPrim.indices.size(); i += 3) {
                                    const uint32_t i0{meshPrim.indices[i]};
                                    const uint32_t i1{meshPrim.indices[i + 1]};
                                    const uint32_t i2{meshPrim.indices[i + 2]};
                                    if (i0 < meshPrim.vertices.size() && i1 < meshPrim.vertices.size() && i2 < meshPrim.vertices.size()) {
                                        const glm::vec3 p0{meshPrim.vertices[i0].position};
                                        const glm::vec3 p1{meshPrim.vertices[i1].position};
                                        const glm::vec3 p2{meshPrim.vertices[i2].position};
                                        glm::vec3 fn{glm::normalize(glm::cross(p1 - p0, p2 - p0))};
                                        if (glm::any(glm::isnan(fn))) fn = glm::vec3{0.0f, 1.0f, 0.0f};
                                        meshPrim.vertices[i0].normal += fn;
                                        meshPrim.vertices[i1].normal += fn;
                                        meshPrim.vertices[i2].normal += fn;
                                    }
                                }
                                for (auto& v : meshPrim.vertices) {
                                    const float len{glm::length(v.normal)};
                                    v.normal = (len > 0.0001f) ? (v.normal / len) : glm::vec3{0.0f, 1.0f, 0.0f};
                                }
                            } else {
                                for (size_t i{0}; i + 2 < meshPrim.vertices.size(); i += 3) {
                                    const glm::vec3 p0{meshPrim.vertices[i].position};
                                    const glm::vec3 p1{meshPrim.vertices[i + 1].position};
                                    const glm::vec3 p2{meshPrim.vertices[i + 2].position};
                                    glm::vec3 fn{glm::normalize(glm::cross(p1 - p0, p2 - p0))};
                                    if (glm::any(glm::isnan(fn))) fn = glm::vec3{0.0f, 1.0f, 0.0f};
                                    meshPrim.vertices[i].normal = fn;
                                    meshPrim.vertices[i + 1].normal = fn;
                                    meshPrim.vertices[i + 2].normal = fn;
                                }
                            }
                        }

                        // Auto-generate tangents for bump/normal mapping if absent or degenerate
                        bool needsTangentCompute{!hasExplicitTangents};
                        if (hasExplicitTangents && !meshPrim.vertices.empty()) {
                            const auto& firstTan{meshPrim.vertices.front().tangent};
                            const glm::vec3 tVec{firstTan.x, firstTan.y, firstTan.z};
                            if (glm::dot(tVec, tVec) < 0.25f || std::abs(firstTan.w) < 0.1f) {
                                needsTangentCompute = true;
                            }
                        }
                        if (needsTangentCompute) {
                            meshPrim.ComputeTangents();
                        }

                        sharedMesh->primitives.push_back(std::move(meshPrim));
        }
        parsedMeshes[mIdx] = std::move(sharedMesh);
    });
    scene->meshes = parsedMeshes;

    // 6. Instantiate Scene Graph Nodes in parallel
    std::vector<std::shared_ptr<SceneNode>> loadedNodes(asset.nodes.size());
    BGL_LOG_INFO("[glTF Loader] --- Scene Nodes Hierarchy ({} nodes total, parallel parsing) ---", asset.nodes.size());

    core::ThreadPool::Instance().ParallelFor(0, asset.nodes.size(), [&](size_t nodeIdx) {
        const auto& gltfNode = asset.nodes[nodeIdx];
        auto node = std::make_shared<SceneNode>();
                    node->nodeIndex = static_cast<int>(nodeIdx);
                    node->name = gltfNode.name.empty() ? ("Node_" + std::to_string(nodeIdx)) : std::string(gltfNode.name);
                    if (gltfNode.lightIndex.has_value() && *gltfNode.lightIndex < asset.lights.size()) {
                        node->lightIndex = static_cast<int>(*gltfNode.lightIndex);
                    }
                    if (gltfNode.cameraIndex.has_value() && *gltfNode.cameraIndex < asset.cameras.size()) {
                        node->cameraIndex = static_cast<int>(*gltfNode.cameraIndex);
                    }

                    if (std::holds_alternative<fastgltf::TRS>(gltfNode.transform)) {
                        const auto& trs = std::get<fastgltf::TRS>(gltfNode.transform);
                        glm::vec3 translation(trs.translation[0], trs.translation[1], trs.translation[2]);
                        glm::quat rotation(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
                        glm::vec3 scale(trs.scale[0], trs.scale[1], trs.scale[2]);

                        glm::mat4 tMat = glm::translate(glm::mat4(1.0f), translation);
                        glm::mat4 rMat = glm::mat4_cast(rotation);
                        glm::mat4 sMat = glm::scale(glm::mat4(1.0f), scale);
                        node->localTransform = tMat * rMat * sMat;
                    } else if (std::holds_alternative<fastgltf::Node::TransformMatrix>(gltfNode.transform)) {
                        const auto& mat = std::get<fastgltf::Node::TransformMatrix>(gltfNode.transform);
                        node->localTransform = glm::make_mat4(mat.data());
                    }

        if (gltfNode.meshIndex.has_value() && *gltfNode.meshIndex < parsedMeshes.size()) {
            node->mesh = parsedMeshes[*gltfNode.meshIndex];
        }

        loadedNodes[nodeIdx] = std::move(node);
    });

    // 7. Assemble parent-child scene hierarchy with cycle prevention
    std::vector<bool> isChild(asset.nodes.size(), false);
    for (size_t i{0}; i < asset.nodes.size(); ++i) {
        for (size_t childIdx : asset.nodes[i].children) {
            if (childIdx < loadedNodes.size() && childIdx != i && !isChild[childIdx]) {
                loadedNodes[i]->children.push_back(loadedNodes[childIdx]);
                isChild[childIdx] = true;
            }
        }
    }

    // 8. Register root nodes from active glTF scene (or fallback to parentless nodes)
    if (!asset.scenes.empty()) {
        const size_t activeSceneIdx{
            (asset.defaultScene.has_value() && *asset.defaultScene < asset.scenes.size())
                ? *asset.defaultScene
                : 0u
        };
        for (size_t rootNodeIdx : asset.scenes[activeSceneIdx].nodeIndices) {
            if (rootNodeIdx < loadedNodes.size() && loadedNodes[rootNodeIdx]) {
                scene->rootNodes.push_back(loadedNodes[rootNodeIdx]);
            }
        }
    }
    if (scene->rootNodes.empty()) {
        for (size_t i{0}; i < loadedNodes.size(); ++i) {
            if (!isChild[i] && loadedNodes[i]) {
                scene->rootNodes.push_back(loadedNodes[i]);
            }
        }
    }

    scene->allNodes = loadedNodes;
    scene->Update();

    // 8b. Parse glTF Cameras and extract world transforms
    scene->cameras.reserve(asset.cameras.size());
    for (size_t cIdx = 0; cIdx < asset.cameras.size(); ++cIdx) {
        const auto& cam = asset.cameras[cIdx];
        SceneCamera sc{};
        sc.name = cam.name.empty() ? ("Camera_" + std::to_string(cIdx)) : std::string(cam.name);
        if (std::holds_alternative<fastgltf::Camera::Perspective>(cam.camera)) {
            const auto& persp = std::get<fastgltf::Camera::Perspective>(cam.camera);
            sc.isPerspective = true;
            sc.yfov = static_cast<float>(persp.yfov);
            sc.znear = static_cast<float>(persp.znear);
            sc.zfar = persp.zfar.has_value() ? static_cast<float>(*persp.zfar) : 1000.0f;
        } else if (std::holds_alternative<fastgltf::Camera::Orthographic>(cam.camera)) {
            const auto& ortho = std::get<fastgltf::Camera::Orthographic>(cam.camera);
            sc.isPerspective = false;
            sc.xmag = static_cast<float>(ortho.xmag);
            sc.ymag = static_cast<float>(ortho.ymag);
            sc.znear = static_cast<float>(ortho.znear);
            sc.zfar = static_cast<float>(ortho.zfar);
        }

        // Find which node instantiates this camera to capture world transform
        for (size_t nIdx = 0; nIdx < loadedNodes.size(); ++nIdx) {
            if (loadedNodes[nIdx] && loadedNodes[nIdx]->cameraIndex == static_cast<int>(cIdx)) {
                sc.nodeIndex = static_cast<int>(nIdx);
                sc.worldTransform = loadedNodes[nIdx]->worldTransform;
                break;
            }
        }
        scene->cameras.push_back(std::move(sc));
    }
    if (!scene->cameras.empty()) {
        BGL_LOG_INFO("[glTF Loader] Successfully parsed {} glTF camera(s)", scene->cameras.size());
    }

    // 9. Display ASCII Hierarchy Tree Diagram & Telemetry
    BGL_LOG_INFO("--------------------------------------------------------------------------------");
    BGL_LOG_INFO("[glTF Loader] Scene Graph Tree Diagram (Root Scene):");
    auto printTree = [&](auto& self, const std::shared_ptr<SceneNode>& n, const std::string& prefix, bool isLast) -> void {
        if (!n) return;
        std::string nodeInfo{n->name.empty() ? "(Unnamed Node)" : n->name};
        auto prims = n->GetPrimitives();
        if (!prims.empty()) {
            size_t totalVerts{0};
            size_t totalIndices{0};
            for (const auto& p : prims) {
                totalVerts += p.vertices.size();
                totalIndices += p.indices.size();
            }
            nodeInfo += " [Mesh: " + std::to_string(prims.size()) + " prims, " +
                        std::to_string(totalVerts) + " verts, " + std::to_string(totalIndices / 3) + " tris]";
        }
        BGL_LOG_INFO("{}{}-- {}", prefix, isLast ? "`" : "|", nodeInfo);
        for (size_t c{0}; c < n->children.size(); ++c) {
            const bool childLast{c + 1 == n->children.size()};
            self(self, n->children[c], prefix + (isLast ? "   " : "|  "), childLast);
        }
    };
    for (size_t r{0}; r < scene->rootNodes.size(); ++r) {
        printTree(printTree, scene->rootNodes[r], "  ", (r + 1 == scene->rootNodes.size()));
    }
    BGL_LOG_INFO("--------------------------------------------------------------------------------");

    glm::vec3 minB = scene->sceneAABB.minBound;
    glm::vec3 maxB = scene->sceneAABB.maxBound;
    glm::vec3 center = scene->sceneAABB.Center();
    glm::vec3 extents = scene->sceneAABB.Extents();
    float radius = glm::length(extents);

    BGL_LOG_INFO("[glTF Loader] Ingestion Finished Successfully:");
    BGL_LOG_INFO("  * Total Nodes:        {}", asset.nodes.size());
    BGL_LOG_INFO("  * Total Meshes:       {}", asset.meshes.size());
    BGL_LOG_INFO("  * Total Materials:    {}", scene->materials.size());
    BGL_LOG_INFO("  * Total Textures:     {}", scene->cpuImages.size());
    BGL_LOG_INFO("  * Total Binary Size:  {} bytes ({:.2f} MB)", totalBufferBytes, totalBufferBytes / (1024.0 * 1024.0));
    BGL_LOG_INFO("  * Scene Bounds (min): [{:.4f}, {:.4f}, {:.4f}]", minB.x, minB.y, minB.z);
    BGL_LOG_INFO("  * Scene Bounds (max): [{:.4f}, {:.4f}, {:.4f}]", maxB.x, maxB.y, maxB.z);
    BGL_LOG_INFO("  * Scene Center:       [{:.4f}, {:.4f}, {:.4f}]", center.x, center.y, center.z);
    BGL_LOG_INFO("  * Scene Extents:      [{:.4f}, {:.4f}, {:.4f}] (Radius: {:.4f})", extents.x, extents.y, extents.z, radius);
    BGL_LOG_INFO("================================================================================");

    return scene;
}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromFile(std::string_view path) {
    if (!core::NetworkFetcher::IsUrl(path) && std::filesystem::exists(path)) {
        fastgltf::GltfDataBuffer dataBuffer{};
        if (dataBuffer.loadFromFile(std::filesystem::path(path))) {
            auto sp = static_cast<fastgltf::span<std::byte>>(dataBuffer);
            return ParseAndAssembleScene(dataBuffer, path, sp.size());
        }
    }
    auto fetchRes = core::NetworkFetcher::FetchSync(path);
    if (!fetchRes) {
        return std::unexpected("Failed to fetch glTF file: " + fetchRes.error());
    }
    return LoadFromMemory(*fetchRes, path);
}

} // namespace bgl::scene
