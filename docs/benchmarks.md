# Benchmark Results

> 🚧 This document will be populated with real benchmark data during Phase 4 (Weeks 7–8).

## Methodology

All benchmarks are run on bare-metal Linux to ensure accurate hardware counter access via `perf`.

### Hardware
- **CPU:** [Your CPU model]
- **RAM:** [Your RAM specs]
- **OS:** [Linux distro and kernel version]
- **Compiler:** GCC [version] with `-O3 -march=native`

### Tools
- **Micro-benchmarks:** [Google Benchmark](https://github.com/google/benchmark) v1.8+
- **Profiling:** `perf stat`, `perf record`, `perf report`
- **Memory:** Valgrind (leak checking), custom allocation counter

## Results

### Order Book Operations

| Operation | p50 | p95 | p99 | p99.9 |
|-----------|-----|-----|-----|-------|
| Add order | TBD | TBD | TBD | TBD |
| Cancel order | TBD | TBD | TBD | TBD |
| Match (1:1 cross) | TBD | TBD | TBD | TBD |

### SPSC Ring Buffer

| Metric | Value |
|--------|-------|
| Single-thread push+pop | TBD |
| Two-thread throughput (msg/sec) | TBD |
| Two-thread latency (p99) | TBD |

### Memory Pool vs malloc

| Allocator | Allocate (ns) | Deallocate (ns) | Cycle (ns) |
|-----------|--------------|-----------------|------------|
| MemoryPool | TBD | TBD | TBD |
| malloc/free | TBD | TBD | TBD |
| **Speedup** | TBD | TBD | TBD |

### End-to-End Pipeline

| Metric | Value |
|--------|-------|
| E2E latency (order → fill) p50 | TBD |
| E2E latency (order → fill) p99 | TBD |
| Sustained throughput (orders/sec) | TBD |
| Hot-path heap allocations | 0 |

### perf stat Output

```
TBD — will paste perf stat output showing:
- Instructions per cycle (IPC)
- L1 data cache miss rate
- Branch misprediction rate
- Context switches
```

## Optimization Log

This section documents specific optimizations and their measured impact.

### Baseline → Flat Array (Week 1–2)
**Change:** Replace `std::map<Price, PriceLevel>` with pre-allocated flat array indexed by price tick.
**Expected impact:** Eliminate O(log N) tree traversal, improve cache locality.
**Measured:** TBD

### Baseline → Memory Pool (Week 1–2)
**Change:** Replace `new`/`delete` with pre-allocated memory pool.
**Expected impact:** Eliminate heap allocation overhead and fragmentation on hot path.
**Measured:** TBD

### Baseline → Intrusive Lists (Week 1–2)
**Change:** Replace `std::list<Order>` with intrusive doubly-linked list embedded in Order struct.
**Expected impact:** Eliminate separate node allocation, improve cache locality.
**Measured:** TBD
