# Benchmarks

Latency benchmarking for the single-symbol `OrderBook`. Two complementary tools:

| Tool | Built from | Reports | Use for |
|------|-----------|---------|---------|
| **Google Benchmark suite** | `bench_add`, `bench_modify`, `bench_cancel` | mean time / throughput per op, across several scenarios | comparing code paths, throughput |
| **Latency harness** | `bench_latency` | per-operation **P50 / P99 / P99.9** (tail) | latency SLOs, regression tracking |

> Google Benchmark reports the *mean* over a large inner loop, which hides the tail.
> `bench_latency` times each operation individually and computes true percentiles —
> that's what the baseline below records.

## Building

Benchmarks **must** be built in **Release** (`-O3 -march=native`). A Debug build makes the
numbers meaningless (Google Benchmark even prints a `Library was built as DEBUG` warning).

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target bench_add bench_modify bench_cancel bench_latency
```

## Running

### 0. Build first (Release)

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target bench_add bench_modify bench_cancel bench_latency
```

All four executables land in `build-release/bench/`. For the steadiest numbers, pin a
performance core and prefix any command with `taskset -c 0` (see Caveats).

### 1. Latency harness — `bench_latency`  (P50 / P99 / P99.9)

```bash
./build-release/bench/bench_latency            # 1,000,000 samples/op (default)
./build-release/bench/bench_latency 2000000    # first arg overrides the sample count
taskset -c 0 ./build-release/bench/bench_latency   # pin to one core for stabler tails
```

Prints one row per operation (`add`, `modify`, `cancel`) with P50/P99/P99.9 + min/max/mean
in nanoseconds, plus the measured `steady_clock` overhead. No flags other than the sample count.

### 2. Throughput suites — `bench_add` / `bench_modify` / `bench_cancel`

These are [Google Benchmark](https://github.com/google/benchmark) binaries, so they accept all
standard `--benchmark_*` flags. Run with no flags to execute every scenario:

```bash
./build-release/bench/bench_add
./build-release/bench/bench_modify
./build-release/bench/bench_cancel
```

Scenarios per binary (use as `--benchmark_filter` substrings):

| Binary | Scenarios |
|--------|-----------|
| `bench_add`    | `NewPriceLevel`, `ExistingPriceLevel`, `SpreadAcrossLevels`, `MarketOrder` |
| `bench_modify` | `QuantityOnly`, `ChangePriceLevel`, `ChangeSide` |
| `bench_cancel` | `LastOrderOnLevel`, `OneOfManyOnLevel`, `FromDeepBook` |

```bash
# List the scenarios in a binary without running them
./build-release/bench/bench_add --benchmark_list_tests

# Run a subset (regex / substring match on the scenario name)
./build-release/bench/bench_modify --benchmark_filter=ChangeSide
./build-release/bench/bench_cancel --benchmark_filter='LastOrderOnLevel|FromDeepBook'

# Statistical run: repeat each scenario and report mean/median/stddev only
./build-release/bench/bench_add --benchmark_repetitions=10 --benchmark_report_aggregates_only=true

# Longer measurement window per scenario (more stable means)
./build-release/bench/bench_cancel --benchmark_min_time=2s

# Machine-readable output (for tracking / plotting)
./build-release/bench/bench_add --benchmark_format=json --benchmark_out=add.json
./build-release/bench/bench_add --benchmark_format=csv  > add.csv

# Full flag reference
./build-release/bench/bench_add --help
```

### 3. Run everything

```bash
for b in bench_add bench_modify bench_cancel; do
    echo "===== $b ====="; ./build-release/bench/$b
done
./build-release/bench/bench_latency
```

## Baseline (latency)

Snapshot — **2026-06-22**, commit `bf6e327` + benchmark infra.
Machine: Intel Core i7-1250U, GCC 11.4, `-O3 -march=native`, Linux (WSL2), 1,000,000 samples/op.

| Operation | P50 | P99 | P99.9 | min | mean |
|-----------|----:|----:|------:|----:|-----:|
| **add** (limit, spread over 100 levels) | 80 ns | 1131 ns | 3685 ns | 67 ns | 156 ns |
| **modify** (reprice to a new level)     | 130 ns | 247 ns | 393 ns | 118 ns | 138 ns |
| **cancel** (against a 100-level book)   | 98 ns | 254 ns | 374 ns | 87 ns | 107 ns |

Each latency includes one `steady_clock::now()` pair (~22 ns here — measured and printed at runtime).

### Reading the numbers
- **`add` has the widest tail** — its P99/P99.9 are dominated by `std::map` node allocation when a
  price level is first created, and by `std::vector` reallocation as a level grows. The P50 (the common
  append-to-existing-level path) is the cheapest of the three.
- **`modify` (reprice) and `cancel` are tight** — bounded work (cancel from old level + add to new, or a
  single tombstone + lookup), so their tails stay within ~3–4× of P50.
- **`max`** (not tabulated) regularly hits the millisecond range. Those are OS scheduling / page-fault /
  allocator outliers on an unpinned laptop, not order-book work.

## Methodology & caveats
- **Per-op timing.** Setup (resting the order a `cancel`/`modify` operates on; rebuilding a grown `add`
  book) is done **outside** the timed window via separate `now()` brackets, so only the operation under
  test is measured. A 50k-iteration warmup precedes each measurement.
- **`add` is bounded to steady state.** `BookLevel::cancelOrder` only *tombstones* an order — a level's
  vector is freed only when the level fully drains. Left unbounded, vector-doubling reallocs would
  dominate the `add` tail, so the harness rebuilds the book every 100k adds (untimed) to hold per-level
  depth realistic (~1k orders/level).
- **Not reproducible to the nanosecond.** CPU frequency scaling is not pinned and the i7-1250U mixes
  P/E cores, so absolute numbers swing run to run (P50 `add` was seen between ~80 and ~132 ns across
  runs). For tighter numbers pin a performance core and disable turbo, e.g.
  `taskset -c 0 ./build-release/bench/bench_latency`. Treat the table as an order-of-magnitude baseline
  and track *relative* movement between commits.

## Not yet covered
- **Match / crossing-fill latency.** The engine does not cross orders yet (incoming orders only rest;
  market orders rest at a sentinel price), so there is no fill path to benchmark. A `bench_match`
  scenario will be added once matching lands.
