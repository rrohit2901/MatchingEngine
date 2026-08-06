# Benchmark reference numbers

Baseline figures to compare future changes against. Re-run and update this file
whenever the hot path changes.

## How these were produced

```
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
./build-release/bench/bench_latency 300000
./build-release/bench/bench_add     --benchmark_min_time=0.5s
./build-release/bench/bench_cancel  --benchmark_min_time=0.5s
./build-release/bench/bench_modify  --benchmark_min_time=0.5s
```

| | |
|---|---|
| Date | 2026-08-06 |
| Commit | `7265687` (plus reject-event changes) |
| CPU | 12th Gen Intel Core i7-1250U (12 threads) |
| Compiler | g++ 11.4.0 |
| Build | Release — `-O3 -march=native -DNDEBUG` |
| Samples | 300,000 per operation |
| Timer overhead | ~27 ns per `steady_clock::now()` pair — subtract from every figure |

CPU frequency scaling was **not** pinned, so absolute numbers move a few percent
run to run. P50s were stable across three runs (±5%); tails were not.

## Per-operation latency (`bench_latency`)

All values in nanoseconds.

| operation | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| OrderBook add | 87 | 273 | 1,892 | 167 |
| OrderBook modify | 221 | 498 | 807 | 291 |
| OrderBook cancel | 150 | 289 | 429 | 171 |
| Engine add | 710 | 5,177 | 42,822 | 1,367 |
| Engine modify | 1,348 | 16,336 | 75,487 | 2,224 |
| Engine cancel | 502 | 3,193 | 45,081 | 814 |

`OrderBook` rows measure the book in isolation. `Engine` rows are the same
operation through `MatchingEngine`, which adds risk checks, event publishing and
matching. Engine benchmarks run with a background thread draining the event
queue and discarding — the Logger's role without its file I/O, so the figure is
the producer-side cost on the hot path.

## Where the Engine overhead goes

Measured separately, same build:

| component | P50 |
|---|---:|
| `LockQueue::push` (one event), consumer draining | 266 ns |
| `LockQueue::push` (one event), no consumer | 206 ns |
| `RiskManager::runAllChecks` | 28 ns |

- **Event publishing dominates.** ~266 ns per operation — more than 3x an entire
  `OrderBook` add. That is `make_unique<Event>` plus `make_shared` inside
  `LockQueue::push` plus a mutex, on every order. Contention with the drain
  thread adds ~60 ns at P50 and far more in the tail: `wait_and_pop` takes
  `tail_mutex` on every predicate re-check, so consumer and producer contend for
  the same lock. That is the source of the 40-75 us P99.9 figures.
- **Risk checks are effectively free.** 28 ns is within timer noise.
- **Matching costs a second book mutation.** `tryMatch` runs `getOrderView` +
  `fillOrders` + `modifyOrder` even when nothing crosses, so
  Engine add ~= book add (87) + event (266) + a modify-shaped operation (~280).

The optimisation target, if the hot path ever needs to go below ~200 ns, is the
queue rather than the engine: pre-allocated POD events in a bounded ring buffer
would remove both allocations and the lock.

## Google Benchmark suites

Mean ns/op.

| benchmark | ns |
|---|---:|
| `BM_Add_NewPriceLevel` | 1,373 |
| `BM_Add_ExistingPriceLevel` | 155 |
| `BM_Add_SpreadAcrossLevels` | 123 |
| `BM_Add_MarketOrder` | 92 |
| `BM_Cancel_LastOrderOnLevel` | 500 |
| `BM_Cancel_OneOfManyOnLevel` | 503 |
| `BM_Cancel_FromDeepBook` | 532 |
| `BM_Modify_QuantityOnly` | 931 |
| `BM_Modify_ChangePriceLevel` | 1,026 |
| `BM_Modify_ChangeSide` | 1,033 |

**Treat these as much softer than the `bench_latency` numbers.** Two known
methodology problems, neither fixed:

1. `bench_modify` and `bench_cancel` call `PauseTiming()`/`ResumeTiming()` every
   iteration, which costs more than the operation being measured. That is why
   cancel reads ~500 ns here and 150 ns in `bench_latency`. Batch the setup to
   fix it.
2. `BM_Add_NewPriceLevel` and `BM_Add_ExistingPriceLevel` grow the book without
   bound while Google Benchmark chooses iteration counts adaptively, so their
   means depend on how many iterations happened to run and are not comparable
   between builds. `bench_latency` avoids this by rebuilding the book every
   `kAddResetEvery` orders.

`BM_Add_MarketOrder` currently measures the same path as
`BM_Add_ExistingPriceLevel`: market orders do not yet rest at a sentinel price.
It is a placeholder for when they do.

## Historical note

The `OrderView` refactor (single lookup per operation instead of one per field)
was measured against the commit before it. Engine-level figures did not exist
then; `OrderBook` P50s moved as follows:

| operation | before `OrderView` | after |
|---|---:|---:|
| add | 56-67 | 66-67 |
| modify | 173-231 | 172-174 |
| cancel | 106-123 | 115 |

Modify recovered most of a regression introduced when orders moved into a
central `OrderManager`, and became far more stable (173-231 -> 172-174). Absolute
values differ from the table above because those runs were on a quieter machine;
compare within a table, not across.
