# Optimization #4: link-time optimization and a `tryMatch` fast path

Goal: cut per-call overhead on the order path. After
[optimization #3](optimization%233.md) removed the hash map and the per-order
allocations, the remaining cost was mostly function calls and repeated work,
not memory:

- **No cross-file inlining.** `OrderManager.cpp`, `BookLevel.cpp` and
  `OrderBook.cpp` are separate translation units. The small helpers called
  several times per operation (`getView`, `resolve`, `valid`, `getTopPrice`)
  were out-of-line calls that the compiler could not inline across files.
- **Redundant matcher work.** `Matcher::tryMatch` ran a full
  `OrderBook::modifyOrder` after every add and every modify, even when
  nothing could trade.

---

## 1. Link-time optimization (`CMakeLists.txt`)

- New option `ME_LTO`, default ON.
- When `check_ipo_supported` passes, it sets
  `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE`, so every Release target is
  built with `-flto`. On a toolchain that can't do LTO, configure prints a
  status message and carries on without it; it never fails the configure.
- **Release only.** Debug is left alone: LTO slows the link and makes stepping
  through code harder, for no benefit there.
- **Set before `FetchContent`,** so dependencies are built the same way. That
  is the configuration the numbers below were measured with.
- **Verified:** `compile_commands.json` shows `-flto` on every Release compile
  unit and none in Debug.

### Python wheel

`pybind11_add_module` adds its own `-flto` unless
`CMAKE_INTERPROCEDURAL_OPTIMIZATION` is defined. Only the `_RELEASE` variant
is set here, so the extension's own compile unit may get the flag twice. That's
harmless, and the wheel built and passed all 35 Python tests.

## 2. `Matcher::tryMatch` fast path (`include/me/Matcher.h`)

### Before
```cpp
std::vector<TradeEvent> filled_orders;          // fresh vector on every call
int rem_qty = order_book->fillOrders(opposite, price, qty, filled_orders, id);
order_book->modifyOrder(id, rem_qty, price, side, type);   // always
```

For a passive order `rem_qty == qty`, so that `modifyOrder` rewrote the
quantity the order already had. The rewrite was not cheap:
- `OrderBook::modifyOrder` calls `isOrderIdExist` on **both** sides (two
  slab lookups).
- `OrderBookSide::modifyOrder` does another lookup plus a binary search for
  the level.
- `BookLevel::modifyOrder` does one more lookup, then the write.

When an order did trade, `emplace_back` into the fresh vector allocated on
the matching thread.

### After
```cpp
const auto best = order_book->getBestPrice(opposite);
if (!best || (side == BUY ? price < *best : price > *best)) return false;

filled_orders.clear();                          // member; capacity is kept
int rem_qty = order_book->fillOrders(opposite, price, qty, filled_orders, id);
if (rem_qty != qty) order_book->modifyOrder(id, rem_qty, price, side, type);
```

- **Early exit.** The crossing test is the same inclusive rule
  `OrderBookSide::fillOrders` uses: a buy crosses an ask at or below its
  price, and a sell crosses a bid at or above it. An off-by-one here would
  silently stop orders priced exactly at the touch from filling, so a test
  pins it on both sides.
- **No no-op rewrite.** The incoming order is rewritten only when a fill
  changed it. That includes the case where the order crossed but met only
  zero-quantity resting orders, which `fillOrders` retires without trading.
- **Reused buffer.** Once the trade vector has grown to the largest sweep
  seen, a fill no longer allocates. The risk is publishing stale trades if
  `clear()` were ever skipped. A test runs a sweep, then a passive order,
  then a second sweep, and checks that each publishes only its own trades.
- **Same return value.** It is still `rem_qty == 0` for every order the risk
  check admits (quantity ≥ 1). `MatchingEngine` ignores it anyway.

---

## 3. Measurements

### Method
- `taskset -c 6,7 bench_latency 300000`, run interleaved three times, in
  this order each round:
  1. `main` (`8454e53`)
  2. LTO only (`eb5fc46`)
  3. this branch (`68b2746`)
- Each cell is the median of three runs.
- Same machine as before: i7-1250U, g++ 11.4.0, `-O3 -march=native`, WSL2.
- Timer overhead was 19–42 ns across runs, so this is the warm/throttled
  state. A 5-minute idle beforehand did not bring it back down: the one run
  taken right after it still read 38 ns.

### Where each change shows up (P50, ns)

| | `main` | LTO only | LTO + fast path |
|---|---:|---:|---:|
| OrderBook modify | 114 | 91 | 91 |
| OrderBook cancel | 85 | 66 | 68 |
| Engine add `[RingBuf/128]` | 278 | 147 | 169 |
| Engine modify `[RingBuf/128]` | 531 | 220 | 278 |
| Engine cancel `[RingBuf/128]` | 250 | 186 | 117 |

### `main` → this branch, full rows (ns)

| operation | P50 | P99 | P99.9 | mean |
|---|---:|---:|---:|---:|
| OrderBook add | 50 → 58 | 126 → 95 | 404 → 453 | 79 → 79 |
| OrderBook modify | 114 → **91** | 166 → **131** | 229 → **184** | 125 → **96** |
| OrderBook cancel | 85 → **68** | 137 → **102** | 183 → **120** | 94 → **75** |
| Engine add `[RingBuf/128]` | 278 → **169** | 536 → **281** | 1,071 → **895** | 337 → **179** |
| Engine modify `[RingBuf/128]` | 531 → **278** | 914 → **444** | 3,056 → **683** | 639 → **310** |
| Engine cancel `[RingBuf/128]` | 250 → **117** | 416 → **294** | 509 → **394** | 266 → **187** |

The engine gains hold at every ring capacity, which is good evidence they're
not run-to-run noise:

| P50, `main` → branch | 128 | 1K | 8K | 64K |
|---|---:|---:|---:|---:|
| Engine add | 278 → 169 | 287 → 161 | 314 → 172 | 292 → 167 |
| Engine modify | 531 → 278 | 538 → 234 | 515 → 200 | 375 → 246 |
| Engine cancel | 250 → 117 | 237 → 129 | 244 → 100 | 245 → 89 |

### What can and can't be attributed

- **LTO is the bulk of it.** Every OrderBook modify and cancel median dropped
  under LTO, and each Engine row roughly halved. The same direction and size
  were measured in an earlier session against the previous `main`
  (`5e68212`): OrderBook modify 179 → 91, Engine add 405 → 195.
- **The fast path is not separable in this session.** LTO-only and the branch
  overlap run for run (Engine add P50 111/147/221 vs 179/136/169; Engine
  modify 162/220/285 vs 278/195/286). In the earlier session, the fast path on
  top of LTO took Engine add P50 from 206 to 143, and all four runs with it
  beat all four without it. Taken together, the gain is real but smaller than
  this machine's run-to-run swing.
- **Engine cancel 186 → 117 is not the fast path's doing.** Cancel never
  reaches `tryMatch`, so treat that gap as noise or code-layout effects.
- **OrderBook add did not move** (50 → 58 P50, 126 → 95 P99). Add is mostly
  slab and level-vector work, which was already inlined inside its own file.

### No benchmark exercises matching

Every engine benchmark adds buy orders only, so nothing ever crosses. In these
runs the fast path only ever takes the early exit. A sweep, a partial fill,
and the removed per-fill allocation are not measured anywhere. A benchmark
with crossing orders is the first follow-up below.

### Google Benchmark suites

Mean ns/op, `--benchmark_min_time=0.5s`, two interleaved runs each:

| benchmark | `main` | branch |
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

- Every cancel and modify row is lower on the branch in both runs.
- `BM_Add_NewPriceLevel` reads about 40% worse. That benchmark opens one level
  per order and never frees anything, so it mostly measures page faults and
  adaptive iteration counts (see `bench/README.md`). It's recorded here, not
  explained. `bench_latency`, which rebuilds the book every 100k orders, shows
  no add regression.

---

## Testing

- **C++:** all 10 suites pass in Debug and in Release with LTO (Make). CI
  builds with Ninja, which goes through the same `check_ipo_supported` path.
- **Python:** all 35 tests pass against a wheel built with
  `pip install '.[test]'` in a fresh virtualenv, the same way CI builds it.
- **New matcher tests** (`tests/test_matcher.cpp`):
  - `NoMatchAgainstAnEmptyOppositeSide`
  - `PriceExactlyAtOppositeBestCrosses` (both sides)
  - `TradesFromAnEarlierMatchAreNotPublishedAgain`

---

## Follow-ups
- **A benchmark with crossing orders,** so matching, sweeps and fills are
  measured at all. It should come before any further matching-path work.
- **Match before resting.** Today an incoming order is booked, matched, then
  rewritten. Matching first and resting only the remainder means a fully
  filled order never touches a price level.
- **Look up an order once per operation.** An engine cancel resolves the same
  id about five times (`MatchingEngine` → `OrderBook::isOrderIdExist` →
  `OrderBookSide` → `BookLevel` → `OrderManager`), and `OrderBook::modifyOrder`
  checks both sides. LTO hides part of the cost, not all of it.
- **Level search.** Binary search runs over `vector<BookLevel>` (~64 bytes per
  element). A separate array of prices, or a scan from the best price, would
  touch fewer cache lines.
- **Linked order queues per level**, as listed in optimization #3.
