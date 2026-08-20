# Benchmark Results

Baseline recorded **2026-08-19** (Commit 12). Numbers below are real,
measured on the hardware described here, with the caveats stated. When a
number has a known distortion (governor, observer effect), the distortion
is documented next to it rather than hidden.

## Methodology

### Hardware

- **CPU:** Intel Core Ultra 7 155H (Meteor Lake, hybrid): 6 P-cores with
  HT (max 4.5 GHz per sysfs) + 8 E-cores (3.8 GHz) + 2 LP E-cores
  (2.5 GHz), 22 logical CPUs. L1d 48 KiB/core, L2 2 MiB/P-core, L3 24 MiB shared.
- **RAM:** 30 GiB
- **OS:** Ubuntu 22.04, kernel 6.8.0-134-generic
- **Compiler:** GCC 11.4, `-O3 -march=native`, CMake Release

### Pinning on a hybrid CPU

Thread pins were chosen from `/sys/devices/system/cpu/*/topology`, not
assumed: on this part **cpu1 and cpu2 are hyperthread siblings of the same
physical P-core** (cpu0's sibling is cpu5). The pipeline pins ingest → cpu0,
engine → cpu1, output → cpu3 — three distinct physical P-cores. An earlier
revision pinned output to cpu2 and had the metrics thread stealing execution
ports from the engine's own core.

### Environment caveats (read before quoting numbers)

- **Run-to-run variance on this host is ±10–15%**, measured directly: the
  full suite was run three times (cold machine + `powersave` governor,
  warm + `performance` governor, warm + `performance` + `taskset` pinning)
  and wall-clock numbers *degraded* run over run — e.g. AddOrder 14.7 →
  16.9 → 18.7ns — because sustained benchmarking heats a laptop package
  (observed 3.9–4.3GHz of a 4.5GHz max at 73°C). The cold-machine run is
  what the tables record; treat the last digit of every ns figure as noise.
- Counter-intuitively, the `performance` governor did **not** speed things
  up here: holding all 22 CPUs at high frequency spends the shared package
  power budget that `powersave` was leaving available for the one busy
  P-core's turbo.
- **The perf ratios (IPC, miss rates) are frequency-independent** and
  reproduced consistently across governor states — they are the most
  portable numbers in this document.
- Laptop-class CPU, desktop environment running. This is an honest
  *baseline*, not a tuned-host record.

### Tools

- **Micro-benchmarks:** Google Benchmark v1.9.0 (`--benchmark_min_time` ≥ 1s)
- **Latency percentiles:** in-process HDR histogram (Commit 11), fed by
  RDTSC timestamps over an SPSC telemetry ring — see `docs/explanations.md`
- **Profiling:** `perf stat` 6.8.12 (hybrid-aware: `cpu_core`/`cpu_atom`
  counters reported separately; P-core figures quoted)

---

## Results

### Order book operations (micro, hot cache)

From `order_book_bench` (mean ns/op over ≥1s of iterations):

| Operation | ns/op | ops/sec | Notes |
|-----------|------:|--------:|-------|
| Add order (rest on book) | 14.7 | 68.2M | array index + intrusive push_back + pool alloc |
| Cancel order | 43.2 | 23.1M | dominated by the `unordered_map` id lookup |
| Match (1:1 cross) | 8.2 | 121.3M | best-level walk + fill + pool recycle |

Cancel being ~3× add is expected and documented: the id→pointer hash lookup
is the one remaining non-array data structure on the hot path (see Future
Optimizations in the README).

### Match latency distribution (full engine, telemetry from Commit 11)

| | p50 | p95 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|
| Live BTC-USD (25s, 54,575 msgs) | 28ns | 165ns | 567ns | 1.4µs | 5.1µs |
| Synthetic saturation (38.5M msgs) | 66ns | 567ns | 759ns | 1.3µs | ~109ms* |

\* synthetic max is a scheduler artifact: without `SCHED_FIFO` (needs root)
the engine thread occasionally gets preempted mid-message. The p99.9 shows
the engine itself; the max shows the OS.

### SPSC ring buffer

From `ring_buffer_bench`:

| Metric | Value |
|--------|------:|
| Single-thread push+pop (uint64) | 1.17 ns/op |
| Two-thread throughput, uint64, 1M batch | 1.10 G items/s |
| Two-thread throughput, 48-byte EngineMessage, 1M batch | 480 M items/s (21.5 GiB/s) |
| Two-thread round trip (2 queues) | 193 ns ⇒ ~97 ns one-way |

The one-way latency is dominated by cache-line ownership transfer between
cores (~2 coherence hops), which is the physical floor for cross-core
message passing.

### Memory pool vs new/delete

Windowed cycle (256 live allocations, free oldest + allocate, from
`memory_pool_bench` — see the file header for why the naive version of this
benchmark was invalid):

| Allocator | Cycle (ns) | Speedup |
|-----------|-----------:|--------:|
| MemoryPool free-list | 0.44 | **21×** |
| new/delete | 9.32 | 1× |

0.44ns (~2 cycles) is real, not optimizer-deleted: a hot freelist pop+push
is two L1 pointer swaps that the out-of-order core overlaps across
iterations. Verified by growing the window to 1 MiB (L2-resident): the pool
number rose to 1.03ns — deleted code would not respond to cache pressure.
In engine context (add+cancel including book ops): 19.2ns pool vs 27.6ns
malloc per cycle.

### Flat array vs std::map order book (the headline)

From `matching_bench`: both engines process the **identical** deterministic
1M-message stream (35% cancels, market orders, natural crossings), and
produced the identical 584,908 fills — validating that the baseline's
matching semantics match the real engine exactly.

| Engine | Time (1M msgs) | Throughput | Speedup |
|--------|---------------:|-----------:|--------:|
| Flat array + pool + intrusive lists | 37.9 ms | 27.7 M msg/s | **1.74×** |
| `std::map` + `std::list` + new/delete | 66.1 ms | 15.9 M msg/s | 1× |

1.74× is the honest end-to-end number — the workload includes matching,
hash-map cancels, and fills whose cost both engines share. On the pure
book operations the structural gap is far larger (array index at 14.7ns vs
tree descent + node allocation), but end-to-end is what you actually get.

### End-to-end pipeline (3 threads, 3 SPSC rings)

| Metric | Value |
|--------|------:|
| Sustained throughput, full telemetry ON | ~3.85 M msg/s |
| Sustained throughput, telemetry OFF (Commit 10) | ~5.9 M msg/s |
| Observer cost (2× RDTSC + 2 ring pushes/msg) | ~35% |
| Fill queue / telemetry ring drops | 0 |
| Hot-path heap allocations | 0 |
| Order latency (E2E) at saturation | p50 ≈ 18.6 ms |

The 18.6ms E2E latency at saturation is **Little's law, not engine
slowness**: the synthetic producer deliberately keeps the 64K-slot input
ring full (backpressure mode), so every message waits 65,536 ÷ 3.6M/s ≈
18ms in queue. In the live run (arrival rate ≪ service rate) E2E p50 was
~0.7ms, set entirely by Coinbase's 50ms `level2_batch` coalescing.

### perf stat — hardware counters

Unlocked with `sudo sysctl -w kernel.perf_event_paranoid=1`. All figures
below are P-core (`cpu_core`) counters; reproduce with:

```bash
perf stat -e cycles,instructions,branches,branch-misses,\
L1-dcache-loads,L1-dcache-load-misses,context-switches \
  ./build/matching_bench --benchmark_filter=BM_Stream_FlatBook
```

**The matching hot loop, flat array vs std::map** (same 1M-message stream,
no spin-waits — this is the engine's pure compute efficiency):

| Counter | Flat array | std::map | Interpretation |
|---------|-----------:|---------:|----------------|
| IPC (insn/cycle) | **1.46** | 1.36 | flat book keeps the pipeline fed |
| L1d miss rate | **0.87%** | 2.66% | 3×: array indexing vs tree pointer-chasing |
| Branch miss rate | **1.55%** | 4.59% | 3×: predictable loops vs tree comparisons |

This is the flat-array thesis in hardware counters: the map's red-black
tree turns every price lookup into dependent pointer dereferences (cache
misses) and data-dependent comparisons (mispredicts). The flat book's
address arithmetic gives the prefetcher and branch predictor regular
patterns to lock onto.

**Whole pipeline process** (`matching-engine --synthetic --duration 15`,
3 threads including busy-spin waits):

| Counter | Value | Note |
|---------|------:|------|
| IPC | 0.40 | *dominated by PAUSE spin loops* — not engine efficiency |
| L1d miss rate | 2.49% | includes cross-core SPSC cache-line transfers |
| Branch miss rate | 3.76% | |
| Context switches | 14.4K over 15.3s | ~1K/s, mostly the sleeping output thread |
| CPU migrations | **30** | pinning works: threads stay put |

The whole-process IPC of 0.40 is expected and *not* a quality signal: two
threads spend their idle moments executing PAUSE loops (cheap
instructions, few per cycle by design). The hot-loop table above is the
number that reflects engine quality; the process table is recorded to show
what a spin-wait architecture looks like from the outside.

---

## Optimization Log

Measured impact of each major design decision, quantified against a
baseline implementation of the same semantics.

### std::map book → flat array (+ pool + intrusive lists)
**Change:** `std::map<Price, std::list<Order>>` replaced by a pre-allocated
flat array indexed by price tick, with intrusive per-level lists and a
free-list pool.
**Measured:** **1.74× end-to-end** on an identical 1M-message stream
(27.7M vs 15.9M msg/s), identical fills. Micro level: add = 14.7ns where
the map pays a tree descent plus a node allocation (~9.3ns alone).

### new/delete → memory pool
**Measured:** **21×** on the allocation cycle itself (0.44ns vs 9.32ns
windowed); 19.2ns vs 27.6ns when embedded in the add+cancel path. Also
removes allocator lock contention and fragmentation as tail-latency
sources, which the mean does not capture.

### Intrusive lists (vs node-based std::list)
Not separable from the flat-array measurement above (they share the
baseline), but structurally: zero node allocations on add, and cancel
unlinks in O(1) via the order's own prev/next pointers — no level scan.

### Telemetry ring (Commit 11) — the cost of observation
**Measured:** timing every message costs ~35% throughput at saturation
(5.9M → 3.85M msg/s). Accepted deliberately: match-latency percentiles are
the product here. A production system would sample (e.g. 1-in-16) to
reclaim most of it.

### Hybrid-topology pinning fix (Commit 12)
**Change:** output thread moved off the engine's hyperthread sibling
(cpu2 → cpu3).
**Measured:** ~3.65M → ~3.85M msg/s sustained (+5%), and removes a
structural noise source from every future measurement.
