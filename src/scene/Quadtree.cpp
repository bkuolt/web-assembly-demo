/**
 * @file Quadtree.cpp
 * @brief Implementation of Quadtree spatial hierarchy and LOD selection.
 */
#include "scene/Quadtree.hpp"

namespace bgl::scene {

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
    for (const auto& item : oldItems) {
        Insert(item);
    }
}

bool QuadtreeNode::Insert(const QuadtreeItem& item) {
    glm::vec3 itemCenter = item.aabb.Center();
    if (itemCenter.x < bounds_.minBound.x || itemCenter.x > bounds_.maxBound.x ||
        itemCenter.z < bounds_.minBound.z || itemCenter.z > bounds_.maxBound.z) {
        return false;
    }

    if (isLeaf_) {
        items_.push_back(item);
        if (items_.size() > kMaxItems && depth_ < kMaxDepth) {
            Subdivide();
        }
        return true;
    }

    for (auto& child : children_) {
        if (child->Insert(item)) return true;
    }

    items_.push_back(item);
    return true;
}

void QuadtreeNode::QueryFrustum(const glm::vec3& camPos, std::vector<QuadtreeItem>& visibleItemsOut) const {
    for (auto item : items_) {
        float dist = glm::distance(camPos, item.aabb.Center());
        if (dist < 15.0f) {
            item.currentLOD = LODLevel::LOD0;
        } else if (dist < 50.0f) {
            item.currentLOD = LODLevel::LOD1;
        } else {
            item.currentLOD = LODLevel::LOD2;
        }
        visibleItemsOut.push_back(item);
    }

    if (!isLeaf_) {
        for (const auto& child : children_) {
            child->QueryFrustum(camPos, visibleItemsOut);
        }
    }
}

Quadtree::Quadtree(const core::AABB& worldBounds)
    : root_(std::make_unique<QuadtreeNode>(worldBounds)) {}

void Quadtree::Insert(std::shared_ptr<SceneNode> node) {
    if (!node) return;
    QuadtreeItem item;
    item.node = node;
    item.aabb = node->worldAABB;
    root_->Insert(item);
}

void Quadtree::UpdateLOD(const glm::vec3& cameraPos) {
    (void)cameraPos;
}

std::vector<QuadtreeItem> Quadtree::QueryVisible(const glm::vec3& cameraPos) const {
    std::vector<QuadtreeItem> results;
    root_->QueryFrustum(cameraPos, results);
    return results;
}

} // namespace bgl::scene

