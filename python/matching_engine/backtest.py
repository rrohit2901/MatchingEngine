"""Backtest a strategy on one replayed Nasdaq trading day.

Write a strategy by subclassing :class:`Strategy` and overriding its callbacks;
everything it can see and do goes through the :class:`Context` it is handed::

    from matching_engine.backtest import Strategy

    class JoinTheBid(Strategy):
        def on_timer(self, ctx):
            bid = ctx.best_bid
            if bid and not ctx.open_orders:
                ctx.buy(bid.price, 100)

Then run it with ``me-backtest run`` (see matching_engine.cli) or from Python
with :func:`run_backtest`.

How the replay treats the strategy (details in include/replay/Simulator.h and
docs/strategy-replay-plan.md):

* One symbol, one day, from the Databento MBO data that scripts/convert_mbo.py
  wrote. The strategy's orders share the book with the real ones: a resting
  order joins the back of its price level and is filled when real executions
  reach it, and an aggressive order takes real liquidity, which the rest of the
  replay then lacks.
* ``on_timer`` runs every ``timer_ms`` of exchange time, inside the trading
  window, and sees the book as of that instant. Orders reach the exchange
  ``md_latency_us + order_latency_us`` later; a cancel travels the same way.
* Prices are dollars on the one-cent grid; anything else is rejected
  (``PRICE_INCREMENT``), never rounded. Quantities are shares.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from datetime import date as Date
from datetime import datetime, time as Time, timezone
from pathlib import Path
from typing import Any, NamedTuple, Optional
from zoneinfo import ZoneInfo

from . import replay
from ._core import OrderSide, RiskParams, SimConfig, Simulator

TICKS_PER_DOLLAR = 10_000
NEW_YORK = ZoneInfo("America/New_York")

__all__ = [
    "BacktestConfig", "BacktestResult", "Context", "Fill", "Level", "Order", "Strategy", "run_backtest",
]


def to_ticks(price: float) -> int:
    return int(round(price * TICKS_PER_DOLLAR))


def to_dollars(ticks: int) -> float:
    return ticks / TICKS_PER_DOLLAR


def _side_name(side: OrderSide) -> str:
    return "BUY" if side == OrderSide.BUY else "SELL"


class Level(NamedTuple):
    price: float
    quantity: int


class Book(NamedTuple):
    bids: list[Level]   # best first
    asks: list[Level]


class Fill(NamedTuple):
    ts: int             # exchange time, UTC ns
    order_id: int
    side: str           # "BUY" or "SELL"
    price: float
    quantity: int
    maker: bool
    source: str         # AGGRESSIVE, AHEAD_IN_QUEUE, SWEEP, CROSSING_ADD or SELF_TRADE


class Order(NamedTuple):
    order_id: int
    side: str
    price: float
    quantity: int
    filled: int
    status: str         # PENDING, OPEN, FILLED, CANCELLED or REJECTED
    reject_reason: str
    ts_sent: int
    ts_arrival: int

    @property
    def remaining(self) -> int:
        return self.quantity - self.filled


def _order(t) -> Order:
    client_id, side, price, quantity, filled, status, reason, sent, arrival = t
    return Order(client_id, _side_name(side), to_dollars(price), quantity, filled, status, reason, sent, arrival)


def _fill(t) -> Fill:
    ts, client_id, side, price, quantity, maker, source = t
    return Fill(ts, client_id, _side_name(side), to_dollars(price), quantity, maker, source)


class Context:
    """What a strategy sees and does. Only valid inside a callback."""

    def __init__(self, sim: Simulator, symbol: str):
        self._sim = sim
        self.symbol = symbol
        self._fills_seen = 0
        self._new_fills: list[Fill] = []

    def _advance(self) -> None:
        count = self._sim.fill_count
        self._new_fills = [_fill(f) for f in self._sim.fills(self._fills_seen)] if count > self._fills_seen else []
        self._fills_seen = count

    # --- time ---------------------------------------------------------------
    @property
    def now(self) -> int:
        """Exchange time, UTC nanoseconds."""
        return self._sim.now

    @property
    def time(self) -> datetime:
        """Exchange time in New York."""
        return datetime.fromtimestamp(self._sim.now / 1e9, tz=timezone.utc).astimezone(NEW_YORK)

    # --- market -------------------------------------------------------------
    def book(self, levels: int = 5) -> Book:
        """Top `levels` price levels per side, best first. Includes the strategy's own orders."""
        bids, asks = self._sim.book(levels)
        return Book([Level(to_dollars(p), q) for p, q in bids], [Level(to_dollars(p), q) for p, q in asks])

    @property
    def best_bid(self) -> Optional[Level]:
        best = self._sim.best(OrderSide.BUY)
        return Level(to_dollars(best[0]), best[1]) if best else None

    @property
    def best_ask(self) -> Optional[Level]:
        best = self._sim.best(OrderSide.SELL)
        return Level(to_dollars(best[0]), best[1]) if best else None

    @property
    def mid(self) -> Optional[float]:
        bid, ask = self.best_bid, self.best_ask
        return (bid.price + ask.price) / 2 if bid and ask else None

    # --- account ------------------------------------------------------------
    @property
    def position(self) -> int:
        return self._sim.position

    @property
    def cash(self) -> float:
        return to_dollars(self._sim.cash_ticks)

    @property
    def pnl(self) -> float:
        """Cash plus position at the last mid, minus fees, in dollars."""
        return self._sim.mark_to_market()

    @property
    def fills(self) -> list[Fill]:
        """Fills since the previous callback."""
        return self._new_fills

    @property
    def open_orders(self) -> list[Order]:
        """PENDING (in flight) and OPEN orders, oldest first."""
        return [_order(t) for t in self._sim.orders(True)]

    def order(self, order_id: int) -> Optional[Order]:
        t = self._sim.order(order_id)
        return _order(t) if t else None

    # --- actions ------------------------------------------------------------
    def buy(self, price: float, quantity: int) -> int:
        """Limit buy: trades what it can on arrival, rests the rest. Returns the order id;
        check ctx.order(id) for its status."""
        return self._sim.submit(OrderSide.BUY, to_ticks(price), int(quantity))

    def sell(self, price: float, quantity: int) -> int:
        """Limit sell. Returns the order id."""
        return self._sim.submit(OrderSide.SELL, to_ticks(price), int(quantity))

    def cancel(self, order_id: int) -> bool:
        """Request a cancel. False if the order is unknown or already done."""
        return self._sim.cancel(order_id)

    def cancel_all(self) -> None:
        self._sim.cancel_all()


class Strategy:
    """Base class. Constructor keyword arguments are the strategy's parameters
    (the [strategy.params] table of the config)."""

    def __init__(self, **params: Any):
        self.params = params

    def on_start(self, ctx: Context) -> None:
        """Once, at the first timer of the trading window, before on_timer."""

    def on_timer(self, ctx: Context) -> None:
        """Every timer interval inside the trading window."""

    def on_end(self, ctx: Context) -> None:
        """Once, after the trading window closed and every open order was cancelled."""


@dataclass
class BacktestConfig:
    date: str
    symbol: str
    data_dir: Path = replay.DEFAULT_DATA_DIR
    start: str = "09:31:00"          # New York time; skips the opening auction
    end: str = "15:59:00"            # New York time; stops before the closing auction
    timer_ms: float = 10.0
    order_latency_us: float = 0.0
    md_latency_us: float = 0.0
    max_position: int = 0            # shares; 0 = no limit
    max_order_qty: int = 100_000
    min_order_qty: int = 1
    max_price_deviation: float = 1.00   # dollars from the same-side best price
    maker_fee: float = 0.0           # dollars per share; negative = rebate
    taker_fee: float = 0.0
    pnl_sample_ms: float = 1_000.0
    # See SimConfig::passive_impact. True (default) takes what the strategy is
    # filled passively out of the venue order behind it; False leaves the
    # venue's orders exactly as recorded.
    passive_impact: bool = True
    # Nasdaq lets self-trades execute unless the firm opts in to prevention.
    self_trade_prevention: bool = False

    def ny_to_utc_ns(self, clock: str) -> int:
        day = Date.fromisoformat(self.date)
        local = datetime.combine(day, Time.fromisoformat(clock), tzinfo=NEW_YORK)
        return int(local.timestamp()) * 1_000_000_000 + local.microsecond * 1_000

    def sim_config(self) -> SimConfig:
        c = SimConfig()
        c.trade_start_ns = self.ny_to_utc_ns(self.start)
        c.trade_end_ns = self.ny_to_utc_ns(self.end)
        if c.trade_end_ns <= c.trade_start_ns:
            raise ValueError(f"trading window {self.start}-{self.end} is empty")
        c.timer_interval_ns = int(self.timer_ms * 1e6)
        c.order_latency_ns = int(self.order_latency_us * 1e3)
        c.md_latency_ns = int(self.md_latency_us * 1e3)
        c.max_position = int(self.max_position)
        c.risk = RiskParams(int(self.max_order_qty), int(self.min_order_qty), to_ticks(self.max_price_deviation))
        c.maker_fee = float(self.maker_fee)
        c.taker_fee = float(self.taker_fee)
        c.pnl_sample_interval_ns = int(self.pnl_sample_ms * 1e6)
        c.passive_impact = bool(self.passive_impact)
        c.self_trade_prevention = bool(self.self_trade_prevention)
        return c


@dataclass
class BacktestResult:
    config: BacktestConfig
    summary: dict[str, Any]
    fills: list[Fill]
    orders: list[Order]
    equity: list[tuple[int, int, float, Optional[float], float]]  # (ts, position, cash $, mid $, equity $)
    stats: dict[str, Any]
    timings: dict[str, float] = field(default_factory=dict)


def _summarise(sim: Simulator, fills: list[Fill], orders: list[Order],
               equity: list[tuple[int, int, float, Optional[float], float]]) -> dict[str, Any]:
    accepted = [o for o in orders if o.status != "REJECTED"]
    sent_qty = sum(o.quantity for o in accepted)
    filled_qty = sum(f.quantity for f in fills)
    curve = [e[4] for e in equity]
    peak, drawdown = float("-inf"), 0.0
    for value in curve:
        peak = max(peak, value)
        drawdown = max(drawdown, peak - value)
    positions = [e[1] for e in equity] or [0]
    return {
        "pnl": sim.mark_to_market(),
        "fees": sim.fees,
        "final_position": sim.position,
        "max_long": max(max(positions), 0),
        "max_short": min(min(positions), 0),
        "orders": len(orders),
        "rejected": len(orders) - len(accepted),
        "fills": len(fills),
        "bought": sum(f.quantity for f in fills if f.side == "BUY"),
        "sold": sum(f.quantity for f in fills if f.side == "SELL"),
        "notional": sum(f.price * f.quantity for f in fills),
        "maker_share": (sum(f.quantity for f in fills if f.maker) / filled_qty) if filled_qty else 0.0,
        "fill_ratio": (filled_qty / sent_qty) if sent_qty else 0.0,
        "max_drawdown": drawdown,
    }


def run_backtest(strategy: Strategy, config: BacktestConfig, mbo: Optional[dict] = None) -> BacktestResult:
    """Replay `config.symbol` on `config.date` with `strategy` trading.

    `mbo` may pass already-loaded columns (matching_engine.replay.load_mbo); by
    default they are read from config.data_dir.
    """
    timings: dict[str, float] = {}
    started = time.perf_counter()
    if mbo is None:
        mbo_path, _ = replay.data_paths(config.date, config.symbol, config.data_dir)
        mbo = replay.load_mbo(mbo_path)
    timings["load_s"] = time.perf_counter() - started

    sim = Simulator(config.sim_config())
    ctx = Context(sim, config.symbol)
    started_flag = False

    def on_timer() -> None:
        nonlocal started_flag
        ctx._advance()
        if not started_flag:
            started_flag = True
            strategy.on_start(ctx)
        strategy.on_timer(ctx)

    replay_started = time.perf_counter()
    sim.run(mbo, on_timer)
    timings["replay_s"] = time.perf_counter() - replay_started

    ctx._advance()
    strategy.on_end(ctx)

    fills = [_fill(f) for f in sim.fills(0)]
    orders = [_order(t) for t in sim.orders(False)]
    equity = []
    for ts, position, cash_ticks, mid_x2, fees in sim.equity_curve():
        mid = mid_x2 / 2 / TICKS_PER_DOLLAR if mid_x2 else None
        value = to_dollars(cash_ticks) + (position * mid if mid is not None else 0.0) - fees
        equity.append((ts, position, to_dollars(cash_ticks), mid, value))

    stats = sim.stats()
    return BacktestResult(config, _summarise(sim, fills, orders, equity), fills, orders, equity, stats, timings)
