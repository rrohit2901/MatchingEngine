# Plan: Databento data + strategy replay (backtest) CLI

## Context
The engine currently matches synthetic orders, and a single 2012 AAPL LOBSTER sample is
the only real data. The goal is a **strategy replay mechanism**:
1. Replay one real Nasdaq trading day.
2. Let a user-written Python strategy trade into that replayed book, with configurable
   latency.
3. Report the results.

The first deliverable is a **CLI tool** that prints the results itself. A Streamlit UI,
email delivery and deployment come later.

## Decisions
- **Data source:** Databento **replaces LOBSTER entirely**.
- **Dataset:** **`XNAS.ITCH`, MBO schema**, plus a small **`mbp-1`** download for
  validation.
- **Download once:** one trading day for **2-3 popular symbols** (proposed: AAPL, NVDA,
  TSLA), stored locally. Databento is never queried during a backtest.
- **Storage:** **Parquet**, with the raw `.dbn.zst` files kept as the source of truth.
- **One symbol per backtest run.** This keeps the original single-symbol design: one
  `OrderBook` per run.
- **Fill model:** strategy orders are filled by real executions that reach their queue
  position. Aggressive strategy orders take real liquidity. Passive fills leave the venue's
  orders as recorded by default. This was revised in Phase 2; see the Phases 2–4 results.
- **Strategy callback: fixed timer every 10-20 ms of simulated time** (configurable,
  default 10 ms). No event-driven callbacks in the first version.

## What Databento's documentation says, and how it affects the design
Sources:
- Databento's own source code: the `dbn` repository, files `rust/dbn/src/enums.rs` and
  `flags.rs`.
- Databento's pages on MBO data, XNAS.ITCH, MBO snapshots and MBO publishing.
- Third-party code, for details Databento's own pages didn't give.

Databento's documentation pages render with JavaScript and came back truncated, so some
points below come from third-party sources. Those are marked **(3rd-party)** and are
checked against the downloaded data in Phase 0.

- **Depth:** MBO on `XNAS.ITCH` is **full depth**: "all orders across all price levels",
  keyed by order ID, with queue position recoverable. It is *not* limited to a fixed number
  of levels, so a replay with no strategy should reproduce Nasdaq's book exactly.
- **Where the replay can still differ from reality, and why it's acceptable:**
  - **Nasdaq only.** This is Nasdaq's book, not the consolidated best bid/offer across all
    venues. Real flow that traded on other venues is not seen. Validation must therefore
    compare against Nasdaq's own `mbp-1`, never against the consolidated quote.
  - **Hidden and non-displayed orders** never appear in the book. Executions against them
    show up as trades with no visible resting order.
  - **Data-quality flags.** `F_MAYBE_BAD_BOOK` means an unrecoverable gap was detected, and
    `F_BAD_TS_RECV` means a receive timestamp is unreliable. Databento's issue tracker
    lists known bad `XNAS.ITCH` dates (crossed books on 2021-07-07, 2021-10-26 and
    2022-09-19, plus an MSFT-specific report). Choose a date and symbols with no known
    issues.
  - **With a strategy running (Phase 2), divergence is the point:** the strategy's orders
    change the book. It is validated with targeted unit tests, not against `mbp-1`.
- **Actions** (quoted from Databento's source):
  - `A` Add: "A new order was added to the book."
  - `C` Cancel: "An order was fully or partially cancelled."
  - `M` Modify: "price and/or size"
  - `R` Clear: "Reset the book". Sent at the start of every session.
  - `T` Trade: "An aggressing order traded. **Does not affect the book.**"
  - `F` Fill: "An existing order was filled. **Does not affect the book.**"
  - `N` None: no effect on the book.
- **Sizes and sequencing (3rd-party):**
  - On `A`/`M`, size is the order's **new total**.
  - On `C`, size is the **amount removed**.
  - On `F`, size is the amount executed.
  - An execution arrives as `T`/`F` and is **followed by a `C` or `M`** that actually
    takes the size off the book.
- **Side:** on a `T` record, side is the **aggressor's** side ("buy aggressor in a trade").
- **`F_LAST`:** marks "the last record in the event". Only `A`/`C`/`M`/`R` change the book,
  but a record that changes nothing can still carry `F_LAST`, so event boundaries must be
  taken from the flag itself.
- **Prices:** `int64` scaled by 1e-9. **Cost:** new accounts get $125 in free credits;
  check the cost with `metadata.get_cost` before downloading.

## Gaps in the original list (folded into the phases)
1. **Passive fills / queue position: essential for OB alpha.**
   - A real execution is `T` (aggressor side) + `F` (the resting orders hit) + `C`/`M`
     (the book removal).
   - The strategy's resting orders never appear in an `F`, so on its own the strategy would
     never be filled.
   - **Synthetic aggressor:**
     - At each event containing executions, form one synthetic aggressor:
       - side = the `T` side
       - price = the `F` price
       - size = the sum of `F` sizes, i.e. the visible liquidity actually executed (this
         excludes hidden executions)
     - Match it FIFO through the engine. If the strategy is ahead in the queue, it fills
       first.
   - **Reconciliation:** the `C`/`M` records that follow refer to real orders already
     consumed by the synthetic aggressor. Clamp them to whatever is left of the order, or
     drop them when nothing is left. This generalises your rule that "further real events
     for consumed orders are rejected".
2. **Real adds can cross the strategy's orders.** If the strategy is quoting at a better
   price, a real incoming order that reaches that price trades with the strategy first.
3. **Latency lives in the simulator, not the order book.**
   - A simulation clock driven by `ts_recv` runs one time-ordered queue.
   - That queue merges market events with strategy actions, which arrive at
     `t + order_latency`.
   - The strategy sees book data and fills as of `t - md_latency`.
   - Orders carry `ts_sent` and `ts_arrival` for reporting.
4. **ID mapping.** A map from Databento `uint64` IDs to engine IDs, updated whenever the
   engine issues a new ID on a modify. Plus an **owner tag** (market or strategy) on every
   order.
5. **Real events bypass `RiskManager`.** Only strategy orders are risk-checked.
6. **Price conversion:** 1e-9 → 1e-4-dollar ticks (`int32`-safe up to about $214k).
7. **Callbacks only at event boundaries.** The 10 ms timer fires at the first `F_LAST`
   at or after each tick, so the strategy never sees a half-applied event.
   - A full day of regular trading hours (6.5 h) at 10 ms is about 2.3 M Python
     callbacks: acceptable.
   - Fills since the previous callback are delivered in a list inside `ctx`.
8. **Session handling.**
   - Replay from the `R` clear at session start, so pre-market events build the book.
   - The strategy only runs from 09:30 to 16:00.
   - It is paused around the opening and closing auctions (configurable buffer).
   - Halts pause the strategy.
9. **Quiet logging.** Log only strategy orders and fills, not millions of market events.
10. **Results:**
    - PnL: realized, plus open positions marked at mid-price.
    - Position over time, orders and fills, fill ratio.
    - Max drawdown.
    - Reconciliation counters.
    - Optional: exchange fees and maker rebates.
    - Self-trade prevention.
11. **Reproducibility:** one TOML config (symbol, date, latencies, timer interval, strategy
    and risk parameters).

## Phases

### Setup (first)
- `git fetch origin`, then `git switch -c feature/databento-data main`.
  - The working tree is clean.
  - The Optimizations #2 work lives on `feature/low_level_optimizations` (PR #6) and is
    not needed for data collection.
- Save this plan as **`docs/strategy-replay-plan.md`**, next to
  `docs/optimizations#2.md`. That file is on the PR #6 branch, so `docs/` starts with only
  this file on the new branch.
- The first implementation scope is **Phase 0 only**: data collection.

### Phase 0: data download + empirical check (replaces LOBSTER)
- **`scripts/fetch_databento.py`**
  - Uses `databento.Historical` and the `DATABENTO_API_KEY` environment variable.
  - Prints `metadata.get_cost` for `mbo` and `mbp-1` and asks for confirmation.
  - Downloads one day for the chosen symbols to `data/databento/raw/*.dbn.zst`.
    (`data/` is already git-ignored.)
- **`scripts/convert_mbo.py`**
  - Writes `data/databento/<date>/<SYM>.mbo.parquet`, with columns
    `ts_recv, ts_event, action, side, price_ticks, size, order_id, flags`.
  - Writes `<SYM>.mbp1.parquet` for the best bid/ask.
- **`scripts/inspect_mbo.py` (empirical check).** On the real data, confirm the
  3rd-party items above:
  - a `C` size never exceeds the order's outstanding size
  - the `T` → `F` → `C`/`M` ordering within one `F_LAST` event
  - `T` records with no `F` (hidden executions)
  - how ITCH replace messages appear (`M`, or `C` + `A`)
  - counts of `F_MAYBE_BAD_BOOK` and `F_BAD_TS_RECV`
  The replayer is written against what this script finds.
- Delete `scripts/parse_lobster.py` and `data/lobster/`, and update `README.md:262`.

#### Phase 0 results: 2026-09-29, AAPL / NVDA / TSLA (done)
**The download**
- Cost: $1.17, 14.1 M MBO records plus 3.2 M `mbp-1` records.
- The request window runs from UTC midnight to New York midnight. Databento warns that MBO
  requests should start at UTC midnight, which is where it places its synthetic book
  snapshot.
- Parquet row counts equal Databento's quoted record counts for every file, and every
  price converted to an exact 1e-4 tick.

**Measured with `scripts/inspect_mbo.py`, the same on all three symbols:**

| finding | AAPL | NVDA | TSLA | consequence for the replay |
|---|---:|---:|---:|---|
| `R` clears | 1 (03:04:53 ET) | 1 | 1 | The book starts empty at the `R`; there is no snapshot to load. |
| `C` larger than the order, or for an unknown id | 0 | 0 | 0 | **Cancel size = amount removed** (confirmed). |
| partial `C` | 21,744 | 31,571 | 18,020 | A partial cancel modifies the order down and keeps its queue priority. |
| `M` records | 0 | 0 | 0 | **No modifies on `XNAS.ITCH`.** A replace is `C` + `A` with a new id, usually in one event. Keep `M` support for other datasets. |
| fills followed by an exact-size `C` on the same order, in the same event | 106,618 / 106,618 | 150,585 / 150,585 | 79,990 / 79,990 | **Every execution is `T` → `F` → `C`.** The `C` is what takes the size off the book, so the synthetic aggressor's reconciliation targets these `C`s. |
| `T` with no `F` (hidden) | 24,864 | 36,345 | 21,185 | Mostly side `N`. They leave the visible book alone; count them, don't match them. |
| `T` size > sum of `F` sizes | 7,894 | 10,277 | 5,926 | Partly hidden executions. **Aggressor size = sum of `F` sizes**, as planned. |
| `F` price ≠ the resting order's price | 1,097 | 1,513 | 512 | About 1% of fills, spread across the day, not only at the auctions. These are probably Nasdaq order types that execute away from their displayed price (not confirmed). Use the `F` price for trade prints; match by book priority. |
| book crossed at an event end | 0 | 0 | 0 | The book never crosses: a good sign for Phase 1. |
| `F_MAYBE_BAD_BOOK` / `F_BAD_TS_RECV` / `ts_recv` going backwards | 0 / 1 / 0 | 0 / 1 / 0 | 0 / 1 / 0 | Clean day. |
| orders still live at 20:00 ET | 7,560 | 4,963 | 17,759 | No deletes are sent at session end; the replay simply stops. |

Notes on the table:
- `execution_order_other` (about 1% of execution events) is an artifact of the check, not
  a violation. In events like `C T F C`, a cancel of a different order precedes the trade.
- Each `T F C` triple is itself in order.

### Phase 1: market replayer (C++), no strategy
- **Input:** a `MboEvents` struct of column spans. Python (pyarrow) loads the Parquet file
  and passes the numpy columns without copying, so there is no Arrow C++ dependency.
- **`MarketReplayer` actions:**
  - `A` → `OrderBook::addOrder`
  - `C` → a partial cancel modifies the order down (keeps queue priority); a full cancel
    calls `cancelOrder`
  - `M` → `modifyOrder`
  - `R` → clear the book
  - `T`/`F`/`N` → no change to the book
- Market events go straight to `OrderBook`, skipping risk checks and matching.
- **Validation:** at every `F_LAST`, compare the best bid/ask against Nasdaq's `mbp-1`
  (target: exact match, apart from records flagged bad). Also check that the book never
  crosses.

#### Phase 1 results: 2026-09-29 (done)
**What was built**
- `include/replay/MboEvents.h`: `MboEvents` and `Mbp1Events`, structs of column spans.
- `MarketReplayer`: venue → engine id map, stats, and anomaly counters.
- `ReplayValidator`: `validateAgainstMbp1`.
- The `me_replay` CMake target.
- `OrderBook::getTopLevel` (allocation-free).
- Python: `matching_engine.replay` (Parquet → numpy; dtypes are checked and never copied
  silently) and `_core.validate_replay`.
- `scripts/validate_replay.py`.
- Tests: 12 gtest cases (`tests/test_market_replayer.cpp`) and 4 pytest cases
  (`tests/python/test_replay.py`).

**Alignment.** Every `mbp-1` record carrying `F_LAST` has exactly the `(ts_recv,
sequence)` key of an MBO event-end record, and those keys are unique. So the comparison
is exact: there is no time-window matching.

| symbol | records | top-of-book comparisons | mismatches | crossed | anomalies | replay + compare |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | 5,027,491 | 809,283 | 0 | 0 | 0 | 7.2 s |
| NVDA | 6,563,202 | 1,606,306 | 0 | 0 | 0 | 5.4 s |
| TSLA | 2,549,255 | 431,039 | 0 | 0 | 0 | 3.4 s |

**The rebuilt book equals Nasdaq's top of book, price and size, after every event that
touched it.**

Throughput is 0.7–1.2 M records/s. That's adequate for a backtester (a full day takes
seconds), but well below what the book should do. Likely costs, not yet profiled:
- the `unordered_map` id map, which allocates one node per add
- `OrderBook::cancelOrder` resolving the order's side with extra lookups
- levels shifting in a deep book

### Phase 2: simulator (C++)
- **Simulation clock and event queue:** order latency and market-data latency, plus the
  10 ms strategy timer.
- **Order changes:** owner tag and timestamps, in `include/me/Order.h` and
  `OrderManager`.
- **Synthetic aggressors** for executions, and real adds matched against strategy orders.
- **Clamp/drop reconciliation** of later `C`/`M` records, with counters.
- Strategy orders go through `RiskManager`.
- Quiet logging.

### Phase 3: Python strategy API (`python/matching_engine/backtest.py` + pybind11)
- **`Backtester(symbol, date, config)`**, with the replay loop in C++. It is separate from
  `PyMatchingEngine`, so there is no logger thread or `LockQueue`.
- **`Strategy`:** `on_start(ctx)`, `on_timer(ctx)` (every 10-20 ms), `on_end(ctx)`.
- **`ctx`:** `book(n)` (the `LevelView` list), `best_bid/ask`, `add`, `cancel`,
  `modify`, `position`, `pnl`, `now`, `fills` (since the last callback), `open_orders`.

### Phase 4: CLI
- **Command:** `me-backtest run --config bt.toml --strategy strategies/ob_alpha.py
  [--symbol --date --order-latency-us --md-latency-us --timer-ms]`.
- **Output:** a terminal summary (PnL, position, fills, fill ratio, drawdown,
  reconciliation counters, run time), plus an optional CSV of fills and PnL.
- **Example strategy:** `strategies/ob_alpha.py` keeps fixed-size quotes on one side of the
  book, with more quantity on the top 5 levels.

### Phases 2–4 results (done)
How to use it: [`backtesting.md`](backtesting.md). The rules: `include/replay/Simulator.h`.

**What was built**
- **`Simulator`** (`include/replay/Simulator.h`, `src/replay/Simulator.cpp`):
  - One clock merges strategy actions arriving at the exchange, venue records and the
    strategy timer. At equal times the order is action, then venue, then timer.
  - Market-data latency is folded into the action delay.
  - Gateway checks: price grid, position limit, trading window.
  - Exchange checks on arrival: `RiskManager`, and self-trade prevention.
  - IOC orders.
  - Fees, an equity curve, and reconciliation counters.
- **Python** (`python/matching_engine/backtest.py`): `Strategy` (`on_start`, `on_timer`,
  `on_end`), `Context` (dollar prices), `BacktestConfig` and `run_backtest`.
- **`me-backtest`** (`python/matching_engine/cli.py`):
  - `run`: TOML config plus flag overrides, `--param`, and CSV outputs.
  - `validate` (`--through-simulator`).
- **Example:** `strategies/ob_alpha.py` and `.toml`.
- **Tests:** 14 gtest cases (`tests/test_simulator.cpp`) and 5 pytest cases
  (`tests/python/test_backtest.py`).

**Departures from the plan above, and why**
- **No owner tag or timestamps in `Order` / `OrderManager`.**
  - The simulator keeps strategy orders in its own table, keyed by engine id, with
    `ts_sent` and `ts_arrival`.
  - That keeps the engine's 32-byte slot and the hot path untouched.
  - The engine gained only read access to a level's FIFO queue
    (`OrderBook::getLevelQueue`).
- **Executions are not re-matched as one synthetic aggressor.** Each venue `F` names the
  order it hit, so the simulator works per fill:
  1. Strategy orders queued ahead of that order at its price are filled first.
  2. The named order is reduced next.
  3. If the strategy already took the named order's size, the fill continues down the
     queue (`SWEEP`).

  This reproduces the venue exactly when there is no strategy, without assuming the
  venue's matching is plain FIFO. (About 1% of fills price away from the order's
  displayed price.)
- **Passive fills have no impact by default (`passive_impact = false`).**
  - The first version conserved execution quantity: a fill the strategy took ahead in the
    queue was taken out of the venue order behind it.
  - That left venue orders holding size Nasdaq had already retired. On AAPL with OB alpha
    that came to 24,591 orphaned orders and 568,120 phantom shares by the close.
  - Now the venue's orders change exactly as recorded, and the strategy is filled in
    addition. It stays available as `passive_impact = true`.
  - Aggressive strategy orders always take real liquidity, and the venue's later records
    for those orders are clamped or swept.
- **Callbacks use a fixed timer only** (default 10 ms), as decided. Fills since the last
  call arrive in `ctx.fills`.

**Checks on 2026-09-29**
- **No strategy:** the `Simulator` with no strategy matches `mbp-1` on all 2,846,628
  comparisons (`me-backtest validate --through-simulator`). So the reconciliation logic
  is neutral when there is nothing to reconcile.
- **OB alpha on AAPL** (both sides, top 5 levels, $5,000 × weights 5..1, ±1,500 shares,
  50 + 20 µs latency):
  - PnL −$20,152, with 707k shares bought and 708k sold, all of it as maker.
  - By source: 826k `CROSSING_ADD` (stale quotes picked off) and 590k `AHEAD_IN_QUEUE`.
  - 6,700 `SELF_TRADE` rejects.
  - Zero orphans, sweeps, clamped cancels and venue anomalies.
  - 14.5 s for 2.33 M strategy calls.
- **PnL reconciles with the fill log exactly** (TSLA: −$18,776.63 both ways), and two runs
  give identical fills.
- **Latency changes the outcome as expected.** On TSLA: 48,392 fills at 0 µs, 48,334 at
  1 ms and 48,290 at 10 ms.

### Later (out of scope)
- Streamlit UI.
- Sandboxing user code.
- Emailing results.
- Deployment.

## Critical files
- **New:**
  - `scripts/fetch_databento.py`, `scripts/convert_mbo.py`, `scripts/inspect_mbo.py`
  - `include/replay/` (`MboEvents.h`, `MarketReplayer.h`, `Simulator.h`) and
    `src/replay/`
  - `python/matching_engine/backtest.py`
  - A CLI console script in `pyproject.toml`
  - `strategies/ob_alpha.py`
- **Modified:**
  - `include/me/Order.h` and `OrderManager`: owner tag and timestamps
  - `include/me/OrderBook.h`: a path for market events that skips risk and matching
  - `src/python/module.cpp`: bindings
  - `CMakeLists.txt`: new targets
  - `README.md`

## Verification
- **Phase 0:** the cost is printed before any spend. Parquet row counts per action match
  the raw DBN files. `inspect_mbo.py` reports on each 3rd-party assumption.
- **Phase 1:** the best bid/ask equals Nasdaq's `mbp-1` at every `F_LAST` (any mismatches
  listed with their flags), and the book never crosses.
- **Phase 2:** unit tests with hand-written event sequences:
  - a strategy order ahead in the queue is filled by the synthetic aggressor
  - a later `C` for a consumed real order is dropped
  - a partial consume clamps a later `C`
  - a real add crossing a strategy quote fills it
  - a latency of 0 against a latency of N changes fills as expected
- **Phase 3/4:** run the CLI with OB alpha on one symbol-day. Check that PnL reconciles with
  the fills, that results are deterministic across runs, and how long the run takes.
