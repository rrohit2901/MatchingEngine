# Benchmark reference numbers

Baseline figures to compare future changes against. Re-run and update this file
whenever the hot path changes.

## How these were produced

```
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j

# two cores, not one -- see the caveats below. Any hyperthread sibling pair
# works (2,3 and 6,7 are both one core's two threads under WSL2).
taskset -c 6,7 ./build-release/bench/bench_latency 300000

./build-release/bench/bench_add     --benchmark_min_time=0.5s
./build-release/bench/bench_cancel  --benchmark_min_time=0.5s
./build-release/bench/bench_modify  --benchmark_min_time=0.5s
```

| | |
|---|---|
| Date | 2026-10-03 |
| Commit | `68b2746`: LTO + `tryMatch` fast path ([optimization #4](../docs/optimization%234.md)) |
| CPU | 12th Gen Intel Core i7-1250U (12 threads) |
| Compiler | g++ 11.4.0 |
| Build | Release — `-O3 -march=native -DNDEBUG`, LTO (`ME_LTO=ON`, the default) |
| Samples | 300,000 per operation |
| Timer overhead | 19–42 ns per `steady_clock::now()` pair across these runs (warm machine) — subtract from every figure |
| Runs | median of 3, interleaved with the previous `main` and an LTO-only build (see below) |

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

**How the current figures were taken (2026-10-03).** Three builds were run
**interleaved**, three rounds of `main` (`8454e53`) → LTO only (`eb5fc46`) →
this commit, so all three see the same thermal drift. Every figure below is the
median of that build's three runs. A 5-minute idle beforehand did not produce a
cool run: the first run still read 38 ns timer overhead. So unlike the previous
update there is no cool-machine table for this build, and the figures are
warm-machine figures. The three-way comparison is in
[`docs/optimization#4.md`](../docs/optimization%234.md).

(Earlier updates used the same interleaving for the slab change, 2026-10-02,
and a single run with the `OrderBook` rows as a control for the allocation-free
event queue, which did not touch the book.)

Two cores, not one: the engine rows run a producer thread and a draining consumer
thread, so pinning to a single core makes `RingBuffer::push` spin against a
consumer that cannot be scheduled, and P99.9 blows up to a scheduler quantum
(~40 ms). That is a measurement artifact, not a queue property.

**P99.9 is not trustworthy on this machine.** The `max` column runs to several
milliseconds under WSL2, so the top 0.1% is measuring scheduler preemption rather
than the code. It is kept in the tables below for completeness and is deliberately
absent from the top-level README. Read P50, and P99 with care.

That floor was measured directly on 2026-10-02: a loop timing nothing but two
back-to-back `steady_clock::now()` calls, 1M samples, has a max of 20–66 µs
on this machine. The `OrderBook` rows' millisecond maxes sit on top of that
floor. They also include page faults on fresh `BookLevel` vectors after each
untimed book rebuild, and in `OrderBook add`, one `OrderManager` slab doubling
per 100k-order cycle (65,536 slots by default). None of it is the event queue:
the `OrderBook` rows have no consumer, and the `Engine` rows' consumer does no
I/O. Details in [`docs/optimization#3.md`](../docs/optimization%233.md) §5.

`me_core` gained `POSITION_INDEPENDENT_CODE` when the Python bindings landed, since
a static library cannot otherwise link into a shared module. A/B'd against a
non-PIC build in the same session: PIC was equal or faster on every P50 (add 87 vs
114, modify 224 vs 224, cancel 142 vs 145), i.e. the difference is inside run-to-run
noise. The figures below stand.

## Per-operation latency (`bench_latency`)

All values in nanoseconds.

### Cool vs warm, previous build (2026-10-02, before LTO)

Kept because it is the only cool-machine measurement there is, and it shows how
large the thermal effect is on this laptop. **These are not the current
binary's numbers**; see the next table for those.

Single cool run, idle beforehand, timer overhead 20 ns, `taskset -c 6,7`:

| operation | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| OrderBook add | 39 | 148 | 508 | 72 |
| OrderBook modify | 98 | 142 | 225 | 103 |
| OrderBook cancel | 64 | 85 | 133 | 65 |
| Engine add `[LockQueue]` | 317 | 2,400 | 11,440 | 572 |
| Engine add `[RingBuf/128]` | 201 | 269 | 674 | 214 |
| Engine modify `[LockQueue]` | 615 | 4,445 | 29,037 | 1,131 |
| Engine modify `[RingBuf/128]` | 528 | 893 | 1,174 | 554 |
| Engine cancel `[LockQueue]` | 429 | 3,655 | 21,092 | 771 |
| Engine cancel `[RingBuf/128]` | 247 | 409 | 506 | 275 |

Three back-to-back runs started from idle show the drift directly. Timer
overhead went 24 → 27 → 28 ns, `OrderBook add` P50 went 45 → 49 → 49, and
`OrderBook modify` P50 went 106 → 132 → 132. After ~40 minutes of continuous
load, that build settled at OrderBook add/modify/cancel P50 67/179/116 and
Engine add/modify/cancel 398/521/238 (median of 3, timer overhead ~36 ns).
Engine modify barely moved between states (528 cool vs 521 warm) and varied
262–555 across those three runs, so that row is mostly run-to-run noise.

### Current (2026-10-03, LTO + fast path, median of 3, warm, `taskset -c 6,7`)

| operation | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| OrderBook add | 58 | 95 | 453 | 79 |
| OrderBook modify | 91 | 131 | 184 | 96 |
| OrderBook cancel | 68 | 102 | 120 | 75 |
| Engine add `[LockQueue]` | 427 | 3,869 | 22,863 | 856 |
| Engine add `[RingBuf/128]` | 169 | 281 | 895 | 179 |
| Engine modify `[LockQueue]` | 429 | 3,960 | 22,864 | 900 |
| Engine modify `[RingBuf/128]` | 278 | 444 | 683 | 310 |
| Engine cancel `[LockQueue]` | 402 | 3,256 | 19,122 | 750 |
| Engine cancel `[RingBuf/128]` | 117 | 294 | 394 | 187 |

Against the previous `main`, interleaved in the same session: OrderBook modify
114 → 91 and cancel 85 → 68 at P50, Engine add 278 → 169, modify 531 → 278 and
cancel 250 → 117. OrderBook add did not move (50 → 58 at P50, 126 → 95 at P99).
Nearly all of the gain is LTO. The `tryMatch` fast path could not be separated
from noise in this session, and none of these benchmarks has an order that
crosses.

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
  (40 bytes then; 48 since order ids widened to 64 bits), so the slot write is a
  memcpy and nothing allocates.
- **The tail moved more than the median.** P99 fell 742 -> 124 ns and P99.9
  2,892 -> 641 ns, against 111 -> 77 ns at P50. That ratio is the signature of an
  allocator leaving the path: the median cost of `malloc` on a hot free list is
  modest, but its tail — a refill, a slow-path arena lock — is not.
- **Matching no longer costs a second book mutation.** Until optimization #4,
  `tryMatch` ran `getOrderView` + `fillOrders` + a full `modifyOrder` even when
  nothing crossed, and that was the largest single component of an engine add.
  It now returns after one lookup and a best-price comparison when the opposite
  side doesn't cross. Engine add ≈ book add (58) + publish (77, measured before
  LTO and not re-measured) + risk check + that early exit.

Not re-measured for this change: `RiskManager::runAllChecks` was previously timed
at 28 ns, which was inside timer noise then and has not been touched since.

## Ring buffer capacity: a negative result

`bench_latency` sweeps the ring at 128 / 1K / 8K / 64K slots. **Capacity makes no
measurable difference.** Across repeated runs the spread between 128 and 65,536
stays inside run-to-run noise at every percentile with no consistent ordering,
and a control run with a **2-slot** ring matched 65,536:

| Engine add | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| `RingBuf/128` | 169 | 281 | 895 | 179 |
| `RingBuf/1K` | 161 | 238 | 634 | 185 |
| `RingBuf/8K` | 172 | 298 | 750 | 209 |
| `RingBuf/64K` | 167 | 312 | 829 | 184 |

(Same runs as the table above, median of three. The `RingBuf/2` control row was
from the earlier `unique_ptr` build and is not re-measured here. The conclusion
is unchanged: the P50 spread across a 512x range of capacities is about 7%.)

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

Mean ns/op, `--benchmark_min_time=0.5s`. Range across two runs, interleaved with
the previous `main`'s build:

| benchmark | previous `main` | current |
|---|---:|---:|
| `BM_Add_NewPriceLevel` | 1,555–1,611 | 2,240–2,287 |
| `BM_Add_ExistingPriceLevel` | 80–86 | 85–138 |
| `BM_Add_SpreadAcrossLevels` | 103–114 | 74–78 |
| `BM_Add_MarketOrder` | 83 | 54–58 |
| `BM_Cancel_LastOrderOnLevel` | 465–536 | 413 |
| `BM_Cancel_OneOfManyOnLevel` | 462–473 | 396–407 |
| `BM_Cancel_FromDeepBook` | 502–516 | 389–416 |
| `BM_Modify_QuantityOnly` | 1,036–1,390 | 715–717 |
| `BM_Modify_ChangePriceLevel` | 713–929 | 600–637 |
| `BM_Modify_ChangeSide` | 807–1,013 | 621–688 |

Every cancel and modify row is lower in both runs. `BM_Add_NewPriceLevel` reads
about 40% worse; see caveat 2 below for why that benchmark can't be compared
between builds. `bench_latency` shows no add regression.

**Treat these as much softer than the `bench_latency` numbers.** Two known
methodology problems, neither fixed:

1. `bench_modify` and `bench_cancel` call `PauseTiming()`/`ResumeTiming()` every
   iteration, which costs more than the operation being measured. That is why
   cancel reads ~400 ns here and ~70 ns in `bench_latency`. Batch the setup to
   fix it.
2. `BM_Add_NewPriceLevel` and `BM_Add_ExistingPriceLevel` grow the book without
   bound while Google Benchmark chooses iteration counts adaptively, so their
   means depend on how many iterations happened to run and are not comparable
   between builds. `bench_latency` avoids this by rebuilding the book every
   `kAddResetEvery` orders.
3. `BM_Add_NewPriceLevel` with `--benchmark_repetitions` also measures glibc:
   each repetition frees the `OrderManager` slab, glibc's dynamic trim threshold
   returns it to the OS, and the next repetition page-faults it back in. It reads
   ~4x slower than without repetitions for that reason alone. Pin
   `GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072:glibc.malloc.trim_threshold=4294967296`
   to remove the effect.

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
