#pragma once

#include <cstdint>
#include <chrono>

// Core scalar types used across the whole engine.
// Prices are integers measured in ticks. There is no floating point anywhere
// in the hot path. Real exchanges work this way because integer comparison is
// exact, fast, and free of rounding surprises.

namespace nano {

using Price     = int64_t;   // Price in ticks. One tick is the smallest increment.
using Quantity  = uint32_t;  // Share or contract count.
using OrderId   = uint64_t;  // Unique per order.
using SymbolId  = uint32_t;  // Interned instrument id. Avoids string compares in the hot path.
using Timestamp = uint64_t;  // Nanoseconds on a steady monotonic clock.

enum class Side : uint8_t { Buy, Sell };

enum class OrderType : uint8_t {
    Limit,              // Rest on the book if not fully matched.
    Market,             // Match at any price. Never rests.
    ImmediateOrCancel,  // Match what is possible now. Cancel the remainder.
    FillOrKill          // Fill the whole order at once or reject it entirely.
};

// Sentinels.
constexpr Price    INVALID_PRICE    = 0;
constexpr OrderId  INVALID_ORDER_ID = 0;
constexpr SymbolId INVALID_SYMBOL   = 0xFFFFFFFFu;

// Monotonic nanosecond clock. steady_clock never runs backwards, which matters
// when timestamps decide time priority. Do not use system_clock here.
[[nodiscard]] inline Timestamp now() noexcept {
    return static_cast<Timestamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Branch hints for the matching fast path. The book is almost never empty when
// an aggressive order arrives, so we tell the compiler which way to bet.
#if defined(__GNUC__) || defined(__clang__)
#define NANO_LIKELY(x)   __builtin_expect(!!(x), 1)
#define NANO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define NANO_HOT         __attribute__((hot))
#define NANO_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define NANO_LIKELY(x)   (x)
#define NANO_UNLIKELY(x) (x)
#define NANO_HOT
#define NANO_ALWAYS_INLINE inline
#endif

} // namespace nano
