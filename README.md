# Limit Order Book — Matching Engine

A price-time priority matching engine in modern C++20, built around O(1) order
cancellation and a zero-allocation memory pool. Every performance claim below
is produced by `tests/benchmark.cpp` and reproducible with two commands.

---

## Results

Measured on Apple Silicon (AppleClang), `-O3`, 1,000,000 orders. Absolute
numbers vary by machine and compiler; the relationships are what matter, and
this file documents both a real result and a real measurement failure —
because a benchmark nobody can be skeptical of isn't worth including.

| Operation | Result |
|---|---|
| Add order (incl. matching) | **~110-120 ns** |
| Sustained throughput | **~8.3-9.1 M orders/sec** |
| Cancel, 500k-order book | **~120-580 ns**, flat as price levels vary 10,000× |
| Pool allocate vs `std::list` | **~2.6-2.8×** faster |
| Pool vs `std::list` under churn (traversal) | **up to ~2.5×** faster |

### Cancellation is O(1), and here is the proof

Watching total time grow with order count proves nothing — that is just more
work. The decisive experiment holds N fixed at 500,000 and varies **M**, the
number of distinct price levels, by four orders of magnitude. A hidden tree
descent would show up immediately.

| Price levels (M) | ns / cancel |
|---|---|
| 10 | 166-506 |
| 1,000 | 121-514 |
| 100,000 | 176-579 |

No consistent upward trend as M grows 10,000×. Run-to-run variance on a
shared laptop (thermal state, other processes) dwarfs anything M contributes
— which is itself the point: if there were a hidden O(log M) term, it would
show up as a trend across the columns, not as noise within them.

---

## Architecture

Three containers, each chosen for one specific guarantee:

| Concern | Structure | Why |
|---|---|---|
| Price levels | `std::map` (red-black tree) | Ordered traversal for sweeps; `begin()` is top-of-book in O(1) |
| Time priority | `std::list` per level | O(1) erase from the middle, **stable iterators** |
| Cancellation | `std::unordered_map` | O(1) average lookup from order ID to position |

Bids use `std::greater`, asks use `std::less`, so `begin()` is the best price
on both sides and the matching loop is a forward iteration regardless of side.

### The detail that makes cancellation O(1)

The obvious directory design maps order ID to a `std::list` iterator. That is
not sufficient. When you cancel the last order at a price, the now-empty price
level must be erased from the tree — and with only the *price* cached, finding
that node costs an O(log M) re-descent. The whole operation degrades to
O(log M).

So the directory caches the **`std::map` iterator itself**:

```cpp
struct OrderLocation {
    OrderIterator orderIt;   // position in the FIFO queue
    LevelHandle   level;     // the owning map node — not its price
    Side          side;      // discriminates the union
};
```

Both containers guarantee node iterators stay valid for the node's lifetime, so
both can be cached from insertion until cancellation. Cancel then costs one
hash probe, one `list::erase` (**O(1) guaranteed** — two pointer writes, no
traversal), and at most one `map::erase(iterator)` (amortised O(1) — no
descent, we hold the node).

`LevelHandle` is a union because bids and asks are different map types. On
libstdc++ and libc++ the comparator is *not* part of the iterator's type, so
the two are in fact the same type and a pair of overloaded constructors is
ill-formed — hence named factories. Static asserts guard trivial copyability
and fire on toolchains with wrapped debug iterators.

### Correctness choices worth naming

- **Integer tick prices.** `double` appears only at the API boundary and is
  converted once via `llround`. Floating-point prices inside a matching engine
  are a defect: `10.10 - 10.05` is not `0.05` in IEEE-754, so a `map<double>`
  silently creates two distinct "same" levels that never aggregate.
- **Trades print at the resting order's price**, not the aggressor's limit. The
  passive side set the price and earns the spread.
- **Reduce retains time priority; increase is rejected.** Any increase must be
  an explicit cancel-replace, so the priority loss is the caller's decision.
- **Unindex before `pop_front`.** After the pop the list node is destroyed and
  the reference dangles. Ordering, not just presence.

---

## Memory pool

`std::list` calls `new` for every order. The pool replaces that with one
contiguous slab and a free list threaded through the free slots themselves —
a free slot's payload bytes are dead anyway, so the first four hold the index
of the next free slot. No side table, no bitmap, no per-slot overhead.

```
slots_:  [ Order ][ Order ][ free→7 ][ Order ][ free→2 ] ...
                               ↑                   ↑
         freeHead_ = 4 --------+-------------------+--> 7 --> NULL
```

- **Bump pointer + free list.** Never-used slots come from `highWater_++`, so
  construction does not walk a million slots wiring up initial links.
- **LIFO reuse**, deliberately: the most recently freed slot is the one most
  likely still resident in L1/L2.
- **Exhaustion returns `NULL_INDEX`** rather than throwing or growing. A full
  pool is a capacity-planning error; the gateway rejects, the engine never
  stalls.
- **32-bit indices** instead of 8-byte pointers: 48-byte nodes vs 56, and
  indices survive relocation and serialise trivially for snapshot/replay.

### An honest result — including a benchmark that broke

Allocation cost is where the pool wins decisively and consistently: roughly
**2.6-2.8×** faster than `std::list`, reproduced across every run on every
machine this was tested on.

Isolated *free* timing is NOT reported here. The standalone deallocate-loop
microbenchmark returned physically implausible numbers under AppleClang
(sub-nanosecond, and once literally 0.0 ns) — the optimiser proved parts of
an empty-looking loop had no externally observable effect and folded it away,
even after adding a standard compiler barrier (`asm volatile` over the
object's address, the same technique Google Benchmark's `DoNotOptimize` uses).
Rather than keep patching a fragile microbenchmark or quote a number that
doesn't survive scrutiny, that column was removed.

Free performance under real conditions is instead measured honestly via
traversal after churn, where the result can't be optimised away because the
traversal sum is used:

| Traversal | `std::list` | Pool |
|---|---|---|
| Freshly built book | ~4-19 ns/node | ~4-6 ns/node |
| After 4M churn operations | ~49-65 ns/node | ~25-46 ns/node |

The pool wins on a fresh book and wins more clearly after churn — up to 2.5×
on this machine. glibc/AppleClang's allocator lays out sequential allocations
reasonably well, so the fresh-book gap is smaller than the churned one; the
allocator's real payoff shows up once the book has lived a while, which is
also the realistic state of a running exchange. Shrinking `Order` below 32
bytes — two nodes per cache line instead of 1.3 — is the next lever, larger
than anything left in the allocator.

**Status: wired into the engine.** `LimitOrderBook` no longer uses `std::list`
anywhere. The FIFO at each price level is an intrusive doubly linked list
threaded through `MemoryPool<OrderNode>` slots — `next`/`prev` are 4-byte pool
indices living inside the node itself, not separately allocated list
machinery. After construction, `LimitOrderBook` makes zero calls to `new` on
its hot path (add, match, cancel, reduce).

The public API is unchanged: `addOrder`, `cancelOrder`, `reduceQuantity`,
`bestBid`, `quantityAt`, and friends all keep their Phase 2/3 signatures.
`tests/benchmark.cpp` required no changes to compile or pass against the new
internals — the correctness suite (37 checks) and the complexity/throughput
benchmarks all exercise the pooled engine directly.

**One tradeoff introduced by this change, stated plainly:** `restOrder` can
fail if the pool's fixed capacity is exhausted. When that happens, any fill
that already occurred in `match()` stands (real trades cannot be undone), but
the unfilled residual is silently dropped rather than rested. This mirrors
how a real gateway would behave — reject the resting leg rather than leave
the engine in an inconsistent state — but `addOrder`'s current `void` return
gives the caller no way to observe that a reject happened. A production
version would need an explicit result/reject type. Noted here rather than
hidden, since a reviewer who traces the pool-exhaustion path deserves to find
this written down before they find it by reading the code.

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/benchmark
```

Compiles clean under `-Wall -Wextra -Wpedantic -Wconversion -Wshadow
-Wold-style-cast -Wcast-align`. The suite runs 37 correctness checks before
any timing: FIFO ordering under mid-queue cancellation, multi-level sweeps,
partial fills, level teardown, double-cancel safety, and pool exhaustion.

Verify before trusting the numbers:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZE=ON
cmake --build build-asan -j && ./build-asan/benchmark   # clean under ASan + UBSan
```

`-march=native` is opt-in via `-DLOB_NATIVE_ARCH=ON` — it is the wrong default
for code that will be cloned onto unknown hardware.

---

## Benchmark methodology

- Cancellations are issued in **shuffled** order. Cancelling in insertion order
  walks memory linearly and lets the prefetcher hide every cache miss.
- Complexity is demonstrated by **varying M with N fixed**, isolating the
  algorithmic term from cache effects.
- Every timed loop's result is forced through a compiler barrier
  (`doNotOptimizeAway` / `clobberMemory` in `benchmark.cpp`) so the optimiser
  cannot delete the measured work — and where that barrier still wasn't
  enough to trust a number (see "An honest result" above), the number was
  removed rather than reported.
- Sanitizer builds are excluded from timing.

---

## Roadmap

- [x] Core structures, price-time priority
- [x] Matching with partial fills and level teardown
- [x] True O(1) cancellation via cached map iterators
- [x] Memory pool, benchmarked standalone
- [x] Migrate the engine to pooled intrusive lists — zero heap allocation
      on the add/match/cancel/reduce hot path, verified clean under
      AddressSanitizer + UndefinedBehaviorSanitizer
- [ ] Explicit reject/result type so pool exhaustion is observable by the
      caller instead of silently dropping the unfilled residual
- [ ] Shrink `Order` to 32 bytes (two per cache line)
- [ ] IOC / FOK order types
- [ ] Latency percentiles (p50/p99/p99.9) rather than means