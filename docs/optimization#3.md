# Optimization #3: `OrderManager` slab with generation-tagged ids

Goal: take the hash map and both per-order heap allocations off the order
path. `OrderManager` stored orders as
`std::unordered_map<order_id_t, std::unique_ptr<Order>>`, so every add did a
node `malloc` plus an `Order` `malloc`. Every lookup chased three dependent
pointers (bucket → node → `Order`), and every rehash was an O(n) stall in the
middle of trading.

Builds on [optimizations#2](optimizations%232.md), whose follow-ups listed
"the `unordered_map` rehash, and the `make_unique<Order>` for every order".

---

## 1. The slab

Files: `include/me/OrderManager.h`, `src/me/OrderManager.cpp`

- **Storage.** One `std::vector<Slot>`, sized at construction
  (`DEFAULT_CAPACITY = 65,536`; the size is a constructor argument). Each slot
  holds an `Order` by value plus two 32-bit fields:

  ```cpp
  struct Slot {
      Order    order;        // 24 bytes, inline, no unique_ptr
      uint32_t generation;   // odd = live, even = free
      uint32_t next_free;    // intrusive free list, meaningful only while free
  };                          // 32 bytes: two slots per cache line
  ```

  A `static_assert(sizeof(Slot) <= 32)` makes any growth of `Order` a
  deliberate decision.
- **Free list.** Free slots are threaded through their own `next_free` field,
  so there's no separate container. Allocation pops the head and release
  pushes it. The list is LIFO on purpose: the most recently freed slot is the
  one most likely to still be in cache.
- **Pre-faulting.** The constructor uses `resize`, not `reserve`, so every
  slot's page is touched up front and the first orders into a new slab take no
  page faults.
- **Growth.** When the free list is empty, the vector doubles and the new
  slots are threaded onto the free list. This is safe because nothing outside
  `OrderManager` holds a pointer into the slab: callers hold ids, and `Slot*`
  never lives longer than one call. Growth is an O(n) copy, so size the slab
  to the expected high-water mark. See *Tail latency* below for what it
  costs.

### Id layout

`order_id_t` is now `uint64_t`:

```
[ 63 .......... 32 | 31 ........... 0 ]
[    generation    |    slot index    ]
```

A lookup (`resolve()`) is a bounds check, one indexed load, and one compare:

```cpp
if (index >= slots.size()) return nullptr;
Slot& s = slots[index];
if (s.generation != generation_of(id) || (s.generation & 1) == 0) return nullptr;
```

### Why the generation is needed

Without it, slot reuse is an ABA bug. An order is cancelled, its slot is
reused, and then a late or duplicate cancel for the *old* id lands on the
*new* order. Every slot's generation is bumped on allocate **and** on release,
so it is odd exactly while the slot is live. An id carries the odd generation
its slot had at allocation, so the moment that order dies the id stops
matching, even after the slot is reused.

- **Parity check.** It stops a forged id with an even generation from
  resolving to a free slot.
- **No id 0.** Live generations are odd, so no id is ever 0. Zero remains free
  as a not-found sentinel (`BookLevel::fillOrders` relies on this).
- **Wrap-around.** A slot's generation wraps only after 2³¹ reuses *of that
  slot*. Wrapping keeps the parity, so even then 0 can't be minted.

### What it replaced, operation by operation

| | `unordered_map` + `unique_ptr` | slab |
|---|---|---|
| add | hash, node `malloc`, `Order` `malloc` | pop free list, construct in place |
| lookup | hash → bucket → node → `Order` | mask → `slots[idx]` |
| erase | two `free`s, bucket relink | bump generation, push free list |
| hot-path allocations | 2 per order | 0 (until the slab has to grow) |
| stale-id safety | ids never reused | generation compare |

---

## 2. Knock-on changes

### `order_id_t` widened to 64 bits (`include/me/Order.h`)
- `EventVariant` grows from 40 to 48 bytes, because `OrderModified` carries two
  ids. The `static_assert` in `include/me/Events.h` was raised to 48. The
  128-slot event ring goes from 5 KiB to 6 KiB, still well inside L1d and the
  8 KiB assert in `EventQueue.h`.
- Ids are opaque and **not sequential**. Arrival order (time priority) comes
  only from each `BookLevel`'s FIFO vector, which never relied on ids being
  ascending. The `BookLevel.h` comment that said "FIFO (= ascending id)" was
  corrected.

### `Order` shrunk from 32 to 24 bytes (`include/me/Order.h`, `src/me/Order.cpp`)
The slot `static_assert` fired as soon as the id widened, and caught this:
- `OrderSide` and `OrderType` are now `enum class : std::uint8_t`.
- `Order` stores its fields directly instead of embedding an `OrderView`.
  `OrderView` rounds up to 24 bytes on its own (8-byte alignment), and the two
  lifecycle flags then spilled into a fourth word. Stored directly it's
  `id(8) price(4) qty(4) side(1) type(1) cancelled(1) fulfilled(1)` = 24 bytes.
  `getView()` builds the `OrderView` on demand, so the public interface is
  unchanged.

### `BookLevel` reserves 256 ids instead of 512 (`src/me/BookLevel.cpp`)
With 8-byte ids, 512 per level doubled each level's up-front reservation from
2 KB to 4 KB. `BM_Add_NewPriceLevel` opens one level per order, so its RSS
doubled (651 MB → 1.24 GB at 300k iterations) and its page faults doubled
with it. At 256 the footprint per level is the same as before, and the
benchmark went back to parity in a fixed-iteration A/B (714/678/1097 ns new
vs 1205/724/991 ns base, same RSS). Now a named constant,
`kReservedOrdersPerLevel`.

### Benchmarks stored ids in `int` (`bench/*.cpp`)
All four benchmark files did `int id = book.addOrder(...)` (15 sites). With
64-bit ids that silently drops the generation, so every timed cancel or
modify became a *failed* lookup. That made the first round of after-numbers
look far better than they were. All sites now use `order_id_t`. The benchmarks
are built without `project_warnings`, so `-Wconversion` never flagged it.
**Any outside code that keeps ids in a 32-bit integer will hit the same
silent failure.**

---

## 3. Tests

`tests/test_order_manager.cpp`:
- `AddOrderIssuesUniqueIds` used to assert that the first id is 1. It now
  asserts that 0 is never issued.
- New slab tests:
  - `FreedSlotIsReusedUnderAFreshId`: capacity 1, cancel, then add. The slot is
    reused without growing, and the id differs.
  - `StaleIdCannotTouchTheOrderNowInItsSlot`: `valid`, `getView`, `cancel`,
    `modify` and `fulfill` through a stale id all miss, and the new occupant
    is untouched.
  - `IdsStayUniqueUnderHeavySlotReuse`: 1,000 add/fill cycles through a 4-slot
    slab, every id new, no growth.
  - `IdOfANeverUsedSlotIsUnknown`: a never-allocated neighbouring slot, an
    even-generation (forged) id, and 0 are all rejected.
  - `GrowingPastCapacityKeepsExistingOrders`: 50 orders into a 2-slot slab;
    every order survives every doubling.
  - `ZeroCapacityStillAcceptsOrders`.

All 8 C++ suites pass in Debug and Release. All 25 Python tests pass against a
rebuilt wheel. No Python change was needed: pybind11 maps `uint64_t` to `int`,
and the unknown-id test (`987654`) indexes past the slab and misses as it
should.

---

## 4. Measurements

Method: `bench/README.md`'s procedure, `taskset -c 2,3 bench_latency 300000`.
HEAD (`cfe9360`) and this change were built side by side and run
**interleaved**, three rounds each (base, new, base, new, …), so both see the
same thermal state. Each cell is the median of the three runs. Same machine as
before: i7-1250U, g++ 11.4.0, `-O3 -march=native`, WSL2. All values in ns and
include one `steady_clock::now()` pair.

**These absolute values are throttled.** The session had been benchmarking
continuously for ~40 minutes and timer overhead read 36 ns. On an idle
machine (timer overhead 20 ns, `taskset -c 6,7`) the same new binary measures
OrderBook add / modify / cancel P50 at **39 / 98 / 64 ns** and Engine add /
cancel `[RingBuf/128]` at **201 / 247 ns**; see `bench/README.md` for the full
cool-machine table. The *ratios* below are what this comparison establishes:
both builds ran interleaved under the same thermal conditions.

### `OrderBook` (the code this change touches)

| operation | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| add | 117 → **67** | 286 → **148** | 1,820 → **591** | 181 → **100** |
| modify | 216 → **179** | 364 → **355** | 599 → **429** | 249 → **203** |
| cancel | 136 → **116** | 257 → **218** | 352 → **255** | 152 → **129** |

- **Add moves the most**, and the tail moves more than the median (P99.9
  3.1×, P50 1.7×). That ratio is the signature of an allocator leaving the
  path, as it was when events stopped allocating. New-build add P50 was 67 ns
  in all three runs; base varied from 84 to 128.
- **Modify and cancel** improve by 15–20% at P50 and 30% at P99.9. They gain
  less than add because the map was small enough to stay cache-resident in
  this harness, so most of their time is in `BookLevel` and `OrderBookSide`.

### `Engine` (`RingBuf/128`, production queue)

| operation | P50 | P99 | P99.9 |
|---|---:|---:|---:|
| add | 301 → 398 | 866 → 640 | 2,586 → 2,422 |
| modify | 390 → 521 | 837 → 905 | 2,844 → 2,679 |
| cancel | 265 → 238 | 450 → 413 | 772 → 621 |

**These are inside run-to-run noise and should not be read as a regression.**
Engine P50s were bimodal in *both* builds, landing near ~290 or near ~400
from one run to the next (add: base 301/409/285, new 451/287/398; modify:
base 390/514/367, new 521/407/521). That's the thermal-throttling pattern
`bench/README.md` warns about. The new binary ran throttled more often: its
timer overhead read 36 ns in all three runs, against 26/35/35 for base. The
OrderBook rows above improved in every individual run despite that. The
engine rows add risk checks, matching and event publishing on top of the
book, so the book's 20–50 ns saving is a small fraction of their total.

### Google Benchmark suites

Mean ns/op, `--benchmark_min_time=0.5s`, two interleaved runs of each build
(range shown):

| benchmark | base | new |
|---|---:|---:|
| `BM_Add_NewPriceLevel` | 1,452–2,182 | 1,370–1,812 |
| `BM_Add_ExistingPriceLevel` | 82–85 | 90–100 |
| `BM_Add_SpreadAcrossLevels` | 106–114 | 85–96 |
| `BM_Add_MarketOrder` | 84 | 56–73 |
| `BM_Cancel_LastOrderOnLevel` | 498–506 | 334–541 |
| `BM_Cancel_OneOfManyOnLevel` | 345–393 | 534–547 |
| `BM_Cancel_FromDeepBook` | 445–513 | 495–555 |
| `BM_Modify_QuantityOnly` | 988–1,480 | 1,147–1,253 |
| `BM_Modify_ChangePriceLevel` | 710–962 | 839–1,031 |
| `BM_Modify_ChangeSide` | 716–849 | 713–956 |

Most ranges overlap, which is what `bench/README.md` predicts for these
suites. Cancel and modify call `PauseTiming()`/`ResumeTiming()` every
iteration, which costs more than the operation itself. `NewPriceLevel` grows
the book without bound under adaptive iteration counts. Use `bench_latency`
above for conclusions; these are recorded for continuity.

`BM_Add_NewPriceLevel` has an extra trap with this change. Run with
`--benchmark_repetitions`, the new build looked about 4× slower (≈1,300 vs
≈300 ns). That's a glibc artifact: freeing the grown multi-MB slab between
repetitions makes glibc's dynamic mmap/trim thresholds hand the memory back to
the OS, so the next repetition page-faults it all in again. With the
thresholds pinned
(`GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072:glibc.malloc.trim_threshold=4294967296`),
the new build was about 3× *faster* (125–132 vs 338–408 ns). A running engine
doesn't repeatedly free its slab.

---

## 5. Tail latency: why `max` is still in milliseconds

The `max` column is 0.4–3 ms for every row, including the `OrderBook` rows,
which never touch the event queue. It isn't the logger: the `Engine` rows'
consumer thread discards events and does no file I/O at all, and the
`OrderBook` rows have no consumer. Two things contribute, both measured:

1. **The OS/VM floor.** A loop that times nothing but two back-to-back
   `steady_clock::now()` calls has a max of **20–66 µs** per million samples
   on this WSL2 machine (P99.99 42–90 ns, 6–18 samples over 10 µs). That
   comes from timer interrupts, scheduler preemption and hypervisor vCPU
   descheduling; no code change can remove it. Pinning, `isolcpus`,
   `nohz_full`, and running on bare metal rather than WSL2 are the levers.
2. **Slab growth (add only).** `bench_latency` puts 100,000 adds into each
   book before rebuilding it, but the slab starts at 65,536, so one doubling
   per cycle lands inside a timed sample. With the default raised to 2¹⁸ the
   add max fell from 2.7–8.9 ms to **0.43–1.0 ms** and the mean from 96–186 ns
   to 74–76 ns. The default stays at 2¹⁶: 2¹⁸ is 8 MB pre-faulted per
   `OrderBook`, which every test and every Python engine would pay. Production
   should size the slab to its expected peak instead. This is why
   `OrderBook add` max went *up* (2.49 → 2.92 ms) even though P99.9 fell 3×.

The rest of the gap between 66 µs and the ~0.5 ms that remains is page faults
on fresh `BookLevel` vectors after each book rebuild, plus the machine's
throttling.

### Would batching log writes help?

Not these numbers, for the reason above: no benchmark row writes to disk.
The production `Logger` already batches as well: `write_event` streams into
an `std::ofstream` with no `endl` or per-event `flush`, so the `filebuf`
coalesces lines into ~8 KB writes. Where logging *can* reach the matching
thread is backpressure. If the logger thread falls behind (formatting with
`ostringstream`/`put_time` on every event, or a slow disk), the 128-slot ring
fills and `RingBuffer::push` spins. No current harness measures that, because
the drain thread never blocks. The way to find out is a harness whose
consumer runs the real `Logger` against a real file, counting producer spins
in `push()`. If it shows spins, the fixes in order of cost are: cheaper
formatting (binary log records formatted offline, or `std::to_chars` instead
of streams), a larger ring, and explicit batched `write()`s with a larger
buffer.

---

## Follow-ups
- **Expose slab capacity** through `OrderBook`/`MatchingEngine` (and the
  Python constructor), so a deployment can pre-size the slab and never grow
  on the hot path.
- **Intrusive level queues.** Orders now live in stable slots, so `BookLevel`
  can link them with `prev`/`next` slot indices instead of keeping a
  `std::vector<order_id_t>` with dead entries. Cancel then becomes O(1) with
  no tombstones and no `compact()`. Fills would walk the slab directly
  without a second `OrderManager` lookup per order. This is where the
  remaining modify/cancel cost is.
- **Drop `Order::isCancelled`/`isFulfilled`.** A slot's parity already says
  whether the order is live, so the flags are redundant inside the slab. They
  are kept only because `Order` is still tested as a standalone class.
- **Logger backpressure harness**, as described in §5.
