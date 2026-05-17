#pragma once

#include "nano/types.hpp"
#include <array>
#include <atomic>
#include <cstddef>

namespace nano {

namespace detail {
// Round up to the next power of two at compile time so the modulo in the ring
// buffer becomes a single bitwise AND.
constexpr std::size_t round_up_pow2(std::size_t v) {
    std::size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}
} // namespace detail

// A lock free single producer single consumer ring buffer.
//
// Exactly one thread pushes and exactly one thread pops. Under that contract we
// need no locks and no compare and swap loops. Correctness rests on two ideas.
// First, the producer owns tail_ and the consumer owns head_, so neither index
// is written by two threads. Second, acquire and release ordering publishes the
// data write before the index update the other side reads, so a consumer that
// sees an advanced tail_ is guaranteed to see the payload written before it.
//
// head_ and tail_ live on separate cache lines so the two threads never fight
// over the same line, which would otherwise cause false sharing and destroy
// throughput.
template <typename T, std::size_t Capacity = 65536>
class SPSCQueue {
public:
    static constexpr std::size_t CAPACITY = detail::round_up_pow2(Capacity);
    static constexpr std::size_t MASK     = CAPACITY - 1;

    SPSCQueue() = default;
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Producer side. Returns false when the queue is full.
    [[nodiscard]] NANO_ALWAYS_INLINE bool try_push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & MASK;
        // Acquire so we observe the consumer's most recent head advance.
        if (NANO_UNLIKELY(next == head_.load(std::memory_order_acquire))) {
            return false; // Full.
        }
        buffer_[tail] = item;
        // Release so a consumer that reads this new tail also sees buffer_[tail].
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when the queue is empty.
    [[nodiscard]] NANO_ALWAYS_INLINE bool try_pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // Acquire so we observe the producer's most recent tail advance and the
        // payload it published before that advance.
        if (NANO_UNLIKELY(head == tail_.load(std::memory_order_acquire))) {
            return false; // Empty.
        }
        out = buffer_[head];
        head_.store((head + 1) & MASK, std::memory_order_release);
        return true;
    }

    // Approximate count. Safe to call from either thread but may be stale.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return (t - h) & MASK;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return CAPACITY - 1; }

private:
    alignas(64) std::atomic<std::size_t> head_{0}; // Owned by the consumer.
    alignas(64) std::atomic<std::size_t> tail_{0}; // Owned by the producer.
    alignas(64) std::array<T, CAPACITY> buffer_{};
};

} // namespace nano
