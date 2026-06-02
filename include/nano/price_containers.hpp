#pragma once

#include "nano/price_level.hpp"
#include "nano/types.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <map>
#include <type_traits>
#include <vector>

// Three interchangeable ways to store the price levels of one book side. They
// exist so the benchmarks can put them head to head and let the data pick the
// winner rather than folklore. Every container satisfies the same concept, so
// they are truly swappable.
//
// A book side is ordered so that the best price is always first. For the bid
// side the best price is the highest. For the ask side the best price is the
// lowest. Each container is parameterised on that direction through the IsBid
// flag.

namespace nano {

// The common contract. Any type that models this can back a book side.
template <typename C>
concept PriceLevelContainer = requires(C c, const C cc, Price p, PriceLevel pl) {
    { c.insert(p, std::move(pl)) } -> std::same_as<void>;
    { c.erase(p) }                 -> std::same_as<void>;
    { c.find(p) }                  -> std::same_as<PriceLevel*>;
    { c.best() }                   -> std::same_as<PriceLevel*>;
    { cc.empty() }                 -> std::same_as<bool>;
    { cc.size() }                  -> std::same_as<std::size_t>;
};

// Option A. A red black tree. Insert, erase, and find are all O(log N). Nodes
// are heap allocated and scattered, so iteration touches memory all over the
// place. This is the safe default and the one the production OrderBook uses.
template <bool IsBid>
class MapContainer {
    using Cmp = std::conditional_t<IsBid, std::greater<Price>, std::less<Price>>;

public:
    void insert(Price p, PriceLevel pl) { levels_.insert_or_assign(p, std::move(pl)); }
    void erase(Price p) { levels_.erase(p); }

    [[nodiscard]] PriceLevel* find(Price p) {
        auto it = levels_.find(p);
        return it == levels_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] PriceLevel* best() {
        return levels_.empty() ? nullptr : &levels_.begin()->second;
    }
    [[nodiscard]] bool empty() const { return levels_.empty(); }
    [[nodiscard]] std::size_t size() const { return levels_.size(); }

private:
    std::map<Price, PriceLevel, Cmp> levels_;
};

// Option B. A sorted contiguous array. Find is O(log N) by binary search, but
// insert and erase are O(N) because the tail must shift. The payoff is perfect
// cache locality. Walking the book is a straight sequential scan, and the best
// price is simply the front element. This tends to win when the number of live
// levels is small, which is the common case near the top of book.
template <bool IsBid>
class SortedVectorContainer {
    static constexpr bool before(Price a, Price b) { return IsBid ? a > b : a < b; }

    auto locate(Price p) {
        return std::lower_bound(levels_.begin(), levels_.end(), p,
                                [](const auto& e, Price x) { return before(e.first, x); });
    }

public:
    void insert(Price p, PriceLevel pl) {
        auto it = locate(p);
        if (it != levels_.end() && it->first == p) {
            it->second = std::move(pl);
            return;
        }
        levels_.insert(it, {p, std::move(pl)});
    }
    void erase(Price p) {
        auto it = locate(p);
        if (it != levels_.end() && it->first == p) levels_.erase(it);
    }
    [[nodiscard]] PriceLevel* find(Price p) {
        auto it = locate(p);
        return (it != levels_.end() && it->first == p) ? &it->second : nullptr;
    }
    [[nodiscard]] PriceLevel* best() {
        return levels_.empty() ? nullptr : &levels_.front().second;
    }
    [[nodiscard]] bool empty() const { return levels_.empty(); }
    [[nodiscard]] std::size_t size() const { return levels_.size(); }

private:
    std::vector<std::pair<Price, PriceLevel>> levels_;
};

// Option C. A direct addressed array indexed by the offset of a price from a
// fixed base. Insert, erase, and find are all O(1) with no comparisons at all.
// The cost is memory proportional to the whole price window rather than the
// number of live levels, and a scan to find the next best price when the current
// best is removed. This wins outright when the tradable price range is bounded
// and known ahead of time, which is true on many real venues.
template <bool IsBid>
class ArrayContainer {
public:
    ArrayContainer() = default;
    explicit ArrayContainer(Price base, std::size_t range = 1u << 17)
        : base_(base), slots_(range), present_(range, false) {}

    void insert(Price p, PriceLevel pl) {
        const std::size_t idx = index(p);
        if (idx >= slots_.size()) return; // Outside the configured window.
        if (!present_[idx]) {
            present_[idx] = true;
            ++count_;
            if (best_idx_ == kNone || better(idx, best_idx_)) best_idx_ = idx;
        }
        slots_[idx] = std::move(pl);
    }
    void erase(Price p) {
        const std::size_t idx = index(p);
        if (idx >= slots_.size() || !present_[idx]) return;
        present_[idx] = false;
        --count_;
        if (idx == best_idx_) rescan_best();
    }
    [[nodiscard]] PriceLevel* find(Price p) {
        const std::size_t idx = index(p);
        return (idx < slots_.size() && present_[idx]) ? &slots_[idx] : nullptr;
    }
    [[nodiscard]] PriceLevel* best() {
        return count_ == 0 ? nullptr : &slots_[best_idx_];
    }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] std::size_t size() const { return count_; }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    [[nodiscard]] std::size_t index(Price p) const {
        return static_cast<std::size_t>(p - base_);
    }
    // A lower index is a lower price. For bids the best is the highest index, for
    // asks the best is the lowest index.
    [[nodiscard]] static bool better(std::size_t a, std::size_t b) {
        return IsBid ? a > b : a < b;
    }
    void rescan_best() {
        best_idx_ = kNone;
        if (count_ == 0) return;
        if constexpr (IsBid) {
            for (std::size_t i = slots_.size(); i-- > 0;) {
                if (present_[i]) { best_idx_ = i; return; }
            }
        } else {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (present_[i]) { best_idx_ = i; return; }
            }
        }
    }

    Price                   base_     = 0;
    std::vector<PriceLevel> slots_;
    std::vector<bool>       present_;
    std::size_t             count_    = 0;
    std::size_t             best_idx_ = kNone;
};

// Confirm all three model the concept. If any drifts out of shape this fails to
// compile, which is exactly the guard we want.
static_assert(PriceLevelContainer<MapContainer<true>>);
static_assert(PriceLevelContainer<MapContainer<false>>);
static_assert(PriceLevelContainer<SortedVectorContainer<true>>);
static_assert(PriceLevelContainer<SortedVectorContainer<false>>);
static_assert(PriceLevelContainer<ArrayContainer<true>>);
static_assert(PriceLevelContainer<ArrayContainer<false>>);

} // namespace nano
