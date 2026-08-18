# Engineering Explanations — Commit by Commit

This document explains every major decision, algorithm, and piece of C++ syntax
used in the project, in the order it was built. Each section corresponds to one
git commit. Read it top to bottom and you can reconstruct the reasoning behind
the whole engine.

---

## Commit 1 — Core Data Definitions (`types.h`, `order.h`)

### Why prices are integers ("ticks"), not doubles

`Price` is an `int64_t` where 1 tick = $0.01, so $150.25 is stored as `15025`.

- **Floating point cannot represent most decimal fractions exactly.** `0.1 + 0.2
  != 0.3` in a double. In a matching engine, "is bid >= ask?" must be an exact
  comparison — a one-ULP rounding error could match orders that shouldn't match.
- **Integer comparison is a single CPU instruction** with no rounding modes, no
  denormals, no platform differences. Deterministic across machines.
- **Ticks double as array indices.** The order book is a flat array where
  `levels_[price_in_ticks]` is the queue at that price. Storing prices as tick
  counts means price → array index is *free* (no conversion on the hot path).

Conversion to/from dollars (`dollars_to_ticks`, `ticks_to_dollars`) only happens
at the system boundary (parsing the feed, printing logs) — never inside matching.

### Why these specific type widths

| Alias | Type | Reason |
|---|---|---|
| `OrderId` | `uint64_t` | Monotonically increasing; 64 bits never wraps in practice |
| `Price` | `int64_t` | Signed so price *differences* (spreads, deltas) are representable |
| `Quantity` | `uint32_t` | 4.2 billion units is plenty; half the memory of a u64 keeps `Order` small |
| `Timestamp` | `uint64_t` | Nanoseconds since epoch; covers ~584 years |

Keeping `Order` small matters: smaller structs → more orders per cache line →
fewer cache misses when walking the book.

### Syntax notes (used throughout the codebase)

- **`enum class Side : uint8_t`** — a *scoped* enum. Unlike a plain `enum`, its
  values don't leak into the surrounding namespace and don't implicitly convert
  to `int` (so you can't accidentally write `if (side == 1)`). The `: uint8_t`
  makes it 1 byte instead of 4, again keeping `Order` compact.
- **`inline constexpr`** — a compile-time constant that can live in a header
  without violating the One Definition Rule. The compiler folds it into the
  code; it occupies no runtime storage.
- **`[[nodiscard]]`** — the compiler warns if you call the function and ignore
  its return value. On `allocate()` this catches leaks at compile time.
- **`noexcept`** — a promise the function never throws. Lets the compiler skip
  generating exception-handling scaffolding, and documents hot-path functions.

### The intrusive linked list idea (`order.h`)

`Order` embeds `Order* prev` and `Order* next` directly in the struct. When an
order joins the FIFO queue at its price level, we just wire up these pointers.

Compare with `std::list<Order>`:

| | `std::list` | intrusive list |
|---|---|---|
| Insert | heap-allocates a *node* wrapping the Order | zero allocation — the order **is** the node |
| Remove by pointer | need the iterator, or O(n) search | O(1): `prev->next = next; next->prev = prev` |
| Cache behavior | order data and node pointers in separate allocations | one contiguous object |

O(1) removal by pointer is the killer feature: cancels look up `OrderId → Order*`
in a hash map, then unlink in constant time without ever walking a list.

The struct also tracks `quantity` (original) and `filled_quantity` (cumulative)
separately rather than decrementing one number — this preserves the original
size for logging/auditing and makes `remaining_quantity()` a trivial subtraction.

---

## Commit 2 — Fixed-Block Memory Pool (`memory_pool.h`)

### The problem: `malloc`/`new` on the hot path

Every incoming order needs an `Order` object; every fill or cancel releases one.
General-purpose allocators are wrong for this:

1. **Unpredictable latency.** `malloc` sometimes returns in 20ns and sometimes
   takes a page fault or a lock and returns in 10µs. p99 latency is destroyed
   by the tail cases.
2. **Locks.** Heap allocators are thread-safe, which means synchronization we
   don't need (only the engine thread allocates orders).
3. **Fragmentation.** Millions of same-size alloc/free cycles churn the heap.
4. **Cache scatter.** Consecutive `new` calls can return addresses anywhere in
   memory; walking a price level would then jump all over RAM.

### The solution: a free-list pool

`MemoryPool<T, N>` grabs one block of `sizeof(T) * N` bytes at startup and never
touches the heap again. Free slots are tracked with a **free list**: a singly
linked list threaded *through the unused slots themselves*.

```
storage:  [slot0][slot1][slot2][slot3]...
                    │       │
free_head_ ──► slot1 ──► slot3 ──► nullptr     (slots 0 and 2 are in use)
```

The trick: an *unused* slot is just spare memory, so we store the "next free
slot" pointer inside it (the `FreeNode` struct is literally one pointer). The
bookkeeping costs zero extra memory.

- `allocate()` = pop the head of the list: 2 pointer reads, 1 write. **O(1).**
- `deallocate()` = push onto the head: 2 writes. **O(1).**
- No size classes, no fragmentation, no locks, no syscalls. Ever.

The free list is LIFO (last freed = first reused). That's deliberate: the most
recently freed slot is the most likely to still be in the CPU cache.

### Decision: return `nullptr` on exhaustion, don't throw

The scaffold's reference version threw `std::bad_alloc`. We chose `nullptr`:

- Exceptions on the hot path are poison — the *possibility* of a throw forces
  the compiler to keep unwind tables and can inhibit optimization; an actual
  throw costs microseconds.
- Pool exhaustion is an *expected*, handleable condition (shed load / reject
  the order), not a programming error.
- `allocate()` can then be `noexcept`, and `[[nodiscard]]` forces the caller
  to look at the result, so a null can't slip through unchecked.

### Decision: caller constructs (placement new), pool only manages memory

`allocate()` returns **raw uninitialized bytes**; the caller does:

```cpp
Order* o = pool.allocate();       // raw memory
new (o) Order{...};               // placement new: construct AT this address
...
o->~Order();                      // explicit destructor call
pool.deallocate(o);               // return the raw slot
```

**Placement new** (`new (address) T{...}`) runs the constructor at a memory
address you supply, instead of allocating. Its inverse is the explicit
destructor call `o->~Order()`. Splitting memory management from object lifetime
keeps the pool dumb and fast, and lets the matching engine control exactly when
construction happens.

### Decision: heap-backed storage, not an inline array

Your WIP version had `std::byte storage_[sizeof(T) * PoolSize];` as a member.
That means the buffer lives *inside* the pool object — so a
`MemoryPool<Order, 1'000'000>` declared as a local variable would put ~64MB on
the stack (typical stack limit: 1–8MB) and crash instantly.

The final version holds `std::unique_ptr<std::byte[]> storage_` — one `new[]`
in the constructor, freed automatically in the destructor. This is still
"zero-malloc": the guarantee is zero allocations *after initialization*, and
construction is initialization.

### Syntax used

- **`std::byte`** — a type that means "raw memory, not a number, not a
  character". We use a byte buffer instead of `T storage_[N]` because a `T`
  array would *default-construct N objects* at startup; we want raw memory
  that objects are placement-newed into later.
- **`reinterpret_cast<FreeNode*>(ptr)`** — "treat these bytes as this type."
  Normally dangerous, but exactly right here: an unused slot has no live object
  in it, so we may temporarily use its bytes as a `FreeNode`.
- **`static_assert(cond, "msg")`** — a compile-time check. Two guard rails:
  1. `sizeof(T) >= sizeof(void*)` — the free-list pointer must fit in a slot.
  2. `alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__` — plain `new[]` only
     guarantees 16-byte alignment; a hypothetical `alignas(64)` T would need an
     aligned allocator, and this assert makes that failure loud at compile time
     instead of silent misalignment at runtime.
- **`= delete` on copy/move** — the pool hands out pointers into its own
  buffer. If the pool could be copied or moved, those pointers would dangle.
  Deleting the special members makes misuse a compile error.
- **Alignment**, briefly: every type has an alignment requirement (`alignof`),
  e.g. an 8-byte pointer must live at an address divisible by 8. Misaligned
  access is slow on x86 and crashes on some architectures. Slot addresses are
  `base + i * sizeof(T)`, and since `sizeof(T)` is always a multiple of
  `alignof(T)`, every slot is correctly aligned if the base is.

### Why the free list is built in reverse

The constructor loops `i = N..1` pushing slots onto the list head, so slot 0
ends up at the head. The first burst of allocations then walks the buffer
front-to-back in address order — the hardware prefetcher recognizes the
sequential pattern and pulls cache lines in ahead of use.

### Build system changes made alongside this commit

`CMakeLists.txt` was restructured with options (`BUILD_FEED`, `BUILD_BENCHMARKS`,
`BUILD_TOOLS`, all OFF for now):

- The old file unconditionally required Boost, simdjson, spdlog, and Google
  Benchmark, and linked bare `ssl`/`crypto` libs — none of which are needed
  until Phase 2/3, and the bare lib names don't exist on Windows/MSVC. Until
  Phase 3, the only dependency is GTest.
- Test sources are added to the build commit-by-commit (commented lines in the
  `unit_tests` target), so the build always matches what is actually finished.
- `-O3 -march=native` are GCC/Clang flags; they're now guarded so MSVC Release
  builds (which use `/O2` automatically) don't break.

`memory_pool_reference.h` (the scaffold's stashed implementation) was deleted —
it had served its purpose as a study reference and is recoverable from git
history (`git show 68bcba4:src/core/memory_pool_reference.h`).

---

## Commit 3 — Intrusive Doubly-Linked List (`price_level.h`)

### What a PriceLevel is

One `PriceLevel` = the FIFO queue of all resting orders at one exact price.
The order book is (conceptually) 10 million of these, one per tick. "Price-time
priority" falls out of the structure naturally:

- **Price priority** is handled by *which* PriceLevel an order sits in.
- **Time priority** is handled by FIFO order *within* the level: new orders
  `push_back`, the matcher always consumes `front()` (the oldest).

### Why doubly-linked (not singly-linked)

A singly-linked list can push_back and pop_front in O(1), which covers adding
orders and matching. But it *cannot* remove an arbitrary middle element in O(1)
— you'd need the predecessor, which means walking the list. Cancels hit random
positions in the queue, and real crypto feeds are cancel-heavy (often >90% of
messages). The `prev` pointer costs 8 bytes per order and buys O(1) cancel:

```
remove(order):                        before:  A ⇄ B ⇄ C
    order->prev->next = order->next;  unlink B: A ⇄ C
    order->next->prev = order->prev;  (plus nullptr edge cases at head/tail)
```

Four pointer writes, no traversal, no matter where the order is in the queue.

### How the operations work

- **`push_back`** — new order becomes the tail. If the list was empty it also
  becomes the head. Two branches, a handful of writes.
- **`remove`** — the unlink above. If `prev` is null the order was the head, so
  the head moves; if `next` is null it was the tail. After unlinking, the
  order's own pointers are reset to nullptr so a stale `prev`/`next` can never
  be followed later (this is the "no memory faults" property the commit plan
  asks for).
- **`pop_front`** — special-cased head removal used on the matching path.
  Slightly cheaper than `remove(front())` because it skips the prev-side
  branches, and it returns the popped order so the matcher can fill it.
- **`reduce_quantity`** — see below.

### Why `total_quantity` is maintained incrementally

The matcher and (later) the dashboard constantly ask "how much size is resting
at this price?". Recomputing that would be a full list walk — O(n) pointer
chasing over potentially thousands of orders, each a likely cache miss.
Instead every mutation keeps a running total: `push_back` adds the order's
remaining quantity, `remove`/`pop_front` subtract it, and `reduce_quantity(qty)`
subtracts a partial fill. Reading it is then a single load, O(1).

The subtle ordering rule: on a partial fill, the matcher updates the *order's*
`filled_quantity` and must call `reduce_quantity` with the filled amount. On
remove/pop the level subtracts `remaining_quantity()` — which is why the
order's fill state must be updated *before* it is removed, or the aggregate
would drift.

### Why PriceLevel is non-copyable and non-movable

The levels live in a fixed flat array inside `OrderBook`. If one were copied,
two levels would point at the same order chain; if moved, orders' `prev/next`
would still point into the old location. `= delete` on all four special members
turns both mistakes into compile errors.

### Why everything is in the header

Every method is a few pointer operations. Defining them in the header lets the
compiler inline them into the matching loop — a function call would cost more
than the operation itself. There is no `price_level.cpp`.

### Testing note

Per the commit plan, PriceLevel has no dedicated test file — it is exercised
indirectly (and thoroughly) by `order_book_test.cpp` in Commit 4, since every
order book operation drives the list through push/remove/pop cycles.

### Housekeeping in this commit

Comment style cleanup across all files built so far: removed Doxygen `///`
blocks, `@file`/`@brief`/`@param` tags, dash-row section banners, and trailing
periods on comments. Comments now only say things the code can't.

---

## Commit 4 — Flat-Array Order Book (`order_book.h/.cpp`)

### The core idea: price IS the array index

The textbook order book is a `std::map<Price, Level>` (a red-black tree):
O(log n) lookup, and every step of the tree walk is a pointer dereference to
an unpredictable address — a likely cache miss. Our book is one flat array:

```cpp
levels_[price_in_ticks]   // THE lookup. One multiply-free index. O(1).
```

Since prices are already stored as tick counts, there is zero arithmetic on
the hot path. The cost is memory: 10M levels × 24 bytes ≈ 240MB allocated once
at startup. That trade — memory for deterministic O(1) access with no pointer
chasing — is the central design decision of the whole engine. The alternative
(a dynamic band of levels around the current price that re-centers when price
moves out of range) was rejected because re-centering is an O(N) stop-the-world
pause at exactly the worst moment: a fast market move.

### One array shared by both sides — the "uncrossed book" invariant

Bids and asks live in the same `levels_` array; a level doesn't know which
side it belongs to. This works because a healthy book is **uncrossed**: every
bid price < every ask price. The matching engine (Commit 6) guarantees this —
an incoming order that would cross is *matched* against the opposite side
until it no longer crosses, and only then rests. So any single level only ever
contains one side's orders, and scanning down from the best bid can never run
into an ask level. The invariant is documented in the header because the
standalone `OrderBook` doesn't enforce it — its correctness depends on how the
engine drives it.

### Best bid/ask tracking: cheap adds, lazy repair on cancel

- **Add**: one comparison. New bid above `best_bid_`? Update it. Done.
- **Cancel that empties the best level**: scan inward (down for bids, up for
  asks) until the next non-empty level. This is O(gap), but real liquidity
  clusters tightly around the top of book, so the gap is nearly always a few
  ticks.

Two bugs in the scaffold version were fixed here:

1. **`has_bids()`/`has_asks()` were wrong.** They checked whether the level at
   `best_bid_`/`best_ask_` was non-empty. But the sentinel for "no asks" is
   level `MAX_PRICE_TICKS - 1` — if a real order ever rested there, or if an
   order of the *opposite* side sat at the sentinel price, the answer was
   wrong. The book now keeps explicit `bid_count_`/`ask_count_` counters
   (incremented on add, decremented on cancel), making the question exact.
2. **Cancelling the last order on a side scanned up to 10M empty levels.**
   The old rescan walked from the removed price all the way to the array edge
   looking for a non-empty level that didn't exist. Now the rescan first checks
   the side counter: if the side just became empty, reset the sentinel directly
   and skip the scan entirely.

### Fixed: the 240MB object

The scaffold declared `std::array<PriceLevel, 10'000'000> levels_` as a plain
member, which makes `sizeof(OrderBook)` ≈ 240MB — and `MatchingEngine` holds an
`OrderBook` *by value*, so `MatchingEngine engine;` as a local variable would
have overflowed the stack instantly. Same fix as the memory pool: the levels
now live behind a `std::unique_ptr<PriceLevel[]>`, one heap allocation in the
constructor. (`make_unique<T[]>(n)` also value-initializes every level, so all
queues start empty — no separate zeroing pass needed.)

### Cancel path and the OrderId map

`cancel_order(id)` is: hash map lookup → intrusive unlink → counter/best
repair. The `unordered_map<OrderId, Order*>` is `reserve`d to 100K buckets up
front because rehashing moves every bucket — an unpredictable multi-microsecond
stall if it happened mid-session. Noted as future work in the header: replace
the hash map with a dense slot array indexed by order id + generation counters,
which turns the lookup's hash-and-probe into a single array index.

One ownership subtlety: `cancel_order` removes the order from the book's
structures but does **not** free the `Order` — the book never allocates, so it
never deallocates. The matching engine owns the pool and returns the slot after
the cancel succeeds.

### Test design notes (`order_book_test.cpp`)

- Tests use a fixture (`TEST_F`) holding the book plus a `std::deque<Order>`
  order arena. A deque (not a vector) because the book stores raw `Order*`
  pointers: deque `push_back` never relocates existing elements, while a vector
  reallocation would invalidate every pointer the book holds.
- `CancelMiddleOrderPreservesFifo` asserts on the raw `prev`/`next` pointers to
  prove the intrusive list rewires correctly around a removed node — this is
  the direct PriceLevel coverage promised in Commit 3.
- `CancelLastOrderOnSideResetsSentinel` pins down scaffold bug #1/#2 above:
  with asks still resting, cancelling the only bid must flip `has_bids()` to
  false via the counter, not by scanning.
- `DeepBookCancelWalk` cancels the best bid 100 times in a row and checks the
  best pointer walks down one level at a time — the lazy-repair scan under
  sustained pressure.
- Each test constructs its own book, i.e. a fresh 240MB allocation per test.
  That's why the suite takes a few seconds — correctness tests prioritize
  isolation over speed; the benchmarks (Commit 7) reuse one book.

---

## Commit 5 — Lock-Free SPSC Ring Buffer (`spsc_queue.h`)

### The problem it solves

The engine has three threads (ingest → match → metrics) that must hand
messages to each other. A mutex-protected queue means every handoff can block,
and a blocked matching thread is a latency spike. The SPSC queue lets the two
threads communicate with **no locks, no syscalls, no blocking** — just two
atomic indices over a shared ring of slots.

"SPSC" — *single* producer, *single* consumer — is the crucial restriction.
With exactly one thread writing `head_` and exactly one writing `tail_`, there
are no compare-and-swap loops, no contention, no ABA problem. The architecture
(one queue per thread boundary) is what makes this simple queue sufficient.

### How the ring works

A fixed array of `Capacity` slots and two indices that wrap around:
`head_` = where the producer writes next; `tail_` = where the consumer reads
next. `head_ == tail_` means empty. Full is "head is one step behind tail",
which is why **one slot always stays unused** — without that sacrifice, full
and empty would both look like `head_ == tail_` and be indistinguishable.
Usable capacity is therefore `Capacity - 1`.

Capacity must be a power of two so that wrapping is `index & (Capacity - 1)`
— a single AND instruction — instead of `index % Capacity`, an integer
division costing ~20-40 cycles.

### Memory ordering: why acquire/release, and what it means

On modern CPUs (and compilers), memory operations can be reordered. Without
constraints, the consumer could observe the new `head_` value *before* the
item write that preceded it — and read garbage. C++ atomics let us forbid
exactly the reorderings that would break us, and no more:

- **`store(x, memory_order_release)`** — nothing written *before* this store
  may be reordered *after* it. The producer writes the slot, *then* releases
  the new head: "everything I did is visible once you see this index."
- **`load(memory_order_acquire)`** — nothing *after* this load may be
  reordered *before* it. When the consumer acquires `head_` and sees the new
  value, the slot contents are guaranteed visible.
- **`memory_order_relaxed`** — no ordering at all, just atomicity. Used when
  a thread reads the index *only it ever writes* (the producer reading its
  own `head_`): there's nothing to synchronize with yourself.

The same handshake runs in reverse for `tail_`: the consumer releases the new
tail only after it finished copying the item out, so the producer's acquire
load of `tail_` proves the slot is safe to overwrite.

Why not `memory_order_seq_cst` (the default)? It adds a full memory fence on
x86 (`mfence`/locked instruction, dozens of cycles) to guarantee a *global*
order across all atomics — a property this algorithm doesn't need. Why not a
mutex? A mutex is a possible syscall and a possible context switch (micro-
seconds); this handshake is a handful of nanoseconds.

### False sharing and the cache-line padding

CPU cores don't share individual bytes — they share 64-byte **cache lines**,
and only one core can hold a line for writing at a time. If `head_` (written
by the producer core) and `tail_` (written by the consumer core) sat in the
same line, every push would yank the line away from the consumer's core and
every pop would yank it back — "false sharing," a silent 10x throughput killer
even though the threads never touch the same variable.

`alignas(64)` on each atomic forces them onto separate lines. The buffer
pointer also gets its own line: it's *read* by both sides on every operation,
and if it shared a line with `tail_`, every consumer index bump would
invalidate the producer's cached copy of the pointer. (The scaffold version
had exactly this bug — `buffer_` sat immediately after `tail_`.)

### The cached-index optimization

The scaffold checked full/empty by loading the *other* thread's index on every
single operation — a guaranteed cross-core cache miss each time. The fix:
each side keeps a private, non-atomic copy of the other's index and trusts it
until it *looks* like the queue is full/empty; only then does it reload the
real atomic:

```cpp
if (next == tail_cache_) {                            // probably full?
    tail_cache_ = tail_.load(memory_order_acquire);   // reload reality
    if (next == tail_cache_) return false;            // actually full
}
```

The insight: a stale cache is always *pessimistic* (the consumer only ever
frees more slots, never un-frees them), so trusting it is safe — worst case
is one unnecessary reload. In the common case a push touches only the
producer's own cache line and the slot. This is the difference between ~10M
and ~100M+ messages/sec.

### Other decisions

- **`static_assert(std::is_trivially_copyable_v<T>)`** — slots are copied
  with plain assignment while the other thread runs concurrently; only POD
  types make that safe. This enforces the "messages are fixed-size POD" rule
  from the plan at compile time.
- **Heap-backed buffer** (same fix as pool and book): the stress test's queue
  alone would be megabytes inline — an instant stack overflow on Windows'
  1MB default stack.
- **`size()`/`empty()` are labeled racy** — each loads two atomics that can
  change mid-computation. Fine for monitoring dashboards, never for control
  flow (that's what the return values of `try_push`/`try_pop` are for).
- **MSVC warning C4324 silenced** in CMake — it warns that `alignas` padded
  the struct, which is precisely the intent.

### Test design notes (`spsc_queue_test.cpp`)

- Single-threaded tests pin the edge cases: pop-from-empty leaves the out-
  param untouched, the `Capacity - 1` usable-slot rule, pop-one-push-one at
  the full boundary, and index wrap-around over 100 ring cycles.
- The stress test deliberately uses a *small* (1024-slot) ring for 1M items,
  unlike the scaffold's 1M-slot ring where the producer would never catch the
  consumer. A small ring forces thousands of wrap-arounds and constant
  collisions at the full/empty boundaries — the exact paths where a memory-
  ordering bug would corrupt or drop items. One caveat kept in mind: x86 has
  a strong memory model that masks some ordering bugs; the same test running
  on ARM (weaker model) would be a stricter judge. The acquire/release
  reasoning above is what makes it correct on both.

---

## Commit 6 — The Matching Loop (`message.h`, `matching_engine.h/.cpp`)

### Message types and `std::variant`

`message.h` defines the POD structs that flow through the SPSC queues, plus
two tagged unions:

```cpp
using EngineMessage = std::variant<NewOrderMessage, CancelMessage>;
```

A `std::variant` is a type-safe union: it holds exactly one of its
alternatives at a time plus a hidden tag saying which. Unlike a raw C union
you can't read the wrong member — `std::get_if<T>` returns nullptr if the
tag doesn't match. Crucially, a variant over trivially copyable types is
itself trivially copyable, so variants can travel through the SPSC queue
directly; the `static_assert`s in the header prove it at compile time.

The scaffold had a separate `Fill` struct identical to `FillMessage`; that
duplication is gone (`using Fill = FillMessage`) — a fill *is* the outbound
message, no translation step.

### The matching algorithm

Price-time priority falls out of two nested loops:

```
outer loop (PRICE priority):  walk levels from the best opposite price inward
    re-read book.best_ask() each iteration - the book repairs it as levels empty
    limit orders break when the level no longer crosses their limit
inner loop (TIME priority):   fill_level() consumes orders front-to-back (FIFO)
```

Per fill: take `min(incoming remaining, passive remaining)`, update the
passive order's `filled_quantity`, subtract from the level aggregate
(`reduce_quantity` — *this* is where the ordering rule from Commit 3
matters), emit a fill, and if the passive order is done, remove it via
`book_.cancel_order()` — which already handles the map erase, side counters,
and best-price repair — then return its slot to the pool.

Three rules encode exchange semantics:

- **Execution at the passive price.** A bid at 100.50 hitting an ask resting
  at 100.00 trades at 100.00 — the maker set the price, the taker accepted
  it. This is how real venues work, and it gives price *improvement* to the
  aggressor, never worse.
- **Limit orders stop at their limit; the remainder rests.** After the loop
  breaks, any unfilled quantity becomes a resting order at the limit price —
  with `filled_quantity` carried over, so a 100-lot that matched 30 rests
  showing 70 remaining.
- **Market orders never rest.** They sweep until filled or the side is
  empty; any remainder is simply discarded (there is no price at which a
  market order could rest).

This also maintains the **uncrossed-book invariant** from Commit 4: an
incoming order only rests after consuming everything it crosses, so bids and
asks can never overlap.

### Interface change: no vector-by-value on the hot path

The scaffold returned `std::vector<Fill>` by value — a fresh heap allocation
per processed order, which would have been the single biggest cost in the
whole pipeline and a violation of the zero-malloc rule. The engine now owns
one `fills_` vector, reserved to 1024 at construction, cleared (which keeps
capacity) and reused every call, returned by const reference. Steady state:
zero allocations. The trade-off is a lifetime rule — the returned reference
is valid only until the next `process` call — which is fine for the intended
single-threaded drain loop and is documented in the header.

### Allocate late: takers never touch the pool

The scaffold allocated a pool slot for every incoming order and freed it if
the order didn't rest. The engine now matches using just the message fields
and a local `remaining` counter, and only allocates when a residual actually
needs to rest. Consequences:

- a pure taker (fully filled on arrival) costs zero pool traffic;
- pool exhaustion degrades gracefully — the residual is dropped instead of
  crashing (a real venue would reject the order; we note the policy);
- allocation failure can't corrupt matching, because fills have already
  happened by the time we try to rest.

### Ownership summary (who frees what)

The pool is the only allocator; the book never allocates or frees. An order's
slot is returned in exactly one of three places: it was fully filled during
matching (`fill_level` frees it), it was cancelled (`process_cancel` frees
it), or it never rested (market remainder/duplicate id — freed immediately).
The `PoolAccountingAcrossMixedFlow` test walks a mixed scenario asserting
`pool_available()` at each step to prove no slot leaks.

### Test design notes (`matching_engine_test.cpp`)

All five scaffold scenarios are implemented (cross, partial fill, price-time
priority, market sweep, cancel) plus the edges that catch real matching bugs:
execution at the passive price, a better-priced level filling before an
earlier-quoted worse one, a limit order sweeping one level then stopping at
its price while the next level survives, market order on an empty book, and
cancelling an already-filled order (must fail — the order is gone). Fill
assertions check the aggressive/passive id pairing, not just quantities, to
pin down who traded with whom.

---

## Commit 7 — Micro-benchmarking Framework (`synthetic_workload.h`, `synthetic_generator.cpp`, `order_book_bench.cpp`, `ring_buffer_bench.cpp`)

### How Google Benchmark works (and the primitives used everywhere)

Google Benchmark runs your code in a loop and *decides the iteration count
itself*: it keeps increasing iterations until the total runtime is
statistically stable (default ~1s per benchmark), then reports time per
iteration. That's why every benchmark has the shape:

```cpp
for (auto _ : state) { /* the ONE operation being measured */ }
```

Four primitives appear throughout our benchmarks:

- **`benchmark::DoNotOptimize(x)`** — tells the optimizer "assume `x` is
  used." Without it, a Release-mode compiler sees that a result is never read
  and deletes the entire computation — you'd be measuring an empty loop.
- **`benchmark::ClobberMemory()`** — a compiler-level memory barrier: "assume
  all memory was touched." Stops the compiler from caching writes in
  registers across iterations.
- **`state.PauseTiming()` / `ResumeTiming()`** — stop the clock for periodic
  housekeeping (refilling a drained book, rebuilding an engine). The pause
  itself has overhead (~100ns+), so it must never wrap the *hot* operation —
  we only use it for work that happens once per *batch*, amortized to ~zero.
- **`state.SetItemsProcessed(n)`** — converts "ns/iteration" into
  "operations/second" in the report, which is the number the plan's targets
  are written in.

### Why the workload generator became a header (`tools/synthetic_workload.h`)

The commit plan asks the generator to "cycle millions of requests through
Google Benchmark" *and* to write files for the replay tool. Those are two
consumers of the same logic, so the generation moved into a reusable class
(`synth::WorkloadGenerator`) and the CLI (`synthetic_generator.cpp`) became a
thin wrapper that serializes the stream to disk. The benchmarks call
`generate(n)` directly — no file round-trip.

Two properties were designed in:

- **Determinism.** Fixed RNG seed *and* logical-counter timestamps (no
  wall-clock reads). Two runs with the same config produce byte-identical
  streams, so a benchmark number today is comparable with one next week —
  you're never chasing a regression that was actually a different workload.
- **Well-formed cancels.** Cancels always target a randomly chosen still-live
  order id, tracked in a `live_ids_` vector with **swap-and-pop** removal
  (swap the chosen element with the last, `pop_back`) — O(1) removal where
  erase-from-middle would be O(n). Only *limit* orders enter the live set:
  market orders never rest, so cancelling one is meaningless. The engine may
  have already filled an order the generator still considers live — that
  cancel returns false, which is realistic (exchanges race cancels against
  fills all day).

### Order book benchmark design (`order_book_bench.cpp`)

**One book per benchmark, reused across iterations.** A fresh book is a
~240MB allocation; constructing one per iteration would be the entire
measurement. This is the flip side of the test suite's choice (isolation via
fresh books) — benchmarks want steady-state, tests want isolation.

**Disjoint price bands.** The standalone `OrderBook` relies on the engine to
keep it uncrossed (Commit 4). Benchmarks that drive the book directly
(add/cancel) generate bids in one band (~$99.50) and asks in another
(~$100.50) so the invariant holds with no matcher present.

Per-benchmark decisions:

- **`BM_AddOrder`** — a 65K-order arena is regenerated and the book drained
  every 65K iterations, off the clock. Measures: level append + hash map
  insert + best-price comparison. The arena is a `std::vector<Order>` sized
  once — the benchmark itself does zero allocation on the timed path.
- **`BM_CancelOrder`** — cancels in *shuffled* order, not insertion order.
  Sequential cancels would drain levels in a fixed pattern; random cancels
  hit middles of queues and occasionally empty the best level, exercising
  the lazy best-price rescan the way real flow does.
- **`BM_MatchOrder`** — the "giant maker" trick: rest one ask with quantity
  10⁹, then send one-lot crossing bids. Each iteration produces exactly one
  fill and nothing rests, so there's no per-iteration cleanup at all — the
  maker is replenished (paused) once per ~10⁹ fills, i.e. effectively never.
  The taker reuses one id, which is safe *because* it never rests (ids only
  matter once an order enters the book).
- **`BM_AddCancel_PoolVsMalloc`** — identical book operations on both arms;
  the *only* difference is where the `Order` comes from (pool vs
  `new`/`delete`). This isolates the allocator's contribution to a full
  add+cancel round trip — the number that justifies Commit 2's existence.
- **`BM_SyntheticStream`** — the headline: millions of generated messages
  through `MatchingEngine::process`, mixing adds, cancels, market orders and
  natural crossings. The engine is rebuilt fresh each iteration (paused) so
  every replay starts from an empty book and unique ids. Cancel rate is set
  to 0.35 to keep the resting population comfortably under the 1M-order
  pool. The `fills_per_run` counter is reported so you can see the stream
  actually matched (~585K fills per 1M messages) — a throughput number over
  a stream that never crossed would be meaningless.

### Ring buffer benchmark design (`ring_buffer_bench.cpp`)

**The consumer thread persists across iterations.** The scaffold spawned a
thread *per iteration*; thread creation is tens of microseconds, which would
drown a microsecond-scale measurement for small batches. Instead one consumer
is created before the timing loop and told to stop after it.

**Measuring without distorting.** The producer must know when the consumer
has received everything (otherwise an iteration would end while items are
still in flight, under-measuring). But if the consumer bumped a shared atomic
counter per item, that write would fight with the queue's own cache traffic
and slow down the very thing being measured. Compromise: the consumer counts
locally and *publishes* to the shared counter only every 1024 items or when
the queue is momentarily empty. The fast path stays clean; the producer's
end-of-iteration wait is exact.

- **`BM_SPSC_PushPop_SingleThread`** — one thread, push+pop. No cross-core
  traffic, so this is the pure instruction cost (~1ns) — the upper bound.
- **`BM_SPSC_Throughput<T>`** — templated on payload: `uint64_t` (8B) vs
  `EngineMessage` (the real 48B variant: a 40B `NewOrderMessage` + the
  variant's type tag, padded). Run both and you see whether the
  pipe's cost is per-item or per-byte. Note when reading results: batch
  sizes that fit inside the ring (65K items ≈ ring capacity) include a
  "drain tail" where only the consumer is working, so the *larger* batches
  reflect steady-state overlap and are the honest throughput number.
- **`BM_SPSC_RoundTrip`** — two queues, an echo thread, ping-pong. One
  iteration = 2 pushes + 2 pops + 2 cross-core cache-line handoffs. This is
  the latency-oriented view (~190ns round trip ⇒ ~95ns one-way), and the
  closest proxy for "how long does a message take to reach the next thread"
  in the real pipeline.

### CMake: the warnings interface library

Enabling benchmarks broke the build in an instructive way: our global
`add_compile_options(-Wconversion -Wsign-conversion ...)` applied to *every*
target in the build tree — including Google Benchmark's own sources, which
compile with `-Werror`. Their code has (benign) sign conversions; our flags
turned them into hard errors.

The fix is the idiomatic modern-CMake pattern: an **INTERFACE library**
(`engine_warnings`) that carries the flags as usage requirements. Targets
that link it (all of *our* code) get the warnings; third-party code fetched
via FetchContent builds with its own settings. An INTERFACE library compiles
nothing itself — it's a named bundle of flags/includes/dependencies.

Google Benchmark also gets the same treatment as GTest: `find_package` first
(vcpkg/system), FetchContent download as fallback, so the project still
builds with zero pre-installed dependencies. `BUILD_BENCHMARKS` and
`BUILD_TOOLS` now default ON.

### Linux toolchain note

This machine had GCC 11 but no CMake, no make, no pip. Rather than touch the
system, CMake 3.30 and Ninja binaries live in `.toolchain/` inside the repo
(gitignored). Configure with:

```bash
export PATH="$PWD/.toolchain/cmake-3.30.5-linux-x86_64/bin:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_MAKE_PROGRAM=$PWD/.toolchain/ninja
cmake --build build -j$(nproc)
```

### First (noisy) numbers — treat as smoke test only

Measured on this machine with CPU frequency scaling ON and background load —
real numbers for `docs/benchmarks.md` need a quiet machine, performance
governor, and ideally core pinning:

| Benchmark | Result |
|---|---|
| `BM_AddOrder` | ~11.5 ns |
| `BM_CancelOrder` | ~39 ns |
| `BM_MatchOrder` (1 fill) | ~8.3 ns |
| Add+cancel, pool | ~19 ns |
| Add+cancel, malloc | ~29 ns |
| `BM_SyntheticStream` | ~22–25M msgs/sec through the full engine |
| SPSC push+pop (1 thread) | ~1.2 ns |
| SPSC round trip (2 threads) | ~188 ns |

Already comfortably inside the plan's aspirational targets (sub-500ns add,
sub-1µs match) — but the honest measurement happens in Phase 4 with `perf`
on bare metal.
