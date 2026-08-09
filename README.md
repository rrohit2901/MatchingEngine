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
| OrderBook add | 87 ns | 273 ns | 1,892 ns |
| OrderBook modify | 221 ns | 498 ns | 807 ns |
| OrderBook cancel | 150 ns | 289 ns | 429 ns |
| Engine add | 710 ns | 5,177 ns | 42,822 ns |
| Engine modify | 1,348 ns | 16,336 ns | 75,487 ns |
| Engine cancel | 502 ns | 3,193 ns | 45,081 ns |

`OrderBook` rows are the book in isolation. `Engine` rows add risk checks, matching, and
event publishing.

**Where the 710 ns goes.** The book is not the bottleneck:

| component | P50 |
|---|---:|
| `LockQueue::push` (one event, consumer draining) | 266 ns |
| `RiskManager` checks | 28 ns |
| `OrderBook::addOrder` | 87 ns |

Event publishing costs three times an entire book insert — two heap allocations
(`make_unique<Event>`, then `make_shared` inside `push`) plus a mutex, on every order.
Producer/consumer contention on the same lock is also what produces the 40–75 µs P99.9
figures. Replacing the queue with a bounded ring buffer of pre-allocated POD events is the
next piece of work, and it is where the remaining latency is.

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
              EventManager ──> LockQueue ──> Logger thread ──> log file
```

The matching path is single-threaded and deliberately so — determinism matters more than
parallelism for a single symbol. The only thing shared across threads is the event queue:
the engine pushes, the logger drains and does all the file I/O, so no write ever lands on
the hot path.

| type | role |
|---|---|
| `MatchingEngine` | facade — risk checks, event publishing, matching |
| `OrderBook` | both sides plus the order registry |
| `OrderBookSide<Side>` | price-ordered levels; comparator chosen at compile time from the side |
| `BookLevel` | one price, FIFO queue of order ids |
| `OrderManager` | owns every live `Order`; mints ids |
| `Matcher` | crosses an incoming order against the opposite side |
| `RiskManager` | pre-trade fat-finger checks |
| `LockQueue<T>` | two-mutex queue; the engine/logger handoff |
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
- **The event queue uses a mutex** and is the dominant cost on the hot path (see above).
  Nothing in this repository is lock-free.
- **No order book snapshot or recovery** — state lives in memory for the life of the process.
- Risk checks are fat-finger limits only: max/min quantity and distance from top of book.

## Layout

```
include/me/              engine headers (templated: MatchingEngine, Matcher, EventManager)
include/risk_manager/    RiskManager, RiskParams
include/data_structures/ LockQueue
include/event_handler/   Logger
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
- Replacing the mutex event queue with a bounded SPSC ring buffer of POD events.
