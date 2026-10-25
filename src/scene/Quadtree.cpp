/**
 * @file Quadtree.cpp
 * @brief Implementation of Quadtree spatial hierarchy, view frustum culling, and LOD selection.
 */
#include "scene/Quadtree.hpp"
#include <algorithm>
#include <cmath>

namespace bgl::scene {

namespace {

[[nodiscard]] inline bool ContainsXZ(const core::AABB& outer, const core::AABB& inner) noexcept {
    constexpr float kEps = 1e-3f;
    return (inner.minBound.x >= outer.minBound.x - kEps) &&
           (inner.maxBound.x <= outer.maxBound.x + kEps) &&
           (inner.minBound.z >= outer.minBound.z - kEps) &&
           (inner.maxBound.z <= outer.maxBound.z + kEps);
}

[[nodiscard]] inline bool OverlapsXZ(const core::AABB& a, const core::AABB& b) noexcept {
    return (a.minBound.x <= b.maxBound.x && a.maxBound.x >= b.minBound.x) &&
           (a.minBound.z <= b.maxBound.z && a.maxBound.z >= b.minBound.z);
}

} // namespace

QuadtreeNode::QuadtreeNode(const core::AABB& bounds, size_t depth)
    : bounds_(bounds), depth_(depth) {}

void QuadtreeNode::Subdivide() {
    glm::vec3 center = bounds_.Center();
    glm::vec3 minB = bounds_.minBound;
    glm::vec3 maxB = bounds_.maxBound;

    // 4 quadrants on XZ plane
    core::AABB b0{ {minB.x, minB.y, minB.z}, {center.x, maxB.y, center.z} };
    core::AABB b1{ {center.x, minB.y, minB.z}, {maxB.x, maxB.y, center.z} };
    core::AABB b2{ {minB.x, minB.y, center.z}, {center.x, maxB.y, maxB.z} };
    core::AABB b3{ {center.x, minB.y, center.z}, {maxB.x, maxB.y, maxB.z} };

    children_[0] = std::make_unique<QuadtreeNode>(b0, depth_ + 1);
    children_[1] = std::make_unique<QuadtreeNode>(b1, depth_ + 1);
    children_[2] = std::make_unique<QuadtreeNode>(b2, depth_ + 1);
    children_[3] = std::make_unique<QuadtreeNode>(b3, depth_ + 1);

    isLeaf_ = false;

    auto oldItems = std::move(items_);
    items_.clear();

    for (const auto& item : oldItems) {
        bool pushed = false;
        for (auto& child : children_) {
            if (ContainsXZ(child->bounds_, item.aabb)) {
                child->Insert(item);
                pushed = true;
                break;
            }
        }
        if (!pushed) {
            items_.push_back(item);
        }
    }
}

bool QuadtreeNode::Insert(const QuadtreeItem& item) {
    if (!OverlapsXZ(bounds_, item.aabb)) {
        return false;
    }

    looseBounds_.Expand(item.aabb);

    if (isLeaf_) {
        items_.push_back(item);
        if (items_.size() > kMaxItems && depth_ < kMaxDepth) {
            Subdivide();
        }
        return true;
    }

    for (auto& child : children_) {
        if (child && ContainsXZ(child->bounds_, item.aabb)) {
            return child->Insert(item);
        }
    }

    items_.push_back(item);
    return true;
}

bool OcclusionBuffer::ProjectAABB(const core::AABB& box,
                                  const glm::mat4& viewProj,
                                  glm::vec2& ndcMinOut,
                                  glm::vec2& ndcMaxOut,
                                  float& minZOut,
                                  float& maxZOut) noexcept {
    if (box.minBound.x > box.maxBound.x) return false;

    ndcMinOut = glm::vec2{1e30f};
    ndcMaxOut = glm::vec2{-1e30f};
    minZOut = 1e30f;
    maxZOut = -1e30f;

    const glm::vec3& minB{box.minBound};
    const glm::vec3& maxB{box.maxBound};
    const std::array<glm::vec3, 8> corners{{
        {minB.x, minB.y, minB.z},
        {maxB.x, minB.y, minB.z},
        {minB.x, maxB.y, minB.z},
        {maxB.x, maxB.y, minB.z},
        {minB.x, minB.y, maxB.z},
        {maxB.x, minB.y, maxB.z},
        {minB.x, maxB.y, maxB.z},
        {maxB.x, maxB.y, maxB.z}
    }};

    for (const auto& corner : corners) {
        const glm::vec4 clip{viewProj * glm::vec4{corner, 1.0f}};
        if (clip.w <= 0.05f) {
            // Box intersects or straddles camera near plane -> cannot be safely occluded
            return false;
        }
        const float invW{1.0f / clip.w};
        const glm::vec3 ndc{clip.x * invW, clip.y * invW, clip.z * invW};
        ndcMinOut = glm::min(ndcMinOut, glm::vec2{ndc.x, ndc.y});
        ndcMaxOut = glm::max(ndcMaxOut, glm::vec2{ndc.x, ndc.y});
        minZOut = std::min(minZOut, ndc.z);
        maxZOut = std::max(maxZOut, ndc.z);
    }

    ndcMinOut = glm::clamp(ndcMinOut, glm::vec2{-1.0f}, glm::vec2{1.0f});
    ndcMaxOut = glm::clamp(ndcMaxOut, glm::vec2{-1.0f}, glm::vec2{1.0f});
    minZOut = std::clamp(minZOut, 0.0f, 1.0f);
    maxZOut = std::clamp(maxZOut, 0.0f, 1.0f);
    return (ndcMinOut.x <= ndcMaxOut.x && ndcMinOut.y <= ndcMaxOut.y);
}

bool OcclusionBuffer::IsOccluded(const glm::vec2& ndcMin,
                                 const glm::vec2& ndcMax,
                                 float minZ) const noexcept {
    const float uMin{ndcMin.x * 0.5f + 0.5f};
    const float uMax{ndcMax.x * 0.5f + 0.5f};
    const float vMin{ndcMin.y * 0.5f + 0.5f};
    const float vMax{ndcMax.y * 0.5f + 0.5f};

    const uint32_t x0{std::min(kWidth - 1, static_cast<uint32_t>(std::max(0.0f, std::floor(uMin * kWidth))))};
    const uint32_t x1{std::min(kWidth - 1, static_cast<uint32_t>(std::max(0.0f, std::floor(uMax * kWidth))))};
    const uint32_t y0{std::min(kHeight - 1, static_cast<uint32_t>(std::max(0.0f, std::floor(vMin * kHeight))))};
    const uint32_t y1{std::min(kHeight - 1, static_cast<uint32_t>(std::max(0.0f, std::floor(vMax * kHeight))))};

    float maxTileDepth{0.0f};
    for (uint32_t y{y0}; y <= y1; ++y) {
        const size_t rowOffset{static_cast<size_t>(y) * kWidth};
        for (uint32_t x{x0}; x <= x1; ++x) {
            maxTileDepth = std::max(maxTileDepth, tiles_[rowOffset + x]);
        }
    }

    // Object is occluded only if its nearest point (minZ) is strictly behind the farthest depth in all covered tiles
    constexpr float kBias{0.002f};
    return (minZ > maxTileDepth + kBias);
}

void OcclusionBuffer::RasterizeOccluder(const glm::vec2& ndcMin,
                                        const glm::vec2& ndcMax,
                                        float occluderMaxZ) noexcept {
    // Shrink NDC rect conservatively inward by 20% so only the solid interior core marks tiles as occluded
    const glm::vec2 center{(ndcMin + ndcMax) * 0.5f};
    const glm::vec2 halfExt{(ndcMax - ndcMin) * 0.5f * 0.80f};
    if (halfExt.x < (2.0f / kWidth) || halfExt.y < (2.0f / kHeight)) {
        return; // Too narrow to fully cover an interior screen tile
    }

    const glm::vec2 innerMin{center - halfExt};
    const glm::vec2 innerMax{center + halfExt};

    const float uMin{innerMin.x * 0.5f + 0.5f};
    const float uMax{innerMax.x * 0.5f + 0.5f};
    const float vMin{innerMin.y * 0.5f + 0.5f};
    const float vMax{innerMax.y * 0.5f + 0.5f};

    // Use ceil for start and floor for end so only 100%-covered interior tiles are updated
    const int x0{static_cast<int>(std::ceil(uMin * kWidth))};
    const int x1{static_cast<int>(std::floor(uMax * kWidth)) - 1};
    const int y0{static_cast<int>(std::ceil(vMin * kHeight))};
    const int y1{static_cast<int>(std::floor(vMax * kHeight)) - 1};

    if (x0 > x1 || y0 > y1) return;

    const uint32_t cx0{static_cast<uint32_t>(std::clamp<int>(x0, 0, kWidth - 1))};
    const uint32_t cx1{static_cast<uint32_t>(std::clamp<int>(x1, 0, kWidth - 1))};
    const uint32_t cy0{static_cast<uint32_t>(std::clamp<int>(y0, 0, kHeight - 1))};
    const uint32_t cy1{static_cast<uint32_t>(std::clamp<int>(y1, 0, kHeight - 1))};

    for (uint32_t y{cy0}; y <= cy1; ++y) {
        const size_t rowOffset{static_cast<size_t>(y) * kWidth};
        for (uint32_t x{cx0}; x <= cx1; ++x) {
            tiles_[rowOffset + x] = std::min(tiles_[rowOffset + x], occluderMaxZ);
        }
    }
}

void QuadtreeNode::QueryFrustum(const core::Frustum& frustum,
                                const glm::vec3& camPos,
                                std::vector<QuadtreeItem>& visibleItemsOut,
                                const glm::mat4* viewProj,
                                OcclusionBuffer* occlusionBuf,
                                size_t* occludedCountOut) const {
    // 1. Broad-phase Frustum Culling: cull whole quadtree quadrant if outside frustum
    const core::AABB& testBounds{(looseBounds_.minBound.x <= looseBounds_.maxBound.x) ? looseBounds_ : bounds_};
    if (!frustum.IntersectsAABB(testBounds)) {
        return;
    }

    // 1b. Hierarchical Quadtree Node Occlusion Culling: if the whole quadrant is behind near occluders, skip entire subtree
    if (viewProj && occlusionBuf) {
        glm::vec2 nodeNdcMin{}, nodeNdcMax{};
        float nodeMinZ{0.0f}, nodeMaxZ{1.0f};
        if (OcclusionBuffer::ProjectAABB(testBounds, *viewProj, nodeNdcMin, nodeNdcMax, nodeMinZ, nodeMaxZ)) {
            if (occlusionBuf->IsOccluded(nodeNdcMin, nodeNdcMax, nodeMinZ)) {
                if (occludedCountOut) {
                    *occludedCountOut += items_.size();
                }
                return;
            }
        }
    }

    // 2. Narrow-phase Frustum + Occlusion + Contribution Culling for items in this node
    const size_t startIdx{visibleItemsOut.size()};
    for (const auto& item : items_) {
        if (frustum.IntersectsAABB(item.aabb)) {
            const float radius{std::max(0.001f, glm::length(item.aabb.Extents()) * 0.5f)};
            const float dist{glm::distance(camPos, item.aabb.Center())};
            const float relativeDist{dist / radius};

            if (viewProj && occlusionBuf) {
                glm::vec2 itemNdcMin{}, itemNdcMax{};
                float itemMinZ{0.0f}, itemMaxZ{1.0f};
                if (OcclusionBuffer::ProjectAABB(item.aabb, *viewProj, itemNdcMin, itemNdcMax, itemMinZ, itemMaxZ)) {
                    const glm::vec2 ndcSpan{itemNdcMax - itemNdcMin};
                    const float ndcArea{ndcSpan.x * ndcSpan.y};

                    // Sub-pixel contribution culling for distant micro-primitives (< ~1.5x1.5 px at 1080p)
                    if (relativeDist > 22.0f && ndcArea < 0.000012f) {
                        if (occludedCountOut) (*occludedCountOut)++;
                        continue;
                    }

                    // Screen-space Hi-Z tile occlusion test
                    if (occlusionBuf->IsOccluded(itemNdcMin, itemNdcMax, itemMinZ)) {
                        if (occludedCountOut) (*occludedCountOut)++;
                        continue;
                    }

                    // Register large near/mid objects as conservative occluders
                    if (ndcArea > 0.035f) {
                        occlusionBuf->RasterizeOccluder(itemNdcMin, itemNdcMax, itemMaxZ);
                    }
                }
            }

            QuadtreeItem visibleItem{item};
            visibleItem.distanceToCamera = dist;
            if (relativeDist < 8.0f) {
                visibleItem.currentLOD = LODLevel::LOD0;
            } else if (relativeDist < 25.0f) {
                visibleItem.currentLOD = LODLevel::LOD1;
            } else {
                visibleItem.currentLOD = LODLevel::LOD2;
            }
            visibleItemsOut.push_back(std::move(visibleItem));
        }
    }

    // Sort items added in this quadrant Front-to-Back (nearest to camera first)
    if (visibleItemsOut.size() - startIdx > 1) {
        std::sort(visibleItemsOut.begin() + static_cast<std::ptrdiff_t>(startIdx),
                  visibleItemsOut.end(),
                  [](const QuadtreeItem& a, const QuadtreeItem& b) {
                      return a.distanceToCamera < b.distanceToCamera;
                  });
    }

    // 3. Recurse into children in Front-to-Back order (nearest quadrant to camera first)
    if (!isLeaf_) {
        struct ChildOrder {
            size_t index{0};
            float distSq{1e30f};
        };
        std::array<ChildOrder, 4> order{{
            {0, 1e30f}, {1, 1e30f}, {2, 1e30f}, {3, 1e30f}
        }};
        for (size_t i{0}; i < 4; ++i) {
            if (children_[i]) {
                const glm::vec3 d{children_[i]->bounds_.Center() - camPos};
                order[i].distSq = glm::dot(d, d);
            }
        }
        std::sort(order.begin(), order.end(), [](const ChildOrder& a, const ChildOrder& b) {
            return a.distSq < b.distSq;
        });

        for (const auto& entry : order) {
            if (children_[entry.index]) {
                children_[entry.index]->QueryFrustum(frustum, camPos, visibleItemsOut, viewProj, occlusionBuf, occludedCountOut);
            }
        }
    }
}

Quadtree::Quadtree(const core::AABB& worldBounds)
    : worldBounds_{worldBounds},
      root_{std::make_unique<QuadtreeNode>(worldBounds)} {}

void Quadtree::InsertNodes(std::span<const std::shared_ptr<SceneNode>> nodes) {
    for (const auto& node : nodes) {
        Insert(node);
    }
}

void Quadtree::Insert(std::shared_ptr<SceneNode> node) {
    if (!node) return;
    auto insertRecursive = [&](auto& self, const std::shared_ptr<SceneNode>& n) -> void {
        if (!n) return;
        auto prims = n->GetPrimitives();
        if (prims.empty()) {
            if (n->children.empty() && n->worldAABB.minBound.x <= n->worldAABB.maxBound.x) {
                QuadtreeItem item;
                item.node = n;
                item.primitiveIndex = 0;
                item.aabb = n->worldAABB;
                if (!root_->Insert(item)) {
                    root_->ForceInsert(item);
                }
            }
        } else {
            for (size_t pIdx = 0; pIdx < prims.size(); ++pIdx) {
                const auto& prim = prims[pIdx];
                if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
                    QuadtreeItem item;
                    item.node = n;
                    item.primitiveIndex = pIdx;

                    // Compute primitive world-space bounding box
                    core::AABB primWorldAABB;
                    glm::vec3 c = prim.boundingBox.Center();
                    glm::vec3 e = prim.boundingBox.Extents();
                    glm::vec3 corners[8] = {
                        c + glm::vec3(-e.x, -e.y, -e.z),
                        c + glm::vec3( e.x, -e.y, -e.z),
                        c + glm::vec3(-e.x,  e.y, -e.z),
                        c + glm::vec3( e.x,  e.y, -e.z),
                        c + glm::vec3(-e.x, -e.y,  e.z),
                        c + glm::vec3( e.x, -e.y,  e.z),
                        c + glm::vec3(-e.x,  e.y,  e.z),
                        c + glm::vec3( e.x,  e.y,  e.z)
                    };
                    for (const auto& corner : corners) {
                        glm::vec4 worldCorner = n->worldTransform * glm::vec4(corner, 1.0f);
                        primWorldAABB.Expand(glm::vec3(worldCorner));
                    }
                    item.aabb = primWorldAABB;
                    if (!root_->Insert(item)) {
                        root_->ForceInsert(item);
                    }
                }
            }
        }

        for (const auto& child : n->children) {
            self(self, child);
        }
    };
    insertRecursive(insertRecursive, node);
}

void Quadtree::Clear() {
    root_ = std::make_unique<QuadtreeNode>(worldBounds_);
}

void Quadtree::Reset(const core::AABB& worldBounds) {
    worldBounds_ = worldBounds;
    root_ = std::make_unique<QuadtreeNode>(worldBounds_);
}

std::vector<QuadtreeItem> Quadtree::QueryVisible(const core::Frustum& frustum,
                                                 const glm::vec3& cameraPos,
                                                 const glm::mat4& viewProj,
                                                 bool enableOcclusionCulling) const {
    std::vector<QuadtreeItem> results{};
    lastOccludedCount_ = 0;
    if (root_) {
        if (enableOcclusionCulling) {
            OcclusionBuffer occlusionBuf{};
            root_->QueryFrustum(frustum, cameraPos, results, &viewProj, &occlusionBuf, &lastOccludedCount_);
        } else {
            root_->QueryFrustum(frustum, cameraPos, results, nullptr, nullptr, nullptr);
        }
    }
    return results;
}

std::vector<QuadtreeItem> Quadtree::QueryVisible(const core::Frustum& frustum, const glm::vec3& cameraPos) const {
    std::vector<QuadtreeItem> results{};
    lastOccludedCount_ = 0;
    if (root_) {
        root_->QueryFrustum(frustum, cameraPos, results, nullptr, nullptr, nullptr);
    }
    return results;
}

std::vector<QuadtreeItem> Quadtree::QueryVisible(const glm::vec3& cameraPos) const {
    core::Frustum infiniteFrustum;
    for (auto& plane : infiniteFrustum.planes) {
        plane.normal = glm::vec3(0.0f);
        plane.distance = 1e30f;
    }
    return QueryVisible(infiniteFrustum, cameraPos);
}

void Quadtree::GetNodeBounds(std::vector<core::AABB>& boundsOut) const {
    auto collect = [&](auto& self, const QuadtreeNode& n) -> void {
        boundsOut.push_back(n.GetBounds());
        if (!n.IsLeaf()) {
            for (const auto& child : n.GetChildren()) {
                if (child) {
                    self(self, *child);
                }
            }
        }
    };
    if (root_) {
        collect(collect, *root_);
    }
}

size_t Quadtree::GetNodeCount() const noexcept {
    size_t count = 0;
    auto countNodes = [&](auto& self, const QuadtreeNode& n) -> void {
        count++;
        if (!n.IsLeaf()) {
            for (const auto& child : n.GetChildren()) {
                if (child) {
                    self(self, *child);
                }
            }
        }
    };
    if (root_) {
        countNodes(countNodes, *root_);
    }
    return count;
}

} // namespace bgl::scene
