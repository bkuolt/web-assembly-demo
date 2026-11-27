/**
 * @file MemoryPool.hpp
 * @brief Fixed-block memory pool and polymorphic memory resource (PMR) staging arena.
 */
#ifndef BGL_CORE_MEMORYPOOL_HPP
#define BGL_CORE_MEMORYPOOL_HPP

#include "core/Types.hpp"
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <utility>
#include <vector>

namespace bgl::core {

/**
 * @class MemoryPool
 * @brief Block-based memory pool with a free-list to avoid heap fragmentation.
 * @tparam T Object type.
 * @tparam BlockSize Number of elements allocated per contiguous block.
 */
template <typename T, size_t BlockSize = 256>
class MemoryPool {
    static_assert(BlockSize > 0, "MemoryPool BlockSize must be greater than zero");

public:
    MemoryPool() = default;
    ~MemoryPool() { Clear(); }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&& other) noexcept
        : blocks_(std::move(other.blocks_)), freeHead_(std::exchange(other.freeHead_, nullptr)),
          allocatedCount_(std::exchange(other.allocatedCount_, 0)) {}

    MemoryPool& operator=(MemoryPool&& other) noexcept {
        if (this != &other) {
            Clear();
            blocks_ = std::move(other.blocks_);
            freeHead_ = std::exchange(other.freeHead_, nullptr);
            allocatedCount_ = std::exchange(other.allocatedCount_, 0);
        }
        return *this;
    }

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
        node->occupied = true;
        allocatedCount_++;

        try {
            T* memory = reinterpret_cast<T*>(&node->storage);
            return ::new (static_cast<void*>(memory)) T(std::forward<Args>(args)...);
        } catch (...) {
            node->occupied = false;
            node->next = freeHead_;
            freeHead_ = node;
            allocatedCount_--;
            throw;
        }
    }

    /**
     * @brief Destruct and return object storage back to the pool free list.
     */
    void Deallocate(T* ptr) noexcept {
        if (!ptr) {
            return;
        }
        Node* node = reinterpret_cast<Node*>(ptr);
        bool owned{false};
        for (Node* block : blocks_) {
            const auto addr = reinterpret_cast<uintptr_t>(node);
            const auto begin = reinterpret_cast<uintptr_t>(block);
            const auto end = reinterpret_cast<uintptr_t>(block + BlockSize);
            if (addr >= begin && addr < end && ((addr - begin) % sizeof(Node)) == 0u) {
                owned = true;
                break;
            }
        }
        if (!owned || !node->occupied) {
            return;
        }
        node->occupied = false;
        ptr->~T();
        node->next = freeHead_;
        freeHead_ = node;
        if (allocatedCount_ > 0) {
            allocatedCount_--;
        }
    }

    [[nodiscard]] size_t AllocatedCount() const noexcept { return allocatedCount_; }
    [[nodiscard]] size_t BlockCount() const noexcept { return blocks_.size(); }

    /**
     * @brief Destroys any live objects and releases all allocated blocks.
     */
    void Clear() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (Node* block : blocks_) {
                for (size_t i{0}; i < BlockSize; ++i) {
                    if (block[i].occupied) {
                        block[i].occupied = false;
                        reinterpret_cast<T*>(&block[i].storage)->~T();
                    }
                }
            }
        }
        for (auto* block : blocks_) {
            delete[] block;
        }
        blocks_.clear();
        freeHead_ = nullptr;
        allocatedCount_ = 0;
    }

private:
    struct Node {
        union {
            alignas(std::max(alignof(T), alignof(void*))) std::byte storage[std::max(sizeof(T), sizeof(void*))];
            Node* next;
        };
        bool occupied{false};
    };

    void AllocateNewBlock() {
        Node* newBlock{new Node[BlockSize]};
        blocks_.push_back(newBlock);
        for (size_t i{0}; i < BlockSize - 1; ++i) {
            newBlock[i].next = &newBlock[i + 1];
            newBlock[i].occupied = false;
        }
        newBlock[BlockSize - 1].next = freeHead_;
        newBlock[BlockSize - 1].occupied = false;
        freeHead_ = &newBlock[0];
    }

    std::vector<Node*> blocks_{};
    Node* freeHead_{nullptr};
    size_t allocatedCount_{0};
};

/**
 * @class ScratchArena
 * @brief O(1) resettable monotonic buffer resource staging arena for per-frame uploads.
 */
class ScratchArena {
public:
    explicit ScratchArena(size_t initialCapacity = 1024 * 1024 * 4); // 4 MB default
    ~ScratchArena() = default;

    ScratchArena(const ScratchArena&) = delete;
    ScratchArena& operator=(const ScratchArena&) = delete;
    ScratchArena(ScratchArena&&) noexcept = default;
    ScratchArena& operator=(ScratchArena&&) noexcept = default;

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
    size_t capacity_{0};
    std::unique_ptr<std::byte[]> buffer_{nullptr};
    std::unique_ptr<std::pmr::monotonic_buffer_resource> monotonicResource_{nullptr};
};

} // namespace bgl::core

#endif // BGL_CORE_MEMORYPOOL_HPP
