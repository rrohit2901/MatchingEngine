"""webapp.runner with the local backend: real child processes on a tiny synthetic day."""

import textwrap
import threading
import time

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from test_backtest import BOOK, DATE, tape
from webapp.runner import (Limits, RateLimiter, RateLimits, Runner, Settings, available_data, run_config,
                           validate)

GOOD = textwrap.dedent("""
    from matching_engine.backtest import Strategy

    class Lift(Strategy):
        def __init__(self, qty=5):
            super().__init__(qty=qty)
            self.qty = qty

        def on_timer(self, ctx):
            if not getattr(self, "sent", False):
                self.sent = True
                print("lifting", self.qty)
                ctx.buy(ctx.best_ask.price, self.qty)
""")


def strategy(body: str) -> str:
    return "from matching_engine.backtest import Strategy\n\nclass S(Strategy):\n" + textwrap.indent(textwrap.dedent(body), "    ")


@pytest.fixture
def data_dir(tmp_path):
    day = tmp_path / "data" / DATE
    day.mkdir(parents=True)
    pq.write_table(pa.table(tape(BOOK + [(59_000, "N", "N", 0, 0, 0, True)])), day / "TEST.mbo.parquet")
    return tmp_path / "data"


def settings(**overrides) -> Settings:
    base = dict(date=DATE, symbol="TEST", start="09:31:00", end="09:32:00", timer_ms=100)
    return Settings(**{**base, **overrides})


def test_available_data(data_dir):
    assert available_data(data_dir) == {DATE: ["TEST"]}


def test_successful_run_returns_results_and_log(data_dir, tmp_path):
    statuses = []
    result = Runner(data_dir, work_dir=tmp_path).run(settings(), GOOD, {"qty": 7}, statuses.append)

    assert result.ok, result.error
    assert result.result["summary"]["final_position"] == 7
    assert result.fills_csv.decode().count("\n") == 2
    assert "lifting 7" in result.log
    assert result.peak_mb > 0
    assert [s.state for s in statuses][0] == "running" and statuses[-1].state == "finished"
    assert not list(tmp_path.glob("me-run-*"))   # the run directory is cleaned up


def test_strategy_error_is_passed_through(data_dir):
    result = Runner(data_dir).run(settings(), strategy("""
        def on_timer(self, ctx):
            raise ValueError("bad idea")
    """), {})
    assert not result.ok
    assert result.error["kind"] == "strategy_runtime"
    assert result.error["message"] == "ValueError: bad idea"


@pytest.mark.parametrize("body, limits, kind", [
    ("""
        def on_timer(self, ctx):
            time.sleep(10)
     """, Limits(wall_s=3, cpu_s=60), "time_limit"),
    ("""
        def on_timer(self, ctx):
            while True:
                pass
     """, Limits(wall_s=30, cpu_s=2), "cpu_limit"),
    ("""
        def on_timer(self, ctx):
            hog = []
            while True:
                hog.append(bytearray(20_000_000))
     """, Limits(wall_s=30, cpu_s=60, memory_mb=400), "memory_limit"),
])
def test_limits_stop_the_run(data_dir, body, limits, kind):
    started = time.monotonic()
    result = Runner(data_dir, limits=limits).run(settings(), "import time\n" + strategy(body), {})
    assert not result.ok
    assert result.error["kind"] == kind, result.error
    assert time.monotonic() - started < 25


def test_runs_queue_in_arrival_order(data_dir):
    runner = Runner(data_dir, max_concurrent=1)
    slow = strategy("""
        def on_start(self, ctx):
            import time
            time.sleep(2)
    """)
    first_started = threading.Event()
    seen = []

    def first():
        runner.run(settings(), slow, {}, lambda s: first_started.set() if s.state == "running" else None)

    t = threading.Thread(target=first)
    t.start()
    first_started.wait(10)
    second = runner.run(settings(), GOOD, {}, seen.append)
    t.join()

    assert second.ok
    assert seen[0].state == "queued" and seen[0].position == 1
    assert "running" in [s.state for s in seen]


def test_queue_limit(data_dir):
    runner = Runner(data_dir, max_concurrent=1, max_queue=0)
    result = runner.run(settings(), GOOD, {})
    assert result.error["kind"] == "busy"


@pytest.mark.parametrize("change, fragment", [
    (dict(date="2020-01-01"), "No data for 2020-01-01"),
    (dict(symbol="MSFT"), "No MSFT data"),
    (dict(start="10:00:00", end="09:00:00"), "04:00:00 <= start < end"),
    (dict(timer_ms=0), "Timer interval"),
    (dict(maker_fee=float("nan")), "Maker fee"),
])
def test_validation(data_dir, change, fragment):
    errors = validate(settings(**change), GOOD, {}, data_dir, Limits())
    assert any(fragment in e for e in errors), errors


def test_validation_of_code_and_params(data_dir):
    import datetime
    assert any("over 1 KB" in e for e in validate(settings(), "x" * 2_000, {}, data_dir, Limits(code_kb=1)))
    assert any("plain values" in e for e in validate(settings(), GOOD, {"when": datetime.date.today()}, data_dir, Limits()))


def test_generated_config_round_trips(data_dir):
    try:
        import tomllib
    except ModuleNotFoundError:
        import tomli as tomllib
    params = {"side": "both", "weights": [5, 4.5], "nested": {"a": True}, "quote\"d": "x\\y"}
    doc = tomllib.loads(run_config(settings(max_capital=1e5), params, data_dir))
    assert doc["strategy"]["params"] == params
    assert doc["risk"]["max_capital"] == 1e5
    assert doc["model"]["passive_impact"] is True


# --- per-client rate limits ------------------------------------------------------

def test_rate_limiter_one_active_run_per_client():
    rl = RateLimiter(RateLimits(max_active=1, cooldown_s=0, per_hour=0))
    assert rl.acquire("a", now=0) is None
    assert "One at a time" in rl.acquire("a", now=1)
    assert rl.acquire("b", now=1) is None          # other clients are unaffected
    rl.release("a", now=2)
    assert rl.acquire("a", now=3) is None


def test_rate_limiter_cooldown_after_a_run():
    rl = RateLimiter(RateLimits(max_active=0, cooldown_s=30, per_hour=0))
    assert rl.acquire("a", now=0) is None
    rl.release("a", now=10)
    assert rl.acquire("a", now=25) == "Please wait 15 s before the next run."
    assert rl.acquire("a", now=40) is None


def test_rate_limiter_runs_per_rolling_hour():
    rl = RateLimiter(RateLimits(max_active=0, cooldown_s=0, per_hour=3))
    for t in (0, 100, 200):
        assert rl.acquire("a", now=t) is None
        rl.release("a", now=t + 1)
    refusal = rl.acquire("a", now=300)
    assert "3 runs in the last hour" in refusal and "55 min" in refusal
    assert rl.acquire("a", now=3_600) is None       # the first run has rolled out of the hour


def test_rate_limiter_forgets_idle_clients():
    rl = RateLimiter(RateLimits(max_active=1, cooldown_s=0, per_hour=5))
    for i in range(1_200):
        rl.acquire(f"ip{i}", now=0)
        rl.release(f"ip{i}", now=1)
    rl.acquire("late", now=7_200)                  # an hour later: the idle ones are pruned
    assert len(rl._starts) < 10


def test_runner_refuses_a_limited_client(data_dir):
    runner = Runner(data_dir, rate_limits=RateLimits(max_active=1, cooldown_s=60, per_hour=10))
    first = runner.run(settings(), GOOD, {}, client="1.2.3.4")
    second = runner.run(settings(), GOOD, {}, client="1.2.3.4")
    other = runner.run(settings(), GOOD, {}, client="5.6.7.8")
    assert first.ok and other.ok
    assert second.error["kind"] == "rate_limited" and "Please wait" in second.error["message"]
    # Invalid requests never count against the limits.
    bad = runner.run(settings(date="2020-01-01"), GOOD, {}, client="9.9.9.9")
    assert bad.error["kind"] == "invalid"
    assert runner.run(settings(), GOOD, {}, client="9.9.9.9").ok
