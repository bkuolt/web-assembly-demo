/**
 * @file MemoryPool.hpp
 * @brief Fixed-block memory pool and polymorphic memory resource (PMR) staging arena.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory_resource>
#include <new>
#include <concepts>
#include "core/Types.hpp"

namespace bgl::core {

/**
 * @class MemoryPool
 * @brief Block-based memory pool with a free-list to avoid heap fragmentation.
 * @tparam T Object type.
 * @tparam BlockSize Number of elements allocated per contiguous block.
 */
template <typename T, size_t BlockSize = 256>
class MemoryPool {
public:
    MemoryPool() = default;
    ~MemoryPool() {
        Clear();
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&& other) noexcept = default;
    MemoryPool& operator=(MemoryPool&& other) noexcept = default;

    /**
     * @brief Allocate and construct an object of type T using forwarding arguments.
     */
    template <typename... Args>
    T* Allocate(Args&&... args) {
        if (!freeHead_) {
            AllocateNewBlock();
        }

        Node* node = freeHead_;
        freeHead_ = freeHead_->next;
        allocatedCount_++;

        T* memory = reinterpret_cast<T*>(&node->storage);
        return ::new (static_cast<void*>(memory)) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Destruct and return object storage back to the pool free list.
     */
    void Deallocate(T* ptr) noexcept {
        if (!ptr) return;
        ptr->~T();
        Node* node = reinterpret_cast<Node*>(ptr);
        node->next = freeHead_;
        freeHead_ = node;
        allocatedCount_--;
    }

    [[nodiscard]] size_t AllocatedCount() const noexcept { return allocatedCount_; }
    [[nodiscard]] size_t BlockCount() const noexcept { return blocks_.size(); }

    /**
     * @brief Releases all allocated blocks.
     */
    void Clear() {
        for (auto* block : blocks_) {
            delete[] block;
        }
        blocks_.clear();
        freeHead_ = nullptr;
        allocatedCount_ = 0;
    }

private:
    union Node {
        alignas(std::max(alignof(T), alignof(void*))) std::byte storage[std::max(sizeof(T), sizeof(void*))];
        Node* next;
    };

    void AllocateNewBlock() {
        Node* newBlock = new Node[BlockSize];
        blocks_.push_back(newBlock);
        for (size_t i = 0; i < BlockSize - 1; ++i) {
            newBlock[i].next = &newBlock[i + 1];
        }
        newBlock[BlockSize - 1].next = freeHead_;
        freeHead_ = &newBlock[0];
    }

    std::vector<Node*> blocks_;
    Node* freeHead_ = nullptr;
    size_t allocatedCount_ = 0;
};

/**
 * @class ScratchArena
 * @brief O(1) resettable monotonic buffer resource staging arena for per-frame uploads.
 */
class ScratchArena {
public:
    explicit ScratchArena(size_t initialCapacity = 1024 * 1024 * 4); // 4 MB default
    ~ScratchArena();

    /**
     * @brief Reset all allocations in O(1) without freeing the backing memory.
     */
    void Reset() noexcept;

    /**
     * @brief Access the underlying std::pmr::memory_resource.
     */
    [[nodiscard]] std::pmr::memory_resource* GetResource() noexcept;

    [[nodiscard]] size_t Capacity() const noexcept { return capacity_; }

private:
    size_t capacity_;
    std::byte* buffer_ = nullptr;
    std::pmr::monotonic_buffer_resource* monotonicResource_ = nullptr;
};

} // namespace bgl::core

