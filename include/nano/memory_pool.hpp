#pragma once

#include "nano/types.hpp"
#include <cstddef>
#include <cstring>
#include <new>
#include <utility>

namespace nano {

// A fixed capacity object pool. It pre-allocates one contiguous block of
// Capacity objects up front, then hands them out and takes them back in O(1)
// with no syscalls and no locking. This removes the single biggest source of
// tail latency in the matching path, which is a call into the general purpose
// heap allocator.
//
// Free slots are threaded into an intrusive singly linked free list. The next
// pointer for a free slot lives inside the slot's own storage, so the free list
// costs no extra memory. This requires sizeof(T) to be at least the size of a
// pointer, which we assert.
template <typename T, std::size_t Capacity = 1'000'000>
class MemoryPool {
    static_assert(Capacity > 0, "Pool capacity must be positive");
    static_assert(sizeof(T) >= sizeof(void*),
                  "T must be at least pointer sized to hold the free list link");

public:
    MemoryPool() {
        storage_ = static_cast<std::byte*>(
            ::operator new[](Capacity * sizeof(T), std::align_val_t(64)));
        // Build the free list. Slot i points at slot i+1, last points at null.
        free_head_ = slot(0);
        for (std::size_t i = 0; i + 1 < Capacity; ++i) {
            set_next_free(slot(i), slot(i + 1));
        }
        set_next_free(slot(Capacity - 1), nullptr);
    }

    ~MemoryPool() {
        ::operator delete[](storage_, std::align_val_t(64));
    }

    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // Pop the head of the free list. Returns nullptr when the pool is exhausted.
    // The caller is responsible for constructing the object in place.
    template <typename... Args>
    [[nodiscard]] NANO_ALWAYS_INLINE T* allocate(Args&&... args) noexcept {
        if (NANO_UNLIKELY(free_head_ == nullptr)) return nullptr;
        void* mem = free_head_;
        free_head_ = next_free(free_head_);
        ++allocated_;
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    // Push a slot back onto the free list. The object is destroyed first.
    NANO_ALWAYS_INLINE void deallocate(T* ptr) noexcept {
        if (NANO_UNLIKELY(ptr == nullptr)) return;
        ptr->~T();
        set_next_free(ptr, free_head_);
        free_head_ = ptr;
        --allocated_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return allocated_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return Capacity; }
    [[nodiscard]] bool full() const noexcept { return allocated_ == Capacity; }
    [[nodiscard]] bool empty() const noexcept { return allocated_ == 0; }

private:
    T* slot(std::size_t i) noexcept {
        return reinterpret_cast<T*>(storage_ + i * sizeof(T));
    }
    // Read and write the free list link stored inside an unused slot.
    static T* next_free(void* p) noexcept {
        T* n;
        std::memcpy(&n, p, sizeof(T*));
        return n;
    }
    static void set_next_free(void* p, T* n) noexcept {
        std::memcpy(p, &n, sizeof(T*));
    }

    std::byte*  storage_   = nullptr;
    T*          free_head_ = nullptr;
    std::size_t allocated_ = 0;
};

} // namespace nano
