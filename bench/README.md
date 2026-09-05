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
| Date | 2026-08-19 |
| Commit | `bb59657` plus the lock-free event queue change |
| CPU | 12th Gen Intel Core i7-1250U (12 threads) |
| Compiler | g++ 11.4.0 |
| Build | Release — `-O3 -march=native -DNDEBUG` |
| Samples | 300,000 per operation |
| Timer overhead | ~27 ns per `steady_clock::now()` pair — subtract from every figure |

CPU frequency scaling was **not** pinned, so absolute numbers move a few percent
run to run. P50s were stable across three runs (±5%); tails were not. The figures
below come from a single run (timer overhead 27 ns) so every row shares machine
conditions; where a claim depends on the difference between two rows it was
checked across four runs and that is called out.

`me_core` gained `POSITION_INDEPENDENT_CODE` when the Python bindings landed, since
a static library cannot otherwise link into a shared module. A/B'd against a
non-PIC build in the same session: PIC was equal or faster on every P50 (add 87 vs
114, modify 224 vs 224, cancel 142 vs 145), i.e. the difference is inside run-to-run
noise. The figures below stand.

## Per-operation latency (`bench_latency`)

All values in nanoseconds.

| operation | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| OrderBook add | 82 | 188 | 1,382 | 118 |
| OrderBook modify | 217 | 289 | 574 | 241 |
| OrderBook cancel | 148 | 251 | 459 | 189 |
| Engine add `[LockQueue]` | 699 | 5,987 | 65,548 | 1,472 |
| Engine add `[RingBuf/128]` | 768 | 2,372 | 52,823 | 1,110 |
| Engine modify `[LockQueue]` | 1,755 | 22,523 | 100,405 | 2,811 |
| Engine modify `[RingBuf/128]` | 827 | 2,359 | 22,365 | 1,122 |
| Engine cancel `[LockQueue]` | 750 | 5,893 | 98,294 | 1,554 |
| Engine cancel `[RingBuf/128]` | 452 | 1,482 | 10,276 | 642 |

`OrderBook` rows measure the book in isolation. `Engine` rows are the same
operation through `MatchingEngine`, which adds risk checks, event publishing and
matching. Engine benchmarks run with a background thread draining the event
queue and discarding — the Logger's role without its file I/O, so the figure is
the producer-side cost on the hot path.

`RingBuf/128` is the production configuration (`kEventQueueCapacity` in
`include/event_handler/EventQueue.h`); `LockQueue` is kept as the baseline the
change is measured against. `bench_latency` builds both, so the comparison is one
run rather than two builds.

**Read the tails, not the P50.** P99 and P99.9 improve on every operation, which
is the mutex contention going away. At P50 only `modify` and `cancel` improve
consistently; across four runs `Engine add` P50 landed on both sides of the
`LockQueue` figure (430/764/699/750 vs 354/749/768/701), so treat add's P50 as
unchanged.

## Where the Engine overhead goes

Measured with the same payload as the engine publishes
(`make_unique<TradeEvent>` + one push, consumer draining), 500k samples, median
of three runs:

| component | P50 | P99 | P99.9 |
|---|---:|---:|---:|
| `LockQueue::push` (one event), consumer draining | 217 ns | 2,557 ns | 10,956 ns |
| `RingBuffer<128>::push` (one event), consumer draining | 96 ns | 755 ns | 864 ns |
| — of which `make_unique<TradeEvent>` alone | 44 ns | 69 ns | — |
| `RiskManager::runAllChecks` | 28 ns | | |

- **Publishing is no longer the dominant cost.** 217 ns -> 96 ns at P50, and
  10,956 ns -> 864 ns at P99.9. `LockQueue::push` did two heap allocations
  (`make_unique<Event>`, then `make_shared` inside `push`) and took a mutex that
  `wait_and_pop` re-acquires on every predicate re-check, so producer and
  consumer contended for the same lock — that was the source of the tens-of-µs
  tails. The ring buffer does one release store and no allocation of its own.
- **What is left is the allocation.** 44 of the remaining 96 ns is
  `make_unique<TradeEvent>`. Pre-allocated POD events constructed in place in the
  ring would remove it; that is the next piece of work on this path.
- **Risk checks are effectively free.** 28 ns is within timer noise.
- **Matching costs a second book mutation.** `tryMatch` runs `getOrderView` +
  `fillOrders` + `modifyOrder` even when nothing crosses, so
  Engine add ~= book add (82) + event (96) + a modify-shaped operation (~280),
  and matching is now the largest single component.

## Ring buffer capacity: a negative result

`bench_latency` sweeps the ring at 128 / 1K / 8K / 64K slots. **Capacity makes no
measurable difference.** Across repeated runs the spread between 128 and 65,536
stays inside run-to-run noise at every percentile with no consistent ordering,
and a control run with a **2-slot** ring matched 65,536:

| Engine add | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| `RingBuf/2` | 771 | 1,623 | 30,290 | 966 |
| `RingBuf/128` | 770 | 2,124 | 25,193 | 1,019 |
| `RingBuf/1K` | 771 | 2,111 | 19,319 | 1,005 |
| `RingBuf/8K` | 812 | 2,388 | 39,909 | 1,121 |
| `RingBuf/64K` | 732 | 2,110 | 18,436 | 976 |

That is not surprising once you look at the drain thread: it discards events and
does no I/O, so it never falls behind, the ring sits near-empty, and `push()`
essentially never spins. Capacity only buys anything while the consumer is
stalled — which is exactly what this harness excludes by design, since it
measures producer-side cost.

So the win over `LockQueue` is the lock-free protocol itself, not queue depth.
Sizing the production queue is a question about the logger's worst-case I/O
pause, and answering it needs a harness whose consumer actually blocks on a
file. 128 was chosen to absorb the burst a single sweeping order emits, not
measured against a stall.

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
