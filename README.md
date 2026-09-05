# Matching Engine

![CI](https://github.com/rrohit2901/MatchingEngine/actions/workflows/CI.yml/badge.svg)

A single-symbol limit order book matching engine in C++20, with price-time priority,
pre-trade risk checks, an off-hot-path event log, and Python bindings.

Built to find out where the time actually goes in an order path — so it is benchmarked
per operation at P50/P99/P99.9 rather than in aggregate, and the numbers below include
the ones that are unflattering.

## Performance

P50 latency, 300k samples per operation, `-O3 -march=native`, on a 12th-gen i7-1250U.
Every figure includes one `steady_clock::now()` pair (~27 ns) — subtract it.

| operation | P50 | P99 | P99.9 |
|---|---:|---:|---:|
| OrderBook add | 82 ns | 188 ns | 1,382 ns |
| OrderBook modify | 217 ns | 289 ns | 574 ns |
| OrderBook cancel | 148 ns | 251 ns | 459 ns |
| Engine add | 768 ns | 2,372 ns | 52,823 ns |
| Engine modify | 827 ns | 2,359 ns | 22,365 ns |
| Engine cancel | 452 ns | 1,482 ns | 10,276 ns |

`OrderBook` rows are the book in isolation. `Engine` rows add risk checks, matching, and
event publishing.

**The event queue is now lock-free.** The engine/logger handoff used to be the two-mutex
`LockQueue`; it is now a bounded SPSC ring buffer, `RingBuffer<T, 128>`. Both measured in
the same run of the same build:

| Engine operation | P50 (LockQueue → ring) | P99 (LockQueue → ring) | P99.9 (LockQueue → ring) |
|---|---:|---:|---:|
| add | 699 → 768 ns | 5,987 → 2,372 ns | 65,548 → 52,823 ns |
| modify | 1,755 → 827 ns | 22,523 → 2,359 ns | 100,405 → 22,365 ns |
| cancel | 750 → 452 ns | 5,893 → 1,482 ns | 98,294 → 10,276 ns |

The tail is where it shows, because publishing no longer contends with the draining
thread for a mutex. At P50 `modify` and `cancel` roughly halve. `add` is a wash — across
four runs it landed either side of the old figure, since a single add publishes one event
and the queue is a smaller share of its cost.

**Where the 768 ns goes.** The book is not the bottleneck, and now neither is the queue:

| component | P50 |
|---|---:|
| `RingBuffer<128>::push` (one event, consumer draining) | 96 ns |
| — of which `make_unique<TradeEvent>` | 44 ns |
| `RiskManager` checks | 28 ns |
| `OrderBook::addOrder` | 82 ns |

Publishing fell from 217 ns to 96 ns (both re-measured in this session; the 266 ns
quoted here previously was an earlier run), and roughly half of what is left is the event
allocation rather than the queue itself. Removing that means pre-allocated POD events
constructed in place in the ring, which is the next piece of work. Matching accounts for
most of the balance: `tryMatch` performs a modify-shaped book mutation even when nothing
crosses.

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
./build/bench/bench_latency 300000
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
| `OrderManager` | owns every live `Order`; mints ids |
| `Matcher` | crosses an incoming order against the opposite side |
| `RiskManager` | pre-trade fat-finger checks |
| `RingBuffer<T, N>` | bounded lock-free SPSC ring; the engine/logger handoff |
| `LockQueue<T>` | two-mutex queue; still used by the tests and the Python binding |
| `Logger` | drains the queue on its own thread |

## Design notes

**One lookup per operation.** `OrderView` is an immutable snapshot of an order; the
per-field accessors on `OrderManager` were removed so the one-lookup-per-field pattern
cannot creep back in.

**Dead orders vanish.** An order is erased from `OrderManager` the moment it is cancelled or
filled, so the absence of a view *is* the signal that it is gone. There is no terminal state
to query — the event log is the record of what happened.

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
  did. 128 slots absorbs the burst from one sweeping order, not a long flush.
- **Events are still heap-allocated.** `make_unique<Event>` per publish is ~44 ns and is
  now about half the remaining publish cost (see above).
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
- Pre-allocated POD events constructed in place in the ring, removing the per-publish
  `make_unique` that is now half the publish cost.
- Moving the Python binding off `LockQueue`, which needs the SPSC contract argued for a
  facade whose engine can be driven from different OS threads across calls.
