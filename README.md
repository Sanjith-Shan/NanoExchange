# NanoExchange

High performance limit order book and matching engine in modern C++20.

NanoExchange is a single instrument and multi instrument matching engine built
for nanosecond scale latency. It uses a custom memory pool allocator for zero
heap allocation in the hot path, intrusive linked lists for O(1) order
management, and a lock free single producer single consumer ring buffer for
concurrent order ingestion. It supports limit, market, immediate or cancel, and
fill or kill orders with strict price time priority matching.

Prices are integers in ticks. There is no floating point anywhere in the hot
path, which is how real venues avoid rounding surprises and keep comparisons
exact and fast.

## Why this project

An order book is the core data structure of every electronic market, and a
matching engine is one of the most latency sensitive pieces of software there is.
This project builds one from scratch and then does the part most order book demos
skip. It implements the real alternatives for each key data structure, benchmarks
them head to head, and lets the numbers pick the winner. The reasoning behind
every choice is written up in [docs/DESIGN.md](docs/DESIGN.md).

## Performance

Measured with the bundled demo driving one million messages through a single
book on an Apple Silicon laptop, built with `-O3` and native tuning. The process
is not pinned to an isolated core, so the median is the honest measure of engine
work and the far tail is dominated by operating system scheduling noise. The
design writeup explains this in detail.

| Operation    | Median | p99   | Notes                                  |
|--------------|--------|-------|----------------------------------------|
| Add limit    | ~125ns | ~625ns| Add a resting limit order              |
| Add market   | ~167ns | ~875ns| Sweep the opposite side                |
| Cancel       | ~125ns | ~350ns| Remove a resting order by id           |
| Modify       | ~42ns  | ~667ns| Cancel and re add at new terms         |
| Best bid ask | <50ns  | -     | Top of book query                      |
| Snapshot(5)  | ~700ns | -     | Build a five level depth snapshot      |

End to end throughput is in the range of a few million messages per second on
one thread, higher on a shallow book and lower as the book deepens and matching
walks more levels.

Run the demo yourself to reproduce the table.

```
./build/nano_exchange --orders 1000000 --symbol AAPL
```

### Head to head shootouts

The project does not just pick one data structure. It implements the real
contenders behind a common C++20 concept and benchmarks them. Full tables and
charts are in [docs/DESIGN.md](docs/DESIGN.md) and `results/`.

- Price level container. `std::map` red black tree versus a sorted contiguous
  vector versus a direct addressed array. See which wins at shallow and deep
  books and why cache locality decides it.
- Memory allocator. The custom pool versus `new` and `delete` versus a `std::pmr`
  pool resource versus a pre faulted `mmap` region. The pool wins the tail
  because it never calls into the kernel.
- Queue. The lock free `SPSCQueue` versus a mutex around `std::queue` versus a
  mutex around a ring buffer. The lock free version is about four times faster on
  single message round trip latency. The two thread throughput result is
  counterintuitive and is reported honestly with the cache coherence explanation
  in the design writeup.

## Architecture

```
   order entry (thread A)                 matching (thread B)
  ------------------------              ------------------------
   parse and intern symbol
            |
            v
     +---------------+   lock free    +------------------+
     |  SPSC ring    | =============> |  MatchingEngine  |
     |  buffer       |   no locks     |  route by symbol |
     +---------------+                +--------+---------+
                                               |
                                               v
                                      +------------------+
                                      |    OrderBook     |
                                      |  bids_   asks_   |   std::map by price
                                      |  order_map_      |   O(1) id lookup
                                      +--------+---------+
                                               |
                                               v
                                      +------------------+
                                      |   PriceLevel     |   intrusive FIFO list
                                      |  of Order*       |   of pooled orders
                                      +--------+---------+
                                               |
                                               v
                                          trades out
```

Three design decisions carry most of the performance.

- A memory pool pre allocates every order object once, so the matching path pops
  a pointer off a free list instead of calling the heap allocator. This removes
  the biggest cause of tail latency, which is an allocator slow path into the
  kernel.
- Intrusive linked lists thread each price level directly through the order
  objects, so adding or removing an order at a level is a few pointer writes with
  no node allocation and no search.
- A lock free single producer single consumer queue moves orders between threads
  with plain atomic loads and stores under acquire and release ordering, so no
  thread ever blocks and the jitter stays low.

## Order types

| Type              | Behavior                                                       |
|-------------------|----------------------------------------------------------------|
| Limit             | Match what crosses the limit price, rest the remainder         |
| Market            | Match at any price until filled or the book is empty           |
| Immediate or cancel | Match what is available now, cancel the remainder            |
| Fill or kill      | Fill the whole order at once or reject it entirely             |

Cancel removes a resting order by id. Modify cancels the old order and re adds it
at the new price and size, which loses time priority. That is the correct and
standard exchange behavior for a reprice.

## Quick start

Requires CMake 3.20 or newer and a C++20 compiler. GoogleTest and Google
Benchmark are fetched automatically at configure time.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run the market simulation and print the latency table
./build/nano_exchange --orders 1000000

# Run the test suite
ctest --test-dir build --output-on-failure

# Run the benchmarks
./build/bin/benchmarks

# Regenerate the charts from the benchmark JSON
./build/bin/benchmarks --benchmark_format=json --benchmark_out=results/benchmarks.json
python3 scripts/plot_latency.py --input results/benchmarks.json --outdir results
```

To build just the engine without the fetched dependencies, configure with
`-DNANO_BUILD_TESTS=OFF -DNANO_BUILD_BENCH=OFF`.

## Benchmark methodology

The benchmarks use Google Benchmark. Each case runs many iterations with warm up,
reports mean, median, and standard deviation across repetitions, and where the
work is a stream of operations it reports items per second. For the most stable
latency numbers, run on a quiet machine, and on Linux pin the process to an
isolated core with `taskset` and disable frequency scaling. On macOS the same
pinning is not available, so the median is the number to trust and the tail
reflects the environment rather than the engine. The timing source is the
monotonic `std::chrono::steady_clock`, chosen because these timestamps also decide
time priority and must never move backward.

## Design decisions

The full performance engineering writeup is in [docs/DESIGN.md](docs/DESIGN.md).
It covers the hot path instruction by instruction, the three data structure
shootouts with data, a cache behavior analysis, a correctness argument for the
lock free queue and its memory ordering, and an honest account of what a
production venue would do differently.

## Project layout

```
include/nano/     header only core engine
src/              order_book and matching_engine implementations plus the demo
test/             GoogleTest suite for matching, pool, queue, price level
bench/            Google Benchmark microbenchmarks and the shootouts
scripts/          order flow generator and chart plotter
data/             notes on obtaining and generating replay data
docs/             design and performance writeup
results/          benchmark JSON and generated charts
```

## Tests

The suite covers matching correctness end to end. That includes price time
priority, price improvement at the maker price, partial and multi level fills,
every order type, cancel and modify, and the market data snapshot. It also covers
the memory pool including exhaustion and reuse, and the lock free queue under a
concurrent producer and consumer with wrap around.

## License

MIT. See [LICENSE](LICENSE).
