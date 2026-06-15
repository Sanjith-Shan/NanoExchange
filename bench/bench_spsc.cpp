// Single producer single consumer queue shootout. Three implementations move the
// same payload under the same access pattern.
//   1. nano::SPSCQueue, the lock free ring
//   2. std::mutex guarding a std::queue
//   3. std::mutex guarding a fixed ring buffer
//
// Two access patterns are measured. A single threaded push then pop loop that
// isolates the per operation cost, and a two thread producer consumer transfer
// that exposes real contention. Throughput comes from SetItemsProcessed.

#include <benchmark/benchmark.h>

#include "nano/spsc_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>

using namespace nano;

namespace {

using Payload = uint64_t;

constexpr std::size_t kCap      = 1u << 16;
constexpr int64_t     kTransfer = 200'000; // Items moved per two thread iteration.

// Baseline 2. A mutex around std::queue.
template <typename T>
class MutexStdQueue {
public:
    bool try_push(const T& v) {
        std::lock_guard<std::mutex> lk(m_);
        q_.push(v);
        return true;
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop();
        return true;
    }

private:
    std::mutex    m_;
    std::queue<T> q_;
};

// Baseline 3. A mutex around a fixed capacity ring buffer.
template <typename T, std::size_t Cap>
class MutexRing {
public:
    bool try_push(const T& v) {
        std::lock_guard<std::mutex> lk(m_);
        const std::size_t next = (tail_ + 1) & MASK;
        if (next == head_) return false;
        buf_[tail_] = v;
        tail_ = next;
        return true;
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (head_ == tail_) return false;
        out = buf_[head_];
        head_ = (head_ + 1) & MASK;
        return true;
    }

private:
    static constexpr std::size_t MASK = Cap - 1;
    std::mutex  m_;
    T           buf_[Cap]{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
};

// Single threaded push then pop. One item makes a full round trip per iteration.
template <typename Q>
void single_thread(benchmark::State& state, Q& q) {
    Payload v = 42;
    for (auto _ : state) {
        (void)q.try_push(v);
        Payload out = 0;
        (void)q.try_pop(out);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}

// Two thread transfer. A producer thread pushes kTransfer items while the
// benchmark thread consumes them. Both sides spin when the queue is full or
// empty. The thread spawn cost is amortised over the large transfer count.
template <typename Q>
void two_thread(benchmark::State& state, Q& q) {
    for (auto _ : state) {
        std::thread producer([&q] {
            for (int64_t i = 0; i < kTransfer; ++i) {
                Payload v = static_cast<Payload>(i);
                while (!q.try_push(v)) { /* spin until space */ }
            }
        });
        int64_t got = 0;
        Payload out = 0;
        while (got < kTransfer) {
            if (q.try_pop(out)) {
                benchmark::DoNotOptimize(out);
                ++got;
            }
        }
        producer.join();
    }
    state.SetItemsProcessed(state.iterations() * kTransfer);
}

} // namespace

static void BM_SPSC_Single_Nano(benchmark::State& state) {
    SPSCQueue<Payload, kCap> q;
    single_thread(state, q);
}
BENCHMARK(BM_SPSC_Single_Nano);

static void BM_SPSC_Single_MutexQueue(benchmark::State& state) {
    MutexStdQueue<Payload> q;
    single_thread(state, q);
}
BENCHMARK(BM_SPSC_Single_MutexQueue);

static void BM_SPSC_Single_MutexRing(benchmark::State& state) {
    MutexRing<Payload, kCap> q;
    single_thread(state, q);
}
BENCHMARK(BM_SPSC_Single_MutexRing);

static void BM_SPSC_TwoThread_Nano(benchmark::State& state) {
    SPSCQueue<Payload, kCap> q;
    two_thread(state, q);
}
BENCHMARK(BM_SPSC_TwoThread_Nano)->UseRealTime();

static void BM_SPSC_TwoThread_MutexQueue(benchmark::State& state) {
    MutexStdQueue<Payload> q;
    two_thread(state, q);
}
BENCHMARK(BM_SPSC_TwoThread_MutexQueue)->UseRealTime();

static void BM_SPSC_TwoThread_MutexRing(benchmark::State& state) {
    MutexRing<Payload, kCap> q;
    two_thread(state, q);
}
BENCHMARK(BM_SPSC_TwoThread_MutexRing)->UseRealTime();
