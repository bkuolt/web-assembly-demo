/**
 * @file MemoryPool.cpp
 * @brief Implementation of ScratchArena PMR staging allocator.
 */
#include "core/MemoryPool.hpp"
#include <memory>

namespace bgl::core {

ScratchArena::ScratchArena(size_t initialCapacity)
    : capacity_(initialCapacity), buffer_(std::make_unique<std::byte[]>(initialCapacity)),
      monotonicResource_(std::make_unique<std::pmr::monotonic_buffer_resource>(buffer_.get(), capacity_,
                                                                               std::pmr::get_default_resource())) {}

void ScratchArena::Reset() noexcept {
    if (monotonicResource_) {
        monotonicResource_->release();
    }
}

std::pmr::memory_resource* ScratchArena::GetResource() noexcept {
    return monotonicResource_.get();
}

} // namespace bgl::core
