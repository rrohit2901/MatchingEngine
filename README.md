# Matching Engine

![CI](https://github.com/rrohit2901/MatchingEngine/actions/workflows/CI.yml/badge.svg)

A single-symbol limit order book matching engine in C++20, with price-time priority,
pre-trade risk checks, an off-hot-path event log, and Python bindings.

Built to find out where the time actually goes in an order path — so it is benchmarked
per operation at P50 and P99 rather than in aggregate, and the numbers below include the
ones that are unflattering.

## Performance

300k samples per operation, `-O3 -march=native` with LTO, on a 12th-gen i7-1250U (WSL2),
pinned to the two hyperthreads of one core (`taskset -c 6,7`). Median of three runs, taken
interleaved with the previous `main` in one session. Every figure includes one
`steady_clock::now()` pair — subtract it. Deeper tail percentiles, and why they are not
trustworthy on this machine, are in [`bench/README.md`](bench/README.md).

| operation | P50 | P99 |
|---|---:|---:|
| OrderBook add | 58 ns | 95 ns |
| OrderBook modify | 91 ns | 131 ns |
| OrderBook cancel | 68 ns | 102 ns |
| Engine add | 169 ns | 281 ns |
| Engine modify | 278 ns | 444 ns |
| Engine cancel | 117 ns | 294 ns |

`OrderBook` rows are the book in isolation. `Engine` rows add risk checks, matching, and
event publishing.

**These are warm-machine numbers.** This laptop CPU throttles under sustained load (timer
overhead was 19–42 ns across these runs), and on a cool, idle machine the same binary reads
noticeably lower. Before LTO, a cool run measured the book rows up to 1.8x below the
throttled median ([`bench/README.md`](bench/README.md)). The timer overhead printed by
`bench_latency` is the quickest way to tell which state a run was taken in. Engine P50s also
swing by ~100 ns from run to run within a state, so compare builds by running them
interleaved, never one after the other.

**Link-time optimization, and a matcher that skips work it doesn't need.** The order path is
split across `OrderManager.cpp`, `BookLevel.cpp` and `OrderBook.cpp`. Without LTO, the small
helpers called several times per operation were out-of-line calls; Release builds now use
LTO (`ME_LTO`, on by default). `Matcher::tryMatch` also used to rewrite every incoming order
with a full `modifyOrder`, even when nothing could trade. It now returns early when the
opposite best doesn't cross, and reuses its trade buffer. Against the previous `main`,
interleaved, median of three:

| operation | P50 | P99 |
|---|---:|---:|
| OrderBook modify | 114 → 91 ns | 166 → 131 ns |
| OrderBook cancel | 85 → 68 ns | 137 → 102 ns |
| Engine add | 278 → 169 ns | 536 → 281 ns |
| Engine modify | 531 → 278 ns | 914 → 444 ns |
| Engine cancel | 250 → 117 ns | 416 → 294 ns |

Nearly all of this is LTO. The matcher change could not be separated from noise in this
session, and no benchmark yet exercises orders that cross. Details and the per-change
breakdown: [`docs/optimization#4.md`](docs/optimization%234.md).

**Orders live in a slab, not a hash map** (previous change, 2026-10-02, measured before
LTO). `OrderManager` used to be an
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

**Where an engine add goes.** An engine add is the book add, the risk check, one event
publish, and `tryMatch`. For a passive order, `tryMatch` is now one lookup and a
best-price comparison:

| component | P50 |
|---|---:|
| `OrderBook::addOrder` | 58 ns |
| publish one event (construct in variant + ring push; measured 2026-09-11, before LTO) | 77 ns |

Until the matcher change, `tryMatch` rewrote every incoming order with a full
`modifyOrder`, even when nothing crossed, and that was the largest single component of an
engine add.

Full methodology, caveats, and the Google Benchmark suites: [`bench/README.md`](bench/README.md).

## Quick start

### C++

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # 10 suites
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
- **No IOC, FOK, stop, or iceberg orders**, and no self-trade prevention in the engine
  itself. The backtest simulator has opt-in self-trade prevention.
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
include/replay/          MBO replay, mbp-1 validation, strategy Simulator
src/replay/              replay sources
src/me/                  engine sources
src/python/              pybind11 module
src/main.cpp             demo driver
python/matching_engine/  Python package (bindings, replay loaders, backtest API, me-backtest CLI)
strategies/              example strategy (ob_alpha.py) and its config
deploy/                  VM provisioning, release installer, CI deploy entry points (docs/deploy.md)
webapp/                  Streamlit UI and the sandboxed backtest runner
docs/                    backtesting and deployment guides, replay and UI plans, optimization notes
tests/                   10 GoogleTest suites
tests/python/            pytest suites: bindings, replay, backtest/CLI, web runner, web UI
bench/                   Google Benchmark suites + latency harness
scripts/                 Databento market data download, conversion, inspection, validation
```

Build options. A plain configure builds everything except the Python extension, with LTO and
`-march=native` in Release:

| option | default | effect |
|---|---|---|
| `ME_BUILD_TESTS` | ON | GoogleTest suites (fetches googletest) |
| `ME_BUILD_BENCH` | ON | benchmarks (fetches Google Benchmark) |
| `ME_BUILD_APPS` | ON | `me_main` demo |
| `ME_BUILD_PYTHON` | OFF | pybind11 extension |
| `ME_NATIVE_ARCH` | ON | `-march=native` in Release |
| `ME_LTO` | ON | link-time optimization in Release; skipped with a status message if the toolchain can't do it |

`pip install .` turns tests, benchmarks and apps off, so a wheel build clones neither
dependency.

Requires CMake 3.20+, a C++20 compiler (tested on g++ 11.4), and Python 3.9+ for the bindings.

## Market data

Replay data is one trading day of Nasdaq TotalView-ITCH order-level events (`XNAS.ITCH`,
MBO schema) from [Databento](https://databento.com), plus Nasdaq's own top of book
(`mbp-1`) to validate the rebuilt book. It is downloaded once and kept under `data/`,
which is git-ignored.

```bash
pip install '.[data]'
export DATABENTO_API_KEY=...
python3 scripts/fetch_databento.py --date 2026-09-29          # quotes the cost, asks before downloading
python3 scripts/convert_mbo.py --date 2026-09-29              # raw DBN -> Parquet, prices in 1e-4 $ ticks
python3 scripts/inspect_mbo.py --date 2026-09-29 --symbol AAPL  # checks the MBO semantics the replay relies on
python3 scripts/validate_replay.py --date 2026-09-29 --symbols AAPL NVDA TSLA  # replay vs Nasdaq's top of book
```

The rebuilt book matches Nasdaq's top of book after every event: 2,846,628 comparisons
across the three symbols, with no mismatches.

## Backtesting

`me-backtest` replays one symbol-day and lets a Python strategy trade into the real book:
its orders queue behind real orders, get filled when real executions reach them, take real
liquidity when they cross, and reach the exchange after a configurable latency.

```bash
pip install '.[backtest]'
me-backtest run --config strategies/ob_alpha.toml                  # example: quotes the top 5 levels
me-backtest run --strategy my_strategy.py --date 2026-09-29 --symbol NVDA --order-latency-us 100
```

```python
from matching_engine.backtest import Strategy

class JoinTheBid(Strategy):
    def on_timer(self, ctx):                       # every 10 ms of exchange time
        if ctx.best_bid and not ctx.open_orders and ctx.position < 1_000:
            ctx.buy(ctx.best_bid.price, 100)
```

A full session (about 2.3 M strategy calls) runs in about 15 s.

There is also a web UI, live at https://52-65-150-242.sslip.io; to run it yourself: `pip install '.[web]'` and `streamlit run webapp/app.py`. It has every
setting in a sidebar, a strategy editor, and charts of the result. The guide (strategy API,
config, the fill model and its limits) is [`docs/backtesting.md`](docs/backtesting.md);
how it was built and validated is in [`docs/strategy-replay-plan.md`](docs/strategy-replay-plan.md).
How the site is deployed, operated and taken down: [`docs/deploy.md`](docs/deploy.md).
Every design decision, with who made it: [`docs/decisions.md`](docs/decisions.md).

## Next

- Replay throughput (0.7–1.2 M records/s without a strategy): the venue id map allocates a
  node per add.
- Moving the Python binding off `LockQueue`, which needs the SPSC contract argued for a
  facade whose engine can be driven from different OS threads across calls.
