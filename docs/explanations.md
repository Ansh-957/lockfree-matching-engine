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
