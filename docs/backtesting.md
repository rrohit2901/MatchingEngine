# Backtesting a strategy on a replayed Nasdaq day

`me-backtest` replays one trading day of real Nasdaq order-level data for one
symbol through the engine's order book, lets a Python strategy trade into it
with configurable latency, and reports what happened.

- **How the replay was built and validated:** [`strategy-replay-plan.md`](strategy-replay-plan.md).
- **Every design decision:** [`decisions.md`](decisions.md).
- **The simulator's exact rules:** `include/replay/Simulator.h`.

---

## Quick start

```bash
pip install '.[backtest]'          # builds the extension; adds pyarrow and a TOML reader
me-backtest run --config strategies/ob_alpha.toml
```

That runs the example strategy on AAPL for 2026-09-29. It needs the converted
data in `data/databento/2026-09-29/`; [Getting data](#getting-data) explains how
to get it.

Any setting can be overridden on the command line:

```bash
me-backtest run --config strategies/ob_alpha.toml --symbol NVDA --order-latency-us 200 \
    --param levels=3 --param order_value=2000 --fills-csv logs/fills.csv
me-backtest run --strategy my_strategy.py --date 2026-09-29 --symbol TSLA
me-backtest run --help
```

A full regular session (6.5 h, a 10 ms timer, about 2.3 M strategy calls)
takes about 15 s.

## Writing a strategy

A strategy is a Python file with one subclass of `Strategy`:

```python
from matching_engine.backtest import Strategy

class JoinTheBid(Strategy):
    def __init__(self, size=100):          # parameters come from [strategy.params] / --param
        super().__init__(size=size)
        self.size = size

    def on_start(self, ctx):               # once, at the first timer of the window
        pass

    def on_timer(self, ctx):               # every timer_ms of exchange time
        bid = ctx.best_bid
        if bid and not ctx.open_orders and ctx.position < 1_000:
            ctx.buy(bid.price, self.size)

    def on_end(self, ctx):                 # after the window closed; orders already cancelled
        print(f"final position {ctx.position}, PnL {ctx.pnl:.2f}")
```

### The `ctx` object

| | |
|---|---|
| `ctx.now` / `ctx.time` | exchange time: UTC nanoseconds / a New York `datetime` |
| `ctx.book(levels=5)` | `Book(bids, asks)`, each a list of `Level(price, quantity)`, best first |
| `ctx.best_bid`, `ctx.best_ask`, `ctx.mid` | `Level` or `None`; mid as a float or `None` |
| `ctx.position` | shares, + long / − short |
| `ctx.capital_deployed("BUY")` | dollars deployed on a side, as `max_capital` measures it |
| `ctx.cash`, `ctx.pnl` | dollars; `pnl` = cash + position × the current mid − fees |
| `ctx.fills` | `Fill`s since the previous callback |
| `ctx.open_orders` | `Order`s that are `PENDING` (in flight) or `OPEN`, oldest first |
| `ctx.order(id)` | one `Order`, any status |
| `ctx.buy(price, qty)` / `ctx.sell(price, qty)` | limit order: trades what it can on arrival, rests the rest; returns its id |
| `ctx.cancel(id)` / `ctx.cancel_all()` | cancel requests; they travel with latency too |

- **Prices** are dollars on the one-cent grid. An off-grid price is rejected
  (`PRICE_INCREMENT`), never rounded.
- **Quantities** are whole shares. `100` and `100.0` are fine; `100.7` is rejected
  (`QUANTITY_NOT_INTEGER`), never truncated.
- **The book** includes the strategy's own resting orders, as a real feed would
  show them.
- **`Order`** has `order_id, side, price, quantity, filled, remaining,
  status, reject_reason, ts_sent, ts_arrival`. The statuses are `PENDING`,
  `OPEN`, `FILLED`, `CANCELLED` and `REJECTED`.
- **`Fill`** has `ts, order_id, side, price, quantity, maker, source`. `source`
  says how the fill happened; see [The model](#the-model).

### What gets rejected

| reason | where | when |
|---|---|---|
| `PRICE_INCREMENT` | gateway, immediately | price not a whole cent |
| `QUANTITY_NOT_INTEGER` | gateway, immediately | quantity not a whole number of shares |
| `POSITION_LIMIT` | gateway, immediately | `[risk] max_position` exceeded, counting every open order on that side as if filled |
| `CAPITAL_LIMIT` | gateway, immediately | `[risk] max_capital` exceeded: dollars deployed on that side, which is the position at the current mid plus every open order on the side at its limit price, plus this order. Orders that reduce the position are never blocked. |
| `OUTSIDE_TRADING_WINDOW` | gateway, immediately | sent outside `[session]` start–end |
| `QUANTITY_ABOVE_MAX` / `QUANTITY_BELOW_MIN` | exchange, on arrival | `[risk] max_order_qty` / `min_order_qty` |
| `PRICE_TOO_FAR_FROM_TOP` | exchange, on arrival | more than `[risk] max_price_deviation` dollars from the same-side best price |
| `SELF_TRADE` | exchange, on arrival | only with `[model] self_trade_prevention = true`: the order would trade with one of the strategy's own resting orders |

## The config file

`strategies/ob_alpha.toml` lists every key with a comment. In short:

| section | keys |
|---|---|
| `[data]` | `date`, `symbol` (one per run), `dir` |
| `[session]` | `start`, `end` (New York time; defaults 09:31–15:59, clear of the auctions), `timer_ms` |
| `[latency]` | `order_us` (strategy → exchange), `market_data_us` (exchange → strategy) |
| `[risk]` | `max_position` (shares), `max_capital` (dollars), `max_order_qty`, `min_order_qty`, `max_price_deviation` |
| `[fees]` | `maker`, `taker`: dollars per share, negative = rebate |
| `[model]` | `passive_impact`, `self_trade_prevention` (see below) |
| `[strategy]` | `path`, optional `class`, and `[strategy.params]` passed to the constructor |
| `[output]` | `pnl_sample_ms`, optional `fills_csv`, `equity_csv`, `orders_csv` |

## The model

**One clock.** Exchange time comes from Databento's `ts_recv`. Three things
happen in time order: strategy orders arriving at the exchange, real market
events, and the strategy timer. At equal times, an arriving order goes first,
then market events, then the timer.

**Latency.** `on_timer` at time T sees the book as of T. Its orders reach the
exchange at T + `market_data_us` + `order_latency_us`, which is when a strategy
that saw T's data `market_data_us` late would have sent them.

**How strategy orders fill** (`Fill.source`):

| source | what happened |
|---|---|
| `AGGRESSIVE` | the strategy's order crossed resting liquidity on arrival and took it |
| `AHEAD_IN_QUEUE` | a real execution hit an order queued *behind* the strategy's at the same price, so the real aggressor would have hit the strategy first |
| `CROSSING_ADD` | a real order arrived at a price that crosses the strategy's resting order (typically a stale quote being picked off) |
| `SWEEP` | a real execution ran past an order whose size the strategy had already taken |
| `SELF_TRADE` | the strategy's order traded with one of its own resting orders; both sides are recorded (taker and maker) |

**Impact on the real orders.**
- **Aggressive orders:** when the strategy takes real liquidity, it's gone. The
  later records for those orders are reconciled against what is left:
  - a fill moves on to the next orders in the queue (`SWEEP`)
  - a cancel is clamped to what remains
- **Passive fills:** by default (`passive_impact = true`) a passive fill comes
  out of the real execution. The real order queued behind the strategy keeps the
  size the strategy took, as it would have in reality.
  - The catch: once Nasdaq retires that order, nothing in the data removes the
    size it kept. It stays in the book as an orphan.
  - On the example strategy that came to about 24,600 orphaned orders (568k shares)
    on AAPL by the close. The report counts them.
- **`passive_impact = false`:** leaves the real orders exactly as recorded, so the
  replayed market stays identical to Nasdaq's. The strategy is filled in addition,
  so the execution is counted twice.

**Self-trades.** Nasdaq lets an order trade with a resting order from the same
firm unless the firm opts in to self-match prevention. So by default the
strategy's own orders trade with each other:
- both fills are recorded, with source `SELF_TRADE`
- the position doesn't change, and both the taker and the maker fee are paid

Set `[model] self_trade_prevention = true` to reject such orders instead.

**What the replay cannot know or doesn't model:**
- how other traders would have reacted to the strategy's orders
- hidden liquidity
- other venues: this is Nasdaq's book only
- trading halts: they aren't handled. The default window (09:31–15:59) keeps clear of
  the opening and closing auctions.

A strategy that quotes better than the market is filled only when real flow
arrives at its price.

## Output

`run` prints:
- **PnL**, marked to mid, with fees.
- **Max drawdown**, from the equity curve sampled every `pnl_sample_ms`.
- **Position:** final, maximum long and maximum short.
- **Volume:** shares bought and sold, and notional traded.
- **Orders and rejects**, with reject reasons.
- **Fill ratio** (filled ÷ accepted quantity) and **maker share**.
- **Filled quantity by source.**

It ends with the **reconciliation counters**. Venue anomalies should always be 0.
`orphaned orders` measures how much size passive fills have left in the book; it is
0 with `passive_impact = false`.

`--fills-csv`, `--equity-csv` and `--orders-csv` write each fill, the equity
curve, and every order.

For programs (the web UI uses these):
- `--out DIR` writes `result.json` (everything in the report, plus the equity curve),
  `fills.csv` and `orders.csv`.
- If the run fails, `result.json` holds `{"ok": false, "error": {...}}` and the exit
  code is 2. The error's `kind` is `config`, `strategy_load`, `strategy_init` or
  `strategy_runtime`, and its traceback shows only the strategy's own lines.
- `--progress FILE` keeps a small JSON file updated with how far through the trading
  window the replay is.
- `--progress -` prints `ME-PROGRESS <fraction>` lines on stderr instead. The web UI
  uses this, because a sandboxed run's files can't be read until it ends.
- `--quiet` drops the printed report.

## Web UI

```bash
pip install '.[web]'
streamlit run webapp/app.py          # http://localhost:8501
```

Live at **https://52-65-150-242.sslip.io**.

- **Sidebar:** every setting above.
- **Main area:**
  - the strategy's code (or upload a `.py` file)
  - its parameters, as TOML
  - an optional **run label**, which heads the results and names the downloads.
    Without one, the heading is your Strategy class's name.
- **Run:** executes in a separate process with limits:
  - 120 s wall time, 90 s CPU, 20 MB of output, 100 KB of code
  - memory: 1.5 GB locally, 1 GB on the server
  - runs at once: 2 locally, 1 on the server; the rest queue
  - per visitor IP: one run at a time, 30 s apart, 20 an hour
- **Results** appear on the page:
  - metrics
  - equity, position and mid charts
  - fills by source and rejects
  - reconciliation counters
  - fills and orders as CSV downloads
- **Locally, the runner is not a sandbox.** On the server every run is in an isolate box
  with no network; see [`deploy.md`](deploy.md).

## Getting data

```bash
pip install '.[data]'
export DATABENTO_API_KEY=...
python3 scripts/fetch_databento.py --date 2026-09-29 --symbols AAPL NVDA TSLA   # shows the cost first
python3 scripts/convert_mbo.py --date 2026-09-29
me-backtest validate --date 2026-09-29 --symbols AAPL NVDA TSLA --through-simulator
```

`validate` replays each symbol with no strategy and compares the rebuilt book
with Nasdaq's own top of book after every event. On 2026-09-29 all 2,846,628
comparisons match.
