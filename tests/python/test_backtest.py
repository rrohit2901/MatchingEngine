"""Backtest API and me-backtest CLI on a tiny synthetic day (no data files needed)."""

import csv
import textwrap

import numpy as np
import pytest

from matching_engine import cli
from matching_engine.backtest import BacktestConfig, Strategy, run_backtest
from matching_engine.replay import MBO_COLUMNS

F_LAST = 128
DATE = "2026-09-29"
CONFIG = BacktestConfig(date=DATE, symbol="TEST", start="09:31:00", end="09:32:00", timer_ms=100)
T0 = CONFIG.ny_to_utc_ns("09:31:00")
MS = 1_000_000


def tape(records):
    """records: (offset_ms, action, side, dollars, size, order_id, last)."""
    cols = {name: [] for name in MBO_COLUMNS}
    for seq, (ms, action, side, dollars, size, oid, last) in enumerate(records):
        cols["ts_recv"].append(T0 + int(ms * MS))
        cols["sequence"].append(seq)
        cols["action"].append(ord(action))
        cols["side"].append(ord(side))
        cols["price_ticks"].append(round(dollars * 10_000))
        cols["size"].append(size)
        cols["order_id"].append(oid)
        cols["flags"].append(F_LAST if last else 0)
    return {name: np.array(values, dtype=MBO_COLUMNS[name]) for name, values in cols.items()}


def execution(ms, aggressor, resting, dollars, qty, oid):
    return [(ms, "T", aggressor, dollars, qty, 0, False),
            (ms, "F", resting, dollars, qty, oid, False),
            (ms, "C", resting, dollars, qty, oid, True)]


BOOK = [(-1000, "A", "B", 99.99, 100, 1, True), (-1000, "A", "A", 100.01, 100, 2, True)]


class LiftOnce(Strategy):
    def on_timer(self, ctx):
        if not getattr(self, "done", False):
            self.done = True
            ask = ctx.best_ask
            self.order_id = ctx.buy(ask.price, 30)


def test_aggressive_order_fills_and_pnl_marks_at_mid():
    result = run_backtest(LiftOnce(), CONFIG, mbo=tape(BOOK + [(59_000, "N", "N", 0, 0, 0, True)]))
    assert [(f.side, f.price, f.quantity, f.source) for f in result.fills] == [("BUY", 100.01, 30, "AGGRESSIVE")]
    assert result.summary["final_position"] == 30
    # Bought 30 at 100.01, marked at the 100.00 mid.
    assert result.summary["pnl"] == pytest.approx(-0.30)


class JoinBid(Strategy):
    def on_start(self, ctx):
        self.started_at = ctx.time

    def on_timer(self, ctx):
        if not ctx.open_orders and ctx.position == 0:
            ctx.buy(ctx.best_bid.price, 50)


def test_passive_order_is_filled_when_executions_reach_it():
    events = BOOK + [(500, "A", "B", 99.99, 40, 3, True)]           # queued behind the strategy
    events += execution(1_000, "A", "B", 99.99, 100, 1)             # takes the order ahead of us
    events += execution(1_000, "A", "B", 99.99, 20, 3)              # reaches past us: we are filled first
    events += [(59_000, "N", "N", 0, 0, 0, True)]
    strategy = JoinBid()
    result = run_backtest(strategy, CONFIG, mbo=tape(events))

    assert strategy.started_at.strftime("%H:%M:%S") == "09:31:00"
    assert [(f.quantity, f.source, f.maker) for f in result.fills] == [(20, "AHEAD_IN_QUEUE", True)]
    assert result.stats["orphaned_orders"] == 0


def test_cli_runs_a_strategy_file_with_overrides(tmp_path, capsys):
    import pyarrow as pa
    import pyarrow.parquet as pq

    day = tmp_path / "data" / DATE
    day.mkdir(parents=True)
    pq.write_table(pa.table(tape(BOOK + [(59_000, "N", "N", 0, 0, 0, True)])), day / "TEST.mbo.parquet")

    (tmp_path / "lift.py").write_text(textwrap.dedent("""
        from matching_engine.backtest import Strategy

        class Lift(Strategy):
            def __init__(self, qty=10):
                super().__init__(qty=qty)
                self.qty = qty
                self.sent = False

            def on_timer(self, ctx):
                if not self.sent:
                    self.sent = True
                    ctx.buy(ctx.best_ask.price, self.qty)
    """))
    (tmp_path / "bt.toml").write_text(textwrap.dedent(f"""
        [data]
        date = "{DATE}"
        symbol = "TEST"
        dir = "{tmp_path / 'data'}"
        [session]
        start = "09:31:00"
        end = "09:32:00"
        timer_ms = 100
        [strategy]
        path = "{tmp_path / 'lift.py'}"
        [strategy.params]
        qty = 10
    """))
    fills_csv = tmp_path / "fills.csv"
    code = cli.main(["run", "--config", str(tmp_path / "bt.toml"), "--param", "qty=25",
                     "--order-latency-us", "5", "--fills-csv", str(fills_csv)])

    assert code == 0
    out = capsys.readouterr().out
    assert "Lift on TEST" in out and "qty=25" in out
    rows = list(csv.DictReader(open(fills_csv)))
    assert [(r["side"], r["quantity"], r["source"]) for r in rows] == [("BUY", "25", "AGGRESSIVE")]


def test_strategy_file_must_name_one_class(tmp_path):
    path = tmp_path / "two.py"
    path.write_text("from matching_engine.backtest import Strategy\nclass A(Strategy): pass\nclass B(Strategy): pass\n")
    with pytest.raises(SystemExit, match="exactly one"):
        cli.load_strategy_class(path, None)
    assert cli.load_strategy_class(path, "B").__name__ == "B"


def test_off_grid_price_is_rejected_not_rounded():
    class OffGrid(Strategy):
        def on_timer(self, ctx):
            if not getattr(self, "id", None):
                self.id = ctx.buy(99.995, 10)
                self.status = ctx.order(self.id)

    strategy = OffGrid()
    run_backtest(strategy, CONFIG, mbo=tape(BOOK + [(59_000, "N", "N", 0, 0, 0, True)]))
    assert (strategy.status.status, strategy.status.reject_reason) == ("REJECTED", "PRICE_INCREMENT")


def test_capital_limit_rejects_in_dollars():
    class Spend(Strategy):
        def on_timer(self, ctx):
            if not getattr(self, "ids", None):
                bid = ctx.best_bid.price
                self.ids = [ctx.buy(bid, 9), ctx.buy(bid, 2)]   # $899.91, then $199.98 more
                self.deployed = ctx.capital_deployed("BUY")
                self.orders = [ctx.order(i) for i in self.ids]

    config = BacktestConfig(date=DATE, symbol="TEST", start="09:31:00", end="09:32:00", timer_ms=100,
                            max_capital=1_000.0)
    strategy = Spend()
    run_backtest(strategy, config, mbo=tape(BOOK + [(59_000, "N", "N", 0, 0, 0, True)]))
    assert strategy.orders[0].status == "PENDING"
    assert (strategy.orders[1].status, strategy.orders[1].reject_reason) == ("REJECTED", "CAPITAL_LIMIT")
    assert strategy.deployed == pytest.approx(899.91)


# --- me-backtest run --out: machine-readable results and failures ---------------

def _day(tmp_path):
    import pyarrow as pa
    import pyarrow.parquet as pq

    day = tmp_path / "data" / DATE
    day.mkdir(parents=True)
    pq.write_table(pa.table(tape(BOOK + [(59_000, "N", "N", 0, 0, 0, True)])), day / "TEST.mbo.parquet")
    return tmp_path / "data"


def _run_out(tmp_path, strategy_source, extra=()):
    import json

    data = _day(tmp_path)
    strategy = tmp_path / "s.py"
    strategy.write_text(textwrap.dedent(strategy_source))
    out = tmp_path / "out"
    code = cli.main(["run", "--strategy", str(strategy), "--date", DATE, "--symbol", "TEST", "--data", str(data),
                     "--start", "09:31:00", "--end", "09:32:00", "--timer-ms", "100",
                     "--out", str(out), "--progress", str(tmp_path / "progress.json"), "--quiet", *extra])
    return code, json.loads((out / "result.json").read_text()), out


GOOD = """
    from matching_engine.backtest import Strategy

    class Lift(Strategy):
        def on_timer(self, ctx):
            if not getattr(self, "sent", False):
                self.sent = True
                ctx.buy(ctx.best_ask.price, 5)
"""


def test_out_writes_results_and_progress(tmp_path):
    import json

    code, result, out = _run_out(tmp_path, GOOD)
    assert code == 0 and result["ok"]
    assert result["summary"]["final_position"] == 5
    assert result["filled_by_source"] == {"AGGRESSIVE": 5}
    assert result["strategy"]["name"] == "Lift"
    assert result["equity"] and len(result["equity"][0]) == 5
    assert (out / "fills.csv").read_text().count("\n") == 2   # header + 1 fill
    assert json.loads((tmp_path / "progress.json").read_text())["fraction"] == 1.0


@pytest.mark.parametrize("source, kind, message, line", [
    ("from matching_engine.backtest import Strategy\nclass S(Strategy)\n    pass\n",
     "strategy_load", "SyntaxError", 2),
    ("from matching_engine.backtest import Strategy\nclass S(Strategy):\n"
     "    def __init__(self, size):\n        super().__init__(size=size)\n",
     "strategy_init", "TypeError", None),
    ("from matching_engine.backtest import Strategy\nclass S(Strategy):\n"
     "    def on_timer(self, ctx):\n        x = 1\n        return 1 / 0\n",
     "strategy_runtime", "ZeroDivisionError", 5),
])
def test_out_reports_strategy_failures(tmp_path, source, kind, message, line):
    code, result, _ = _run_out(tmp_path, source)
    assert code == 2 and not result["ok"]
    err = result["error"]
    assert err["kind"] == kind
    assert err["message"].startswith(message)
    if line is not None:
        assert err["location"]["line"] == line
    # Only the strategy's own frames, never the backtester's internals.
    assert all("matching_engine" not in t for t in err["traceback"]) or kind == "strategy_init"


def test_out_reports_config_failures(tmp_path):
    code, result, _ = _run_out(tmp_path, GOOD, extra=["--end", "09:30:00"])   # window ends before it starts
    assert code == 2
    assert result["error"]["kind"] == "config"
    assert "empty" in result["error"]["message"]
