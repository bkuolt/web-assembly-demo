/**
 * @file MemoryPool.cpp
 * @brief Implementation of ScratchArena PMR staging allocator.
 */
#include "core/MemoryPool.hpp"

namespace bgl::core {

ScratchArena::ScratchArena(size_t initialCapacity)
    : capacity_(initialCapacity),
      buffer_(new std::byte[initialCapacity]),
      monotonicResource_(new std::pmr::monotonic_buffer_resource(buffer_, capacity_, std::pmr::get_default_resource())) {}

ScratchArena::~ScratchArena() {
    delete monotonicResource_;
    delete[] buffer_;
}

void ScratchArena::Reset() noexcept {
    delete monotonicResource_;
    monotonicResource_ = new std::pmr::monotonic_buffer_resource(buffer_, capacity_, std::pmr::get_default_resource());
}

std::pmr::memory_resource* ScratchArena::GetResource() noexcept {
    return monotonicResource_;
}

} // namespace bgl::core

