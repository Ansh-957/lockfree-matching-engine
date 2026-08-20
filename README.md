# Low-Latency Matching Engine & Market Data Pipeline

A high-performance C++20 matching engine ingesting live cryptocurrency market data via WebSocket, maintaining a cache-optimized limit order book with zero hot-path heap allocations, and executing price-time priority trades — benchmarked and profiled with `perf` and Google Benchmark.

## Architecture

```
                        ┌──────────────────────────────────┐
                        │   Coinbase WebSocket Feed        │
                        │   (Public L2/trades channel)     │
                        └──────────────┬───────────────────┘
                                       │ Raw JSON (simdjson)
                                       ▼
                 ┌─────────────────────────────────────────┐
                 │         INGESTION THREAD                │
                 │  • Boost.Beast WebSocket client (WSS)   │
                 │  • simdjson parser                      │
                 │  • Normalize → internal Order struct    │
                 │  • Timestamp at receipt                 │
                 └──────────────────┬──────────────────────┘
                                    │
                          SPSC Ring Buffer #1
                       (cache-line padded, lock-free)
                                    │
                                    ▼
                 ┌─────────────────────────────────────────┐
                 │      MATCHING ENGINE THREAD             │
                 │  • Single-writer, zero contention       │
                 │  • Flat-array price levels (O(1) lookup)│
                 │  • Intrusive linked lists per level     │
                 │  • Memory pool (zero malloc on hot path)│
                 │  • Price-time priority matching         │
                 │  • Emits: fills, cancels, book updates  │
                 └──────────────────┬──────────────────────┘
                                    │
                          SPSC Ring Buffer #2
                       (cache-line padded, lock-free)
                                    │
                                    ▼
                 ┌─────────────────────────────────────────┐
                 │       METRICS / LOGGING THREAD          │
                 │  • p50/p95/p99 latency histograms       │
                 │  • Throughput (orders/sec, matches/sec) │
                 │  • Trade log (binary append-only)       │
                 └─────────────────────────────────────────┘
```

**Why single-writer:** The matching engine thread processes orders sequentially with zero locks, zero contention, and zero heap allocations on the hot path. Concurrency is handled entirely by lock-free SPSC ring buffers at thread boundaries. This is the same architecture used by [LMAX Exchange](https://lmax-exchange.github.io/disruptor/) and most modern electronic trading venues.

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **Order book storage** | Static flat array, 10M price levels (~640MB), pre-allocated at startup | O(1) price-level lookup. No dynamic re-centering or stop-the-world pauses. Memory is cheap; CPU cycles are not. |
| **Order queue per level** | Intrusive doubly-linked list | O(1) insert, O(1) removal. `prev`/`next` pointers embedded in `Order` struct — zero separate node allocations. |
| **Memory management** | Pre-allocated pool with free-list | Zero calls to `malloc`/`new` after initialization. Placement `new` for construction. |
| **Inter-thread comms** | Lock-free SPSC ring buffer | No mutexes, no condition variables, no OS context switches. `acquire`/`release` memory ordering only. Cache-line padding to prevent false sharing. |
| **Price representation** | `int64_t` tick count ($0.01 per tick) | Avoids floating-point comparison issues. Direct array indexing — zero arithmetic on the hot path. |
| **Matching algorithm** | Price-time priority (FIFO at each price level) | Standard exchange semantics. Limit and market orders. |
| **Why not fully lock-free order book** | Single-writer pattern via SPSC buffers instead | Fully lock-free concurrent order books require hazard pointers or epoch-based memory reclamation — notoriously bug-prone. The single-writer pattern achieves the same throughput with dramatically simpler correctness guarantees. |

## Benchmark Results

Measured on an Intel Core Ultra 7 155H (see [`docs/benchmarks.md`](docs/benchmarks.md) for methodology, environment caveats, and the full tables):

| Metric | Result |
|--------|--------|
| Match latency, live Coinbase BTC-USD | **p50 28ns · p99 567ns** |
| Match latency, synthetic saturation (38.5M msgs) | p50 66ns · p99 759ns |
| Sustained pipeline throughput (3 threads, full telemetry) | **~3.85M msgs/s** |
| Flat-array book vs `std::map` baseline, identical 1M-msg stream | **1.74× faster** |
| Memory pool vs `new`/`delete` allocation cycle | **21× faster** |
| SPSC ring, cross-thread (48-byte messages) | 480M msgs/s · ~97ns one-way |
| Order book add / match / cancel | 14.7ns / 8.2ns / 43.2ns |
| Matching hot loop, perf-verified | IPC 1.46 · 0.87% L1d miss · 1.55% branch miss |
| Hot-path heap allocations | **0** |

### Future Optimizations
- Replace `std::unordered_map<OrderId, Order*>` with a dense slot array using generation counters to eliminate hash-map cache misses on cancel lookups (cancel at 43ns is ~3× add, and the lookup is why)
- Sampled telemetry (e.g. 1-in-16) to reclaim most of the ~35% observer cost of timing every message
- Sub-1% L1d miss rate already achieved (perf-verified); next win is the cancel-path hash lookup above

## Building

### Prerequisites
- **Linux** (bare-metal, not WSL2 — required for `perf` hardware counters)
- CMake ≥ 3.20
- GCC ≥ 12 or Clang ≥ 15 (C++20 support)
- [vcpkg](https://github.com/microsoft/vcpkg) for dependency management

### Build
```bash
# Clone vcpkg if you haven't
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build -j$(nproc)
```

### Run Tests
```bash
cd build && ctest --output-on-failure
```

### Run Benchmarks
```bash
./build/order_book_bench --benchmark_format=console
./build/ring_buffer_bench --benchmark_format=console
./build/matching_bench --benchmark_format=console
./build/memory_pool_bench --benchmark_format=console
```

### Run the Pipeline
```bash
# synthetic load test (no network), 10 seconds, fills logged to trades.bin
./build/matching-engine --synthetic --duration 10

# live Coinbase BTC-USD until Ctrl+C; prints latency histograms on exit
./build/matching-engine --live

# options: --product ETH-USD | --duration N | --log FILE | --no-log
```

## Project Structure

```
matching-engine/
├── CMakeLists.txt              # Build configuration
├── vcpkg.json                  # Dependency manifest
├── src/
│   ├── core/                   # Order book, matching engine, memory pool
│   │   ├── types.h
│   │   ├── order.h
│   │   ├── price_level.h
│   │   ├── order_book.h/.cpp
│   │   ├── matching_engine.h/.cpp
│   │   └── memory_pool.h
│   ├── transport/              # Lock-free inter-thread communication
│   │   ├── spsc_queue.h
│   │   └── message.h
│   ├── feed/                   # Live market data ingestion
│   │   ├── ws_client.h/.cpp
│   │   ├── coinbase_feed.h/.cpp
│   │   └── feed_handler.h/.cpp
│   ├── metrics/                # Latency tracking and trade logging
│   │   ├── latency_tracker.h
│   │   ├── metrics_collector.h/.cpp
│   │   └── trade_logger.h/.cpp
│   └── main.cpp
├── bench/                      # Google Benchmark micro-benchmarks
├── test/                       # GoogleTest unit tests
├── tools/                      # Synthetic data generator, replay tool
├── docs/                       # Architecture docs, benchmark results
└── deploy/                     # Docker, AWS CloudFormation (Tier 2)
```

## Tech Stack

| Component | Technology |
|-----------|-----------|
| Language | C++20 |
| Build | CMake + vcpkg |
| WebSocket | Boost.Beast + Boost.Asio |
| JSON parsing | simdjson |
| Logging | spdlog |
| Testing | GoogleTest |
| Benchmarking | Google Benchmark |
| Profiling | `perf` (Linux hardware counters) |

## License

MIT
