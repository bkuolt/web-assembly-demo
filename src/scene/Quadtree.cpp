/**
 * @file Quadtree.cpp
 * @brief Implementation of Quadtree spatial hierarchy, view frustum culling, and LOD selection.
 */
#include "scene/Quadtree.hpp"

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

void QuadtreeNode::QueryFrustum(const core::Frustum& frustum, const glm::vec3& camPos, std::vector<QuadtreeItem>& visibleItemsOut) const {
    // 1. Broad-phase test: cull whole quadtree quadrant if outside frustum
    const core::AABB& testBounds = (looseBounds_.minBound.x <= looseBounds_.maxBound.x) ? looseBounds_ : bounds_;
    if (!frustum.IntersectsAABB(testBounds)) {
        return;
    }

    // 2. Narrow-phase test for items in this node
    for (const auto& item : items_) {
        if (frustum.IntersectsAABB(item.aabb)) {
            QuadtreeItem visibleItem = item;
            const float radius = std::max(0.001f, glm::length(item.aabb.Extents()) * 0.5f);
            const float dist = glm::distance(camPos, item.aabb.Center());
            const float relativeDist = dist / radius;
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

    // 3. Recurse into children
    if (!isLeaf_) {
        for (const auto& child : children_) {
            if (child) {
                child->QueryFrustum(frustum, camPos, visibleItemsOut);
            }
        }
    }
}

Quadtree::Quadtree(const core::AABB& worldBounds)
    : worldBounds_(worldBounds),
      root_(std::make_unique<QuadtreeNode>(worldBounds)) {}

void Quadtree::Insert(std::shared_ptr<SceneNode> node) {
    if (!node) return;
    auto insertRecursive = [&](auto& self, const std::shared_ptr<SceneNode>& n) -> void {
        if (!n) return;
        if (n->primitives.empty()) {
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
            for (size_t pIdx = 0; pIdx < n->primitives.size(); ++pIdx) {
                const auto& prim = n->primitives[pIdx];
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

std::vector<QuadtreeItem> Quadtree::QueryVisible(const core::Frustum& frustum, const glm::vec3& cameraPos) const {
    std::vector<QuadtreeItem> results;
    if (root_) {
        root_->QueryFrustum(frustum, cameraPos, results);
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
