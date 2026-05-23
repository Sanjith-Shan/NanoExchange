#pragma once

#include "nano/order.hpp"
#include "nano/types.hpp"

namespace nano {

// All resting orders at one price, held in arrival order so the oldest order
// trades first. This is the time half of price time priority.
//
// The list is intrusive. It threads through the prev and next fields already
// present in Order, so appending or removing an order touches only pointers and
// never allocates. Removal is O(1) because we hold both neighbours through the
// order itself rather than searching for it.
class PriceLevel {
public:
    PriceLevel() = default;
    explicit PriceLevel(Price price) noexcept : price_(price) {}

    // Append to the tail so newer orders sit behind older ones.
    NANO_ALWAYS_INLINE void append(Order* order) noexcept {
        order->next = nullptr;
        order->prev = tail_;
        if (tail_) {
            tail_->next = order;
        } else {
            head_ = order;
        }
        tail_ = order;
        total_qty_ += order->remaining_qty;
        ++count_;
    }

    // Unlink an order from anywhere in the level in O(1).
    NANO_ALWAYS_INLINE void remove(Order* order) noexcept {
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            head_ = order->next;
        }
        if (order->next) {
            order->next->prev = order->prev;
        } else {
            tail_ = order->prev;
        }
        order->prev = nullptr;
        order->next = nullptr;
        total_qty_ -= order->remaining_qty;
        --count_;
    }

    // Called when an order at the front partially fills. Keeps the running total
    // in sync without walking the list.
    NANO_ALWAYS_INLINE void reduce(Quantity by) noexcept { total_qty_ -= by; }

    [[nodiscard]] Order*   front()          const noexcept { return head_; }
    [[nodiscard]] Price    price()          const noexcept { return price_; }
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_qty_; }
    [[nodiscard]] uint32_t order_count()    const noexcept { return count_; }
    [[nodiscard]] bool     empty()          const noexcept { return head_ == nullptr; }

private:
    Price    price_     = INVALID_PRICE;
    Order*   head_      = nullptr; // Oldest order. Trades first.
    Order*   tail_      = nullptr; // Newest order.
    Quantity total_qty_ = 0;
    uint32_t count_     = 0;
};

} // namespace nano
