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
| Date | 2026-09-11 |
| Commit | `4d06b23` plus the allocation-free event queue change |
| CPU | 12th Gen Intel Core i7-1250U (12 threads) |
| Compiler | g++ 11.4.0 |
| Build | Release — `-O3 -march=native -DNDEBUG` |
| Samples | 300,000 per operation |
| Timer overhead | ~27 ns per `steady_clock::now()` pair — subtract from every figure |

CPU frequency scaling was **not** pinned, so absolute numbers move a few percent
run to run. The figures below are the **median of four runs**, each pinned to two
cores (`taskset -c 2,3`) and each reporting 27-30 ns timer overhead, which is this
machine's quiet-state value; runs reporting more than that were discarded as
loaded and re-taken.

Two cores, not one: the engine rows run a producer thread and a draining consumer
thread, so pinning to a single core makes `RingBuffer::push` spin against a
consumer that cannot be scheduled, and P99.9 blows up to a scheduler quantum
(~40 ms). That is a measurement artifact, not a queue property.

**P99.9 is not trustworthy on this machine.** The `max` column runs to several
milliseconds under WSL2, so the top 0.1% is measuring scheduler preemption rather
than the code. Read P50, and P99 with care.

`me_core` gained `POSITION_INDEPENDENT_CODE` when the Python bindings landed, since
a static library cannot otherwise link into a shared module. A/B'd against a
non-PIC build in the same session: PIC was equal or faster on every P50 (add 87 vs
114, modify 224 vs 224, cancel 142 vs 145), i.e. the difference is inside run-to-run
noise. The figures below stand.

## Per-operation latency (`bench_latency`)

All values in nanoseconds.

| operation | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| OrderBook add | 85 | 565 | 2,289 | 191 |
| OrderBook modify | 233 | 912 | 1,908 | 357 |
| OrderBook cancel | 146 | 530 | 752 | 204 |
| Engine add `[LockQueue]` | 708 | 6,231 | 100,557 | 1,448 |
| Engine add `[RingBuf/128]` | 379 | 1,566 | 17,498 | 606 |
| Engine modify `[LockQueue]` | 1,434 | 11,664 | 141,064 | 2,291 |
| Engine modify `[RingBuf/128]` | 554 | 2,022 | 30,742 | 832 |
| Engine cancel `[LockQueue]` | 390 | 2,844 | 77,860 | 747 |
| Engine cancel `[RingBuf/128]` | 286 | 1,046 | 15,957 | 511 |

`OrderBook` rows measure the book in isolation. `Engine` rows are the same
operation through `MatchingEngine`, which adds risk checks, event publishing and
matching. Engine benchmarks run with a background thread draining the event
queue and discarding — the Logger's role without its file I/O, so the figure is
the producer-side cost on the hot path.

`RingBuf/128` is the production configuration (`kEventQueueCapacity` in
`include/event_handler/EventQueue.h`); `LockQueue` is kept as the baseline the
change is measured against. `bench_latency` builds both, so the comparison is one
run rather than two builds.

`RingBuf/128` now beats `LockQueue` at P50 on every operation, which it did not
before events became values: with `unique_ptr<Event>` the allocation dominated the
publish, so the choice of queue barely showed at the median and `Engine add` P50
landed on both sides of the `LockQueue` figure across runs. Removing the
allocation made the queue the visible cost, and the lock-free one wins.

## Where the Engine overhead goes

Measured with the same payload the engine publishes — construct one `TradeEvent`
and push it into the ring with a consumer draining — 400k samples, median of five
runs of each binary, interleaved so both see the same machine conditions:

| publish one event | P50 | P99 | P99.9 |
|---|---:|---:|---:|
| `make_unique<TradeEvent>` + `RingBuffer<128>::push` | 111 ns | 742 ns | 2,892 ns |
| construct in `EventVariant` + `RingBuffer<128>::push` | 77 ns | 124 ns | 641 ns |

- **The allocation is gone.** The queue used to carry `unique_ptr<Event>`, so every
  publish was a `new` on the matching thread and a `delete` on the logger thread.
  A cross-thread free is the slow path in every general-purpose allocator — it
  either takes the arena lock or pushes onto a remote free list — so both cores
  paid. Events are now flat structs in a trivially copyable `std::variant`
  (40 bytes), so the slot write is a memcpy and nothing allocates.
- **The tail moved more than the median.** P99 fell 742 -> 124 ns and P99.9
  2,892 -> 641 ns, against 111 -> 77 ns at P50. That ratio is the signature of an
  allocator leaving the path: the median cost of `malloc` on a hot free list is
  modest, but its tail — a refill, a slow-path arena lock — is not.
- **Matching costs a second book mutation.** `tryMatch` runs `getOrderView` +
  `fillOrders` + `modifyOrder` even when nothing crosses, so
  Engine add ~= book add (85) + publish (77) + a modify-shaped operation, and
  matching is now comfortably the largest single component.

Not re-measured for this change: `RiskManager::runAllChecks` was previously timed
at 28 ns, which was inside timer noise then and has not been touched since.

## Ring buffer capacity: a negative result

`bench_latency` sweeps the ring at 128 / 1K / 8K / 64K slots. **Capacity makes no
measurable difference.** Across repeated runs the spread between 128 and 65,536
stays inside run-to-run noise at every percentile with no consistent ordering,
and a control run with a **2-slot** ring matched 65,536:

| Engine add | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| `RingBuf/128` | 379 | 1,566 | 17,498 | 606 |
| `RingBuf/1K` | 369 | 1,924 | 9,036 | 570 |
| `RingBuf/8K` | 376 | 1,644 | 14,464 | 614 |
| `RingBuf/64K` | 378 | 1,510 | 11,608 | 583 |

(Median of four runs. The `RingBuf/2` control row was from the earlier
`unique_ptr` build and is not re-measured here; the conclusion is unchanged.)

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
