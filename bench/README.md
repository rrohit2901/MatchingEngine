# Benchmark reference numbers

Baseline figures to compare future changes against. Re-run and update this file
whenever the hot path changes.

## How these were produced

```
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j

# two cores, not one -- see the caveats below
taskset -c 2,3 ./build-release/bench/bench_latency 300000

./build-release/bench/bench_add     --benchmark_min_time=0.5s
./build-release/bench/bench_cancel  --benchmark_min_time=0.5s
./build-release/bench/bench_modify  --benchmark_min_time=0.5s
```

| | |
|---|---|
| Date | 2026-09-11 |
| Commit | `bd67f70` (allocation-free event queue) |
| CPU | 12th Gen Intel Core i7-1250U (12 threads) |
| Compiler | g++ 11.4.0 |
| Build | Release — `-O3 -march=native -DNDEBUG` |
| Samples | 300,000 per operation |
| Timer overhead | ~28 ns per `steady_clock::now()` pair — subtract from every figure |

### Measurement caveats, learned the hard way

**This CPU thermally throttles under sustained benchmarking, and stays throttled.**
Running `bench_latency` back to back, the first two runs reported 19-29 ns timer
overhead and `OrderBook add` P50 of 55-60 ns; runs three through six reported a
dead-flat 36 ns overhead and 110 ns P50 — nearly 2x worse on identical code, and
it did not recover. A 90-second cooldown between runs did not restore it either.
Any comparison that runs build A then build B in a fixed order will therefore
credit build B with the throttling, which is a large enough effect to invent or
erase the entire result being measured.

**Use the `OrderBook` rows as a control.** They are untouched by anything on the
event path, so if they move between two runs, the machine moved, not the code.
That check is what caught the problem: an earlier draft of this table showed
`OrderBook add` P99 going 188 -> 565 ns on code that had not been edited at all.

The figures below come from a **single run**, so every row shares machine
conditions, and that run was chosen as the one whose `OrderBook` control rows sit
closest to the pre-change baseline's (82/217/148 P50). Its P50 controls land
within 4% (84/226/151), which is what makes the P50 comparison fair. Its P99
controls are 1.2-1.7x *worse* than the baseline's, so P99 improvements shown here
are understated rather than flattered.

Two cores, not one: the engine rows run a producer thread and a draining consumer
thread, so pinning to a single core makes `RingBuffer::push` spin against a
consumer that cannot be scheduled, and P99.9 blows up to a scheduler quantum
(~40 ms). That is a measurement artifact, not a queue property.

**P99.9 is not trustworthy on this machine.** The `max` column runs to several
milliseconds under WSL2, so the top 0.1% is measuring scheduler preemption rather
than the code. It is kept in the tables below for completeness and is deliberately
absent from the top-level README. Read P50, and P99 with care.

`me_core` gained `POSITION_INDEPENDENT_CODE` when the Python bindings landed, since
a static library cannot otherwise link into a shared module. A/B'd against a
non-PIC build in the same session: PIC was equal or faster on every P50 (add 87 vs
114, modify 224 vs 224, cancel 142 vs 145), i.e. the difference is inside run-to-run
noise. The figures below stand.

## Per-operation latency (`bench_latency`)

All values in nanoseconds.

| operation | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| OrderBook add | 84 | 220 | 1,690 | 134 |
| OrderBook modify | 226 | 485 | 786 | 268 |
| OrderBook cancel | 151 | 333 | 587 | 230 |
| Engine add `[LockQueue]` | 670 | 4,361 | 55,646 | 1,232 |
| Engine add `[RingBuf/128]` | 383 | 843 | 20,548 | 713 |
| Engine modify `[LockQueue]` | 1,199 | 8,006 | 54,975 | 1,926 |
| Engine modify `[RingBuf/128]` | 557 | 1,023 | 21,355 | 732 |
| Engine cancel `[LockQueue]` | 452 | 3,079 | 27,825 | 672 |
| Engine cancel `[RingBuf/128]` | 283 | 525 | 1,204 | 353 |

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
  Engine add ~= book add (84) + publish (77) + a modify-shaped operation, and
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
| `RingBuf/128` | 383 | 843 | 20,548 | 713 |
| `RingBuf/1K` | 375 | 808 | 3,955 | 522 |
| `RingBuf/8K` | 373 | 795 | 13,481 | 490 |
| `RingBuf/64K` | 394 | 864 | 18,819 | 604 |

(Same single run as the table above. The `RingBuf/2` control row was from the
earlier `unique_ptr` build and is not re-measured here; the conclusion is
unchanged — the P50 spread across a 512x range of capacities is under 6%.)

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

Mean ns/op. **Not re-measured for the event-queue change** — these cover
`OrderBook` only, which that change does not touch, so they carry the date of the
previous run rather than the one in the header above.

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
