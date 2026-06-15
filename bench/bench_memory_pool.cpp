// Allocation strategy shootout for fixed size Order sized objects. Every strategy
// runs the same batch pattern. It allocates a batch of objects, then frees the
// whole batch, so both the allocation and the reclamation path are exercised.
//
// The five contenders are
//   1. nano::MemoryPool, the engine free list pool
//   2. plain new and delete
//   3. std::pmr::monotonic_buffer_resource, which does not reclaim per object and
//      is reset in bulk between batches, which is how it is meant to be used
//   4. std::pmr::unsynchronized_pool_resource, which does reclaim per object
//   5. an mmap pre faulted region handing out slots from a hand rolled free list
//
// macOS lacks MAP_POPULATE so it is guarded and we pre fault by writing one byte
// per page instead.

#include <benchmark/benchmark.h>

#include "nano/memory_pool.hpp"
#include "nano/order.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

using namespace nano;

namespace {

constexpr std::size_t kBatch = 4'096; // Objects allocated then freed per iteration.
constexpr std::size_t kSlot  = sizeof(Order);

// A minimal bump plus free list allocator over one pre faulted mmap region.
class MmapArena {
public:
    explicit MmapArena(std::size_t slots) : slots_(slots) {
        const std::size_t bytes = slots_ * kSlot;
        int flags = MAP_ANON | MAP_PRIVATE;
#ifdef MAP_POPULATE
        flags |= MAP_POPULATE;
#endif
        base_ = static_cast<std::byte*>(
            ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0));
        // Pre fault every page. On macOS MAP_POPULATE is absent so touching one
        // byte per page forces the kernel to back the mapping up front.
        const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        for (std::size_t off = 0; off < bytes; off += page) {
            base_[off] = std::byte{0};
        }
        free_head_ = nullptr;
        bump_ = 0;
    }
    ~MmapArena() { ::munmap(base_, slots_ * kSlot); }

    MmapArena(const MmapArena&)            = delete;
    MmapArena& operator=(const MmapArena&) = delete;

    void* allocate() noexcept {
        if (free_head_) {
            void* p = free_head_;
            free_head_ = *reinterpret_cast<void**>(free_head_);
            return p;
        }
        if (bump_ >= slots_) return nullptr;
        return base_ + (bump_++) * kSlot;
    }
    void deallocate(void* p) noexcept {
        *reinterpret_cast<void**>(p) = free_head_;
        free_head_ = p;
    }

private:
    std::byte*  base_      = nullptr;
    std::size_t slots_     = 0;
    std::size_t bump_      = 0;
    void*       free_head_ = nullptr;
};

} // namespace

// Strategy 1. The engine free list pool.
static void BM_Alloc_NanoPool(benchmark::State& state) {
    MemoryPool<Order, kBatch * 2> pool;
    std::vector<Order*> live(kBatch);
    for (auto _ : state) {
        for (std::size_t i = 0; i < kBatch; ++i)
            live[i] = pool.allocate(OrderId{i}, Side::Buy, OrderType::Limit, 100, 10, 0, 1);
        for (std::size_t i = 0; i < kBatch; ++i)
            pool.deallocate(live[i]);
        benchmark::DoNotOptimize(live.data());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBatch));
}
BENCHMARK(BM_Alloc_NanoPool);

// Strategy 2. Plain new and delete.
static void BM_Alloc_NewDelete(benchmark::State& state) {
    std::vector<Order*> live(kBatch);
    for (auto _ : state) {
        for (std::size_t i = 0; i < kBatch; ++i) live[i] = new Order();
        for (std::size_t i = 0; i < kBatch; ++i) delete live[i];
        benchmark::DoNotOptimize(live.data());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBatch));
}
BENCHMARK(BM_Alloc_NewDelete);

// Strategy 3. pmr monotonic buffer. It never reclaims a single object, so the
// fair pattern is a bulk release once the batch is done.
static void BM_Alloc_PmrMonotonic(benchmark::State& state) {
    std::vector<std::byte> buffer(kBatch * kSlot + 4'096);
    std::vector<void*> live(kBatch);
    for (auto _ : state) {
        std::pmr::monotonic_buffer_resource mono(buffer.data(), buffer.size());
        for (std::size_t i = 0; i < kBatch; ++i) {
            void* p = mono.allocate(kSlot, alignof(Order));
            live[i] = ::new (p) Order();
        }
        for (std::size_t i = 0; i < kBatch; ++i)
            static_cast<Order*>(live[i])->~Order();
        mono.release(); // Bulk reclaim, the intended use.
        benchmark::DoNotOptimize(live.data());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBatch));
}
BENCHMARK(BM_Alloc_PmrMonotonic);

// Strategy 4. pmr unsynchronized pool. This one does reclaim per object.
static void BM_Alloc_PmrPool(benchmark::State& state) {
    std::pmr::unsynchronized_pool_resource pool;
    std::vector<void*> live(kBatch);
    for (auto _ : state) {
        for (std::size_t i = 0; i < kBatch; ++i) {
            void* p = pool.allocate(kSlot, alignof(Order));
            live[i] = ::new (p) Order();
        }
        for (std::size_t i = 0; i < kBatch; ++i) {
            static_cast<Order*>(live[i])->~Order();
            pool.deallocate(live[i], kSlot, alignof(Order));
        }
        benchmark::DoNotOptimize(live.data());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBatch));
}
BENCHMARK(BM_Alloc_PmrPool);

// Strategy 5. mmap pre faulted region with a manual free list.
static void BM_Alloc_MmapArena(benchmark::State& state) {
    MmapArena arena(kBatch * 2);
    std::vector<void*> live(kBatch);
    for (auto _ : state) {
        for (std::size_t i = 0; i < kBatch; ++i) {
            void* p = arena.allocate();
            live[i] = ::new (p) Order();
        }
        for (std::size_t i = 0; i < kBatch; ++i) {
            static_cast<Order*>(live[i])->~Order();
            arena.deallocate(live[i]);
        }
        benchmark::DoNotOptimize(live.data());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBatch));
}
BENCHMARK(BM_Alloc_MmapArena);
