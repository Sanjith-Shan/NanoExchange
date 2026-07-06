# NanoExchange Design and Performance Notes

This document explains what the engine does, how the hot path is built, why each
data structure was chosen, and where a real production venue would go further. It
is meant to be read by an engineer who cares about performance and does not
assume any finance background.

## 1. Problem statement

An order book is the data structure at the center of every electronic market. It
holds all the resting buy orders and sell orders for one instrument. The highest
price a buyer is willing to pay is the best bid. The lowest price a seller is
willing to accept is the best ask. The gap between them is the spread. When a new
order arrives that is willing to trade at a price the other side already offers,
the two orders match and a trade prints.

Matching follows price time priority. Better prices trade first. Among orders at
the same price, the one that arrived first trades first. The order already resting
on the book is the maker. The incoming order that crosses the spread is the taker.
Trades print at the maker price, so the taker can receive price improvement
relative to its own limit. This rule set is simple to state and unforgiving to
implement, because every operation has to preserve two orderings at once, one by
price and one by time.

Performance matters because the book is hit constantly and the tail of the
latency distribution is what hurts. A market maker quotes on both sides and
updates those quotes as the world moves. If a cancel or a requote takes an extra
microsecond at the ninety ninth percentile, that is a microsecond during which a
stale quote sits exposed to smarter traders. The goal here is not only a low
average. The goal is a low and predictable tail, which means avoiding the things
that cause occasional multi microsecond stalls. Those things are heap allocation,
cache misses, and lock contention.

## 2. Hot path analysis

The most common expensive operation is a limit order that crosses and matches. It
is worth walking the whole path for a buy order that lifts one resting sell.

1. The engine interns the symbol string to an integer once at the edge. Every
   step after that compares integers only.
2. `MatchingEngine::submit` pulls one `Order` out of the memory pool. This is a
   pointer read and a free list pop. There is no call into malloc.
3. `OrderBook::add_order` dispatches on order type. For a plain limit the branch
   predictor learns the common direction quickly because most orders are limits.
4. `match` looks at the best opposite level. For a buy that is `asks_.begin()`,
   which on a `std::map` is the leftmost node. The price is compared against the
   taker limit. If it does not cross the loop exits immediately.
5. If it crosses, the front resting order at that level is read through the
   intrusive list head pointer. The executable quantity is the minimum of the two
   remaining quantities. A `Trade` is appended to the output vector.
6. Both remaining quantities are decremented. If the maker is now fully filled it
   is unlinked from the level in O(1) using its own prev and next pointers, erased
   from the id map, and returned to the pool.
7. When the taker still has quantity and the price still crosses, the loop
   repeats. When the taker is exhausted or the price no longer crosses, the loop
   ends. A leftover limit is threaded onto its own side and recorded in the id map.

The irreducible work for a single fill is a handful of pointer reads, two integer
subtractions, one comparison, and one push onto the trade vector. On a warm cache
that is a few nanosecond of real work. Everything in the design exists to keep the
memory those steps touch inside L1 and L2 and to keep the allocator out of the
path. The measured median for an add that rests is around 125 nanoseconds on an
unpinned laptop, and a matching add is a little higher because it also writes
trades. The mean and the far tail are much larger than the median on this
hardware, and section 4 explains why that is measurement noise rather than engine
work.

## 3. Data structure choices with benchmark data

Three choices dominate performance. They are the container that holds price
levels, the allocator that hands out order objects, and the queue that moves
orders between threads. For each one the project implements the real alternatives
and measures them head to head rather than trusting intuition. The raw numbers
live in `results/benchmarks.txt` and the charts live in `results/`. The
`scripts/plot_latency.py` script regenerates the charts from the JSON.

### 3a. Price level container

Three containers all model the same `PriceLevelContainer` concept in
`include/nano/price_containers.hpp`, so they are drop in interchangeable.

- `MapContainer` wraps `std::map`. It is a red black tree with O(log N) insert,
  erase, and find. Nodes are individually heap allocated and scattered in memory,
  so iterating the book chases pointers across cache lines.
- `SortedVectorContainer` keeps a sorted contiguous array. Find is O(log N) by
  binary search and best is the front element, but insert and erase are O(N)
  because the tail has to shift. The payoff is that the whole side lives in one
  cache friendly run of memory.
- `ArrayContainer` is a direct addressed array indexed by the offset of a price
  from a fixed base. Insert, erase, and find are all O(1) with no comparisons. The
  cost is memory proportional to the whole price window and a scan to find the
  next best price when the current best is removed.

The tables below report throughput in millions of operations per second, so
higher is better. The workloads are read heavy (mostly best price queries),
insert heavy (mostly new levels), and mixed at sixty percent insert, thirty
percent erase, and ten percent best price query. Level counts sweep from a
shallow book to a deep one.

Read heavy, millions of ops per second.

| Live levels | Map (tree) | Sorted vector | Array |
|-------------|-----------:|--------------:|------:|
| 100         | 44.9       | 74.3          | 1058  |
| 10000       | 14.5       | 24.3          | 1056  |

Insert heavy, millions of ops per second.

| Live levels | Map (tree) | Sorted vector | Array |
|-------------|-----------:|--------------:|------:|
| 100         | 53.8       | 64.9          | 1313  |
| 10000       | 15.0       | 3.3           | 618   |

Mixed sixty thirty ten, millions of ops per second.

| Live levels | Map (tree) | Sorted vector | Array |
|-------------|-----------:|--------------:|------:|
| 100         | 27.1       | 32.1          | 228   |
| 10000       | 10.8       | 0.82          | 208   |

The array container wins every workload at every depth, often by more than an
order of magnitude, because its operations are a single indexed access with no
comparison and no pointer chase. The sorted vector is strong at a shallow book
where its contiguous layout makes reads fast, but it collapses on the insert
heavy and mixed workloads once the book is deep, because every insert or erase
shifts an O(N) tail. That is the classic array versus tree tradeoff made visible.
Notice that at ten thousand levels the tree actually overtakes the sorted vector
for mutation heavy work, exactly where the O(N) shifting cost dominates the O(log
N) tree.

The production `OrderBook` uses `std::map` as its default. That choice is
deliberate even though the array container has the best asymptotics. The map never
needs to know the price range ahead of time, it degrades gracefully when the book
is deep and sparse, and its per operation cost is dominated by a few cache misses
that are hard to avoid for a general venue. The array container wins outright when
the tradable price range is bounded and dense, which is true for a single liquid
name over a short horizon, and section 6 notes that a real venue often does shard
by instrument and pin the range so the array becomes viable.

### 3b. Memory allocator

`bench_memory_pool.cpp` compares four strategies for handing out fixed size order
objects. They are the custom `MemoryPool`, plain `new` and `delete`, a standard
library `std::pmr` pool resource, and a hand managed region from `mmap` that is
pre faulted so no page fault happens on first touch.

Throughput of a repeated allocate and deallocate cycle in millions of operations
per second, so higher is better.

| Strategy                     | Throughput | Relative to pool |
|------------------------------|-----------:|-----------------:|
| MemoryPool (this project)    | 370 M/s    | 1.00x            |
| std::pmr monotonic buffer    | 375 M/s    | 1.01x            |
| mmap pre faulted region      | 355 M/s    | 0.96x            |
| std::pmr pool (reclaiming)   | 122 M/s    | 0.33x            |
| new and delete (general heap)| 47 M/s     | 0.13x            |

The honest reading of this table is that the exact winner among the arena style
allocators does not matter. The custom pool, the standard monotonic buffer, and
the hand managed mmap region are all within a few percent of each other, because
all three do the same thing, which is hand out a slot from a pre reserved block
by moving a pointer. What matters is the gap to the general heap. The pool is
roughly eight times faster than new and delete and three times faster than the
reclaiming standard pool resource.

The reason the general heap loses is the tail rather than the average. A general
allocator has to be thread safe and has to handle arbitrary sizes, and it
occasionally takes a slow path into the operating system to grow its arena, which
is a syscall costing hundreds or thousands of nanoseconds. The pool never does
this because it pre allocates the whole arena once and only ever moves a pointer
along a free list afterward. The result is a latency distribution that is a tight
spike rather than a spike with a long right tail. For a matching engine the tight
tail is the entire point, and the tables above understate the win because they
report average throughput rather than the worst case that the pool is really
there to eliminate.

### 3c. Single producer single consumer queue

`bench_spsc.cpp` compares the lock free `SPSCQueue` against a `std::mutex` around
a `std::queue` and a `std::mutex` around a ring buffer.

Single thread round trip latency in nanoseconds, so lower is better. This is one
thread pushing and immediately popping, which isolates the raw cost of the queue
operations themselves.

| Queue                       | Latency |
|-----------------------------|--------:|
| SPSCQueue (lock free)       | 3.0 ns  |
| mutex and ring buffer       | 11.8 ns |
| mutex and std::queue        | 12.7 ns |

Two thread transfer throughput in millions of items per second, so higher is
better. Here a real producer thread and a real consumer thread run at the same
time.

| Queue                       | Throughput |
|-----------------------------|-----------:|
| SPSCQueue (lock free)       | 15.7 M/s   |
| mutex and std::queue        | 38.6 M/s   |
| mutex and ring buffer       | 37.5 M/s   |

These two tables tell opposite stories and the reason is the interesting part.
On raw operation cost the lock free queue is about four times faster than either
mutex version, because a push or a pop is a couple of atomic loads and stores with
no kernel involvement at all. That is the number that matters for the latency of
a single message on its way through the system.

The two thread throughput result is the counterintuitive one, and it is reported
as measured rather than tuned away. With both threads spinning as fast as they
can on an eight byte payload, the lock free queue actually loses to the mutex
versions. The cause is cache coherence traffic. The producer and consumer touch
the shared buffer slot and the shared index on every single element, so the cache
line holding the head and tail region ping pongs between the two cores once per
item. A mutex, counterintuitively, batches better under this extreme contention,
because the thread that holds the lock runs a burst of work while the other waits,
which keeps the hot lines on one core for longer.

The lesson is not that mutexes are better. The lesson is that the lock free win is
a latency and jitter win, not a raw throughput win, and that a production design
would recover the throughput by amortizing the coherence cost. The fix is to pass
items in batches so the index only crosses cores once per batch rather than once
per item, and to pad each slot to its own cache line. The value of building all
three and measuring them is precisely that the naive assumption gets corrected by
data, which is the whole spirit of this project.

## 4. Cache and measurement analysis

This project was developed on macOS on Apple Silicon, where the Linux `perf`
counters are not available, so the cache analysis here is by memory layout and
first principles rather than by counter readout. The reasoning still predicts the
observed behavior.

The array container and the sorted vector both lay their data out contiguously, so
a walk down the book is a sequential scan that the hardware prefetcher handles
well and that costs close to one cache line touched per several levels. The
`std::map` scatters its nodes, so the same walk is a series of dependent pointer
chases, and each hop risks a cache miss that stalls for tens of nanoseconds. This
is why the map loses the read heavy container workload by a wide margin even
though its asymptotic complexity is identical to the sorted vector for find.

The far tail seen in the demo table deserves an honest note. The median add is
around 125 nanoseconds, but the ninety nine point nine percentile and the standard
deviation are far larger. That gap is not the engine doing more work. It is the
operating system scheduler moving the process across cores, servicing interrupts,
and letting other processes run, all of which show up as multi microsecond gaps in
a timestamp that is read on every operation. On a Linux box the same binary pinned
to an isolated core with `taskset`, with frequency scaling disabled, would show a
tail a fraction of the size. The median is the honest measure of the engine on
this hardware and the tail is a property of the measurement environment.

The timing source is `std::chrono::steady_clock` read through the `now` helper.
Steady clock is monotonic, which matters because these timestamps also decide time
priority and must never move backward. Its resolution on this platform is well
under the operation latency being measured, so it is an adequate stopwatch for
everything here.

One more measurement caveat is worth stating plainly because it explains an
apparent contradiction between two numbers in this project. The demo reports an
add limit median near 125 nanoseconds, while the Google Benchmark microbenchmark
for the same operation reports closer to 600 nanoseconds. The demo number is the
truer per operation latency. The microbenchmark has to rebuild book state between
iterations, and it does that inside a PauseTiming and ResumeTiming region. Those
two calls carry a fixed overhead of several hundred nanoseconds that leaks into
the timed window, which inflates every absolute number in the add, cancel, and
modify microbenchmarks. The microbenchmarks are still valid for relative
comparison, since the overhead is the same across every case, but the demo table
is the one to read for the real cost of a single operation. This is exactly the
kind of measurement artifact that is easy to miss and worth calling out.

## 5. Lock free correctness argument

The `SPSCQueue` is correct without any lock because it obeys a strict single
producer single consumer contract and uses acquire and release ordering to publish
data across the two threads.

Only the producer ever writes `tail_` and only the consumer ever writes `head_`.
Because no index is written by two threads, there is no lost update and no need for
a compare and swap. The remaining question is visibility. When the producer writes
a slot and then advances `tail_`, the store to `tail_` uses release ordering. When
the consumer reads `tail_` with acquire ordering and sees the new value, the
acquire pairs with the release and guarantees that the slot write which happened
before the release is now visible to the consumer. The mirror image holds for the
consumer advancing `head_` to signal that a slot is free again.

If the code used relaxed ordering everywhere it would still never corrupt the
indices, but it would break visibility. A consumer could observe an advanced
`tail_` while still reading stale slot contents, because relaxed ordering places no
constraint on the order in which the two stores become visible to another thread.
Acquire and release is exactly the amount of ordering required and no more.
Sequential consistency would also be correct but is stronger than needed, and on
some architectures it inserts a full memory barrier that acquire and release avoid,
so it would cost throughput for a guarantee this structure does not use.

The head and tail indices sit on separate sixty four byte aligned cache lines. If
they shared a line, every producer store to `tail_` would invalidate the line the
consumer is reading for `head_` and the other way around, which is false sharing.
Separating them means the two threads touch different lines and do not fight over
the cache coherence protocol.

## 6. What a production system would do differently

This engine is honest about being a portfolio project rather than a venue. A real
matching system would go further in several concrete ways.

- Kernel bypass networking. Orders would arrive through a user space network stack
  such as DPDK or Solarflare ef_vi so the kernel network path never touches the
  data. The kernel stack alone can cost more than the entire match.
- Core pinning and isolation. The matching thread would run on a CPU core that is
  isolated from the scheduler with hyperthreading disabled on that core, so nothing
  else preempts it and the tail seen in section 4 largely disappears.
- Huge pages. The order arena would be backed by two megabyte or one gigabyte pages
  so the translation lookaside buffer covers the whole arena and page walks stop
  showing up in the tail.
- A binary wire protocol. Messages would use a fixed layout binary protocol in the
  spirit of ITCH or a binary FIX variant rather than anything text based, so
  parsing is a cast rather than a scan.
- A pre allocated output path. The one heap touch left on the hot path is the
  `std::vector<Trade>` returned by a match. A venue would write trades into a pre
  allocated ring owned by the caller so even that allocation disappears. This
  project returns a vector on purpose because the clarity is worth the small cost
  outside of a benchmark, and this is the one conscious tradeoff in the hot path.
- Sharding by instrument. Each symbol or group of symbols would run on its own
  matching thread with its own book and its own single producer single consumer
  feed, which scales across cores without ever needing a multi producer queue or a
  lock.

None of these change the core algorithm. They change the environment around it so
that the few nanoseconds of real work per fill are not buried under microseconds of
avoidable overhead.
