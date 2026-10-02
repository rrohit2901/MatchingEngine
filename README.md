# Matching Engine

![CI](https://github.com/rrohit2901/MatchingEngine/actions/workflows/CI.yml/badge.svg)

A single-symbol limit order book matching engine in C++20, with price-time priority,
pre-trade risk checks, an off-hot-path event log, and Python bindings.

Built to find out where the time actually goes in an order path — so it is benchmarked
per operation at P50 and P99 rather than in aggregate, and the numbers below include the
ones that are unflattering.

## Performance

300k samples per operation, `-O3 -march=native`, on a 12th-gen i7-1250U (WSL2), pinned to
the two hyperthreads of one core (`taskset -c 6,7` cool, `2,3` sustained — equivalent
pairs under WSL2). Every figure includes one `steady_clock::now()`
pair — subtract it. Deeper tail percentiles, and why they are not trustworthy on this
machine, are in [`bench/README.md`](bench/README.md).

This laptop CPU throttles under sustained load and the numbers move with it, so both states
are shown. **Cool** is a single run on an idle machine (timer overhead 20 ns). **Sustained**
is the median of three runs taken after ~40 minutes of continuous benchmarking (timer
overhead 36 ns). Same binary in both columns.

| operation | P50 cool | P99 cool | P50 sustained | P99 sustained |
|---|---:|---:|---:|---:|
| OrderBook add | 39 ns | 148 ns | 67 ns | 148 ns |
| OrderBook modify | 98 ns | 142 ns | 179 ns | 355 ns |
| OrderBook cancel | 64 ns | 85 ns | 116 ns | 218 ns |
| Engine add | 201 ns | 269 ns | 398 ns | 640 ns |
| Engine modify | 528 ns | 893 ns | 521 ns | 905 ns |
| Engine cancel | 247 ns | 409 ns | 238 ns | 413 ns |

`OrderBook` rows are the book in isolation. `Engine` rows add risk checks, matching, and
event publishing. The timer overhead printed by `bench_latency` is the quickest way to tell
which state a run was taken in. Engine P50s also swing by ~100 ns from run to run within a
state, so compare builds by running them interleaved, never one after the other.

**Orders live in a slab, not a hash map.** `OrderManager` used to be an
`unordered_map<id, unique_ptr<Order>>`, which meant two heap allocations per add and three
dependent pointer loads per lookup. It is now a pre-allocated vector of 32-byte slots with
an intrusive free list, and an order id encodes its slot, so a lookup is an index plus a
generation check and nothing on the order path allocates. Against the previous commit,
both built and run interleaved in the same (sustained) session, so the deltas are fair even
though the absolute values are the throttled ones:

| OrderBook operation | P50 | P99.9 |
|---|---:|---:|
| add | 117 → 67 ns | 1,820 → 591 ns |
| modify | 216 → 179 ns | 599 → 429 ns |
| cancel | 136 → 116 ns | 352 → 255 ns |

Design, the id scheme, the knock-on changes, and why `max` is still in milliseconds:
[`docs/optimization#3.md`](docs/optimization%233.md).

**Events no longer allocate** (previous change; figures below are from that change's
measurement, 2026-09-11). The queue used to carry `unique_ptr<Event>`: a `new` on the
matching thread and a `delete` on the logger thread, which is a cross-thread free and the
slow path in every general-purpose allocator. Events are now flat structs in a trivially
copyable `std::variant` (40 bytes then, 48 since ids became 64-bit) carried by value, so a
slot write is a memcpy and the publish path allocates nothing.

Measured directly — construct one `TradeEvent` and push it into the ring with a consumer
draining, 400k samples, median of five interleaved runs of both binaries:

| publish one event | P50 | P99 |
|---|---:|---:|
| `make_unique<TradeEvent>` + ring push | 111 ns | 742 ns |
| construct in the variant + ring push | 77 ns | 124 ns |

The P50 is a third cheaper; the tail is where it really shows, because the allocator is no
longer in the path at all.

End to end, against the previous figures on the same machine and methodology:

| Engine operation | P50 | P99 |
|---|---:|---:|
| add | 768 → 383 ns | 2,372 → 843 ns |
| modify | 827 → 557 ns | 2,359 → 1,023 ns |
| cancel | 452 → 283 ns | 1,482 → 525 ns |

The `OrderBook` rows are the control for that comparison — unchanged code, so they should
not move. Their P50s matched the earlier run within 4% (82/217/148 before, 84/226/151 after),
which is what makes the P50 column above a fair comparison. Their **P99s are 1.2-1.7x
worse** than the earlier run (188/289/251 before, 220/485/333 after), so the machine was having
a worse tail day than the baseline: the P99 improvements are understated, not flattered.

**Where an engine add goes.** The book is not the bottleneck, and neither is the queue:

| component | P50 |
|---|---:|
| `OrderBook::addOrder` | 67 ns |
| publish one event (construct in variant + ring push; 2026-09-11) | 77 ns |

Matching accounts for most of the balance: `tryMatch` performs a modify-shaped book mutation
even when nothing crosses, and that is now the largest single component of an engine add.

Full methodology, caveats, and the Google Benchmark suites: [`bench/README.md`](bench/README.md).

## Quick start

### C++

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # 8 suites
./build/me_main                              # demo; writes logs/matching_engine.log
```

Benchmarks:

```bash
taskset -c 2,3 ./build/bench/bench_latency 300000   # two cores: it is producer + consumer
./build/bench/bench_add --benchmark_min_time=0.5s
```

### Python

```bash
pip install .
```

```python
from matching_engine import MatchingEngine, OrderSide, RiskParams

with MatchingEngine("logs/session.log", RiskParams(max_allowed_quantity_quote=50_000)) as engine:
    buy = engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)
    engine.add_order(price=1005, quantity=10, side=OrderSide.SELL)   # crosses, trades

    bids, asks = engine.book(num_levels=5)

    rejected = engine.add_order(price=1005, quantity=0, side=OrderSide.BUY)
    assert rejected is None      # risk rejected; the reason is in the log
```

The engine owns a logger thread, so use it as a context manager or call `close()`. The log
file is complete only once `close()` returns.

```bash
pip install '.[test]' && pytest tests/python
```

## Architecture

```
add_order ──> RiskManager ──> OrderBook ──> Matcher ──> TradeEvents
                  │              │                          │
              reject             └── OrderBookSide<BUY|SELL>
                  │                       └── BookLevel ──> OrderManager
                  ▼                                              (owns Order)
              EventManager ──> RingBuffer<128> ──> Logger thread ──> log file
```

The matching path is single-threaded and deliberately so — determinism matters more than
parallelism for a single symbol. The only thing shared across threads is the event queue:
the engine pushes, the logger drains and does all the file I/O, so no write ever lands on
the hot path. That handoff is exactly one producer and one consumer, which is what lets it
be a lock-free SPSC ring rather than a general-purpose queue.

| type | role |
|---|---|
| `MatchingEngine` | facade — risk checks, event publishing, matching |
| `OrderBook` | both sides plus the order registry |
| `OrderBookSide<Side>` | price-ordered levels; comparator chosen at compile time from the side |
| `BookLevel` | one price, FIFO queue of order ids |
| `OrderManager` | owns every live `Order` in a pre-allocated slab; mints generation-tagged ids |
| `Matcher` | crosses an incoming order against the opposite side |
| `RiskManager` | pre-trade fat-finger checks |
| `EventVariant` | the event itself — a trivially copyable `std::variant` carried by value |
| `RingBuffer<T, N>` | bounded lock-free SPSC ring; the engine/logger handoff |
| `LockQueue<T>` | two-mutex queue; still used by the tests and the Python binding |
| `Logger` | drains the queue on its own thread |

## Design notes

**One lookup per operation.** `OrderView` is an immutable snapshot of an order; the
per-field accessors on `OrderManager` were removed so the one-lookup-per-field pattern
cannot creep back in.

**Dead orders vanish.** An order's slot is released by `OrderManager` the moment it is
cancelled or filled, so the absence of a view *is* the signal that it is gone. There is no
terminal state to query — the event log is the record of what happened.

**Order ids are opaque, not sequential.** `OrderManager` keeps orders in a pre-allocated slab
and an id is `generation << 32 | slot index`, so a lookup is an array index rather than a
hash. Freed slots are reused, and the per-slot generation is what stops a late or duplicate
cancel from hitting whichever order now occupies the slot. Ids are 64-bit and never 0;
arrival order lives in each `BookLevel`'s queue, not in the id. Storing an id in a 32-bit
integer silently drops the generation and makes every lookup miss. Details and
measurements: [`docs/optimization#3.md`](docs/optimization%233.md).

**A modify may not preserve the order id.** Shrinking at the same price keeps the id and the
queue position; a reprice, a quantity increase, or a side flip retires the order and books a
replacement under a fresh id. `modifyOrder` returns the surviving id, so callers must use the
returned value rather than assume.

**The ring buffer has no shared counter.** The obvious SPSC design keeps an atomic size
that both threads mutate, but that makes every push and pop a read-modify-write on one
cache line two cores are fighting over — which is the contention a lock-free queue exists
to avoid. `RingBuffer` instead keeps monotonically increasing `_head` and `_tail`, each
written by exactly one thread, so a release store and an acquire load carry all the
ordering and no RMW is needed anywhere. Each thread also caches the other's index and only
refreshes it when the ring looks full (producer) or empty (consumer), so in steady state
neither core touches the other's line. Capacity must be a power of two: indices are masked
rather than divided, and it keeps the counters exact if `size_t` ever wraps.

**Events are values, not pointers.** The queue used to carry `unique_ptr<Event>` through a
polymorphic hierarchy, which meant a `new` on the matching thread and a `delete` on the
logger thread — a cross-thread free, the slow path in every general allocator, paid on both
cores. Events are now flat structs in a `std::variant` carried by value, so a slot write is
a 48-byte memcpy and nothing allocates. Three things had to change together for that to
hold, and a `static_assert` on `is_trivially_copyable_v<EventVariant>` guards all of them:
the virtual `push_to_file` had to go (a vptr per slot, and a non-trivial variant), the
`std::string reason` on the reject events became a borrowed `const char*`, and the log
formatting moved to the logger (`event_handler/EventFormatter.h`) where `std::visit`
dispatches it. The variant's active alternative is the type tag, so the old `event_type`
member is gone; `event_type()` recovers it. `std::monostate` is not used as the shutdown
sentinel — a `Shutdown` alternative is, so that every alternative has a `kType` and the
sentinel is still just a default-constructed `T{}`.

**Timestamps are taken on the matching thread, deliberately.** Each event stamps
`system_clock::now()` at construction — a vDSO `clock_gettime`, tens of nanoseconds. Reading
a raw cycle counter instead and converting on the logger thread would shave that off the hot
path; it was tried and reverted, because it bought a calibration step, clock-drift caveats
and a platform-specific header in exchange for a cost that is small next to the allocation
that was actually removed. Ordering in the log comes from the queue — FIFO, single producer
— not from comparing timestamps, so the stamp only has to say roughly when something
happened.

**The Python binding owns the wiring.** `MatchingEngine`'s constructor takes an event queue
that nothing drains unless a `Logger` runs alongside it, and the engine must be destroyed
*before* the logger is stopped or the final `SESSION_CLOSE` never reaches the file. Rather
than expose that to Python, the binding wraps queue, logger, thread and engine in an RAII
facade, and handles GIL release around the thread join, fork safety, and interpreter-teardown
cleanup.

## Limitations

Honest list, since some of these look like features from the outside:

- **Single symbol.** No multi-instrument support.
- **Market orders are not fully implemented** — a `MARKET` order is booked at the price passed
  in and only changes which event is emitted. It does not sweep the book at any price.
- **No IOC, FOK, stop, or iceberg orders**, and no self-trade prevention.
- **The event queue is SPSC only.** `RingBuffer` assumes exactly one producer thread and
  one consumer thread; two producers will both observe free space and write the same slot.
  It is not a general-purpose queue.
- **The event queue is bounded.** If the logger stalls on disk, the matching thread spins
  in `push()` rather than allocating. That is backpressure instead of unbounded memory
  growth, but it does mean file I/O can now stall the producer — which `LockQueue` never
  did. 128 slots absorbs the burst from one sweeping order, not a long flush. Slots hold a
  48-byte `EventVariant` by value now rather than an 8-byte pointer, so the ring is 6 KiB
  rather than 1 KiB — still L1-resident, but it is the whole event, not a pointer to it.
- **Reject reasons are borrowed, not owned.** `OrderRejected::reason` is a `const char*`,
  so every producer must pass a string literal or other static-storage text. Pointing it at
  a local buffer is a dangling read on the logger thread. This is what keeps the event
  types trivially copyable.
- **No order book snapshot or recovery** — state lives in memory for the life of the process.
- Risk checks are fat-finger limits only: max/min quantity and distance from top of book.

## Layout

```
include/me/              engine headers (templated: MatchingEngine, Matcher, EventManager)
include/risk_manager/    RiskManager, RiskParams
include/data_structures/ LockQueue, RingBuffer (lock-free SPSC)
include/event_handler/   Logger, EventQueue.h (binds the ring buffer capacity)
src/me/                  engine sources
src/python/              pybind11 module
src/main.cpp             demo driver
python/matching_engine/  Python package
tests/                   8 GoogleTest suites
tests/python/            pytest suite for the bindings
bench/                   Google Benchmark suites + latency harness
scripts/                 LOBSTER data preparation
```

Build options — all default to the historical build, so a plain configure is unchanged:

| option | default | effect |
|---|---|---|
| `ME_BUILD_TESTS` | ON | GoogleTest suites (fetches googletest) |
| `ME_BUILD_BENCH` | ON | benchmarks (fetches Google Benchmark) |
| `ME_BUILD_APPS` | ON | `me_main` demo |
| `ME_BUILD_PYTHON` | OFF | pybind11 extension |
| `ME_NATIVE_ARCH` | ON | `-march=native` in Release |

`pip install .` turns tests, benchmarks and apps off, so a wheel build clones neither
dependency.

Requires CMake 3.20+, a C++20 compiler (tested on g++ 11.4), and Python 3.9+ for the bindings.

## Next

- A C++ feed handler replaying LOBSTER market data through the engine.
- A Python alpha simulator on top of it.
- Moving the Python binding off `LockQueue`, which needs the SPSC contract argued for a
  facade whose engine can be driven from different OS threads across calls.
