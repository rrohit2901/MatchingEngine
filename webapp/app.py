"""Backtest a strategy on a replayed Nasdaq day, in the browser.

    streamlit run webapp/app.py

Every non-strategy setting is in the sidebar; the strategy's code and its
parameters are in the main area. A run executes in its own limited process
(webapp/runner.py) and its results appear on this page when it finishes.

Environment:
    ME_DATA_DIR   converted data (default: data/databento in the repo)
"""

from __future__ import annotations

import io
import os
import sys
import threading
import time
from datetime import datetime, time as Time, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

import pandas as pd
import streamlit as st

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))   # `streamlit run` puts webapp/ on the path, not the repo

from webapp.runner import Runner, RunResult, Settings, Status, available_data   # noqa: E402

try:
    import tomllib
except ModuleNotFoundError:   # Python < 3.11
    import tomli as tomllib

DATA_DIR = Path(os.environ.get("ME_DATA_DIR", REPO / "data" / "databento"))
EXAMPLE_CONFIG = REPO / "strategies" / "ob_alpha.toml"
EXAMPLE_STRATEGY = REPO / "strategies" / "ob_alpha.py"
COOLDOWN_S = 30
NEW_YORK = ZoneInfo("America/New_York")

st.set_page_config(page_title="Nasdaq replay backtester", page_icon="📈", layout="wide")


@st.cache_resource
def runner() -> Runner:
    # One Runner for every visitor: it is the queue (2 runs at a time, 20 waiting).
    return Runner(DATA_DIR)


@st.cache_data
def example_config() -> dict:
    with open(EXAMPLE_CONFIG, "rb") as fh:
        return tomllib.load(fh)


def toml_params(params: dict) -> str:
    from webapp.runner import _toml
    return "\n".join(f"{key} = {_toml(value)}" for key, value in params.items()) + "\n"


# --- a run in the background ---------------------------------------------------

class Job:
    """One run, executed on a thread so that touching a widget (which reruns this
    script) never interrupts it. The thread only sets attributes; the page polls."""

    def __init__(self, settings: Settings, code: str, params: dict):
        self.status = Status("queued")
        self.result: RunResult | None = None
        self.started = time.time()
        self.thread = threading.Thread(target=self._run, args=(settings, code, params), daemon=True)
        self.thread.start()

    def _run(self, settings: Settings, code: str, params: dict) -> None:
        def update(status: Status) -> None:
            self.status = status
        try:
            self.result = runner().run(settings, code, params, update)
        except Exception as exc:   # never leave the page waiting forever
            self.result = RunResult(False, error={"kind": "internal", "message": repr(exc),
                                                  "location": None, "traceback": []})


# --- sidebar: settings -------------------------------------------------------------

def sidebar_settings() -> Settings | None:
    cfg = example_config()
    days = available_data(DATA_DIR)
    sb = st.sidebar
    sb.header("Backtest settings")
    if not days:
        sb.error(f"No converted data found in {DATA_DIR}.")
        return None

    sb.subheader("Data")
    date = sb.selectbox("Day", list(days), index=len(days) - 1)
    symbols = days[date]
    default_symbol = cfg["data"].get("symbol")
    symbol = sb.selectbox("Symbol", symbols, index=symbols.index(default_symbol) if default_symbol in symbols else 0)

    session = cfg.get("session", {})
    sb.subheader("Trading window (New York)")
    c1, c2 = sb.columns(2)
    start = c1.time_input("Start", Time.fromisoformat(session.get("start", "09:31:00")), step=60)
    end = c2.time_input("End", Time.fromisoformat(session.get("end", "15:59:00")), step=60)
    timer_ms = sb.number_input("Strategy timer (ms)", 1.0, 60_000.0, float(session.get("timer_ms", 10)), step=1.0,
                               help="How often on_timer runs, in exchange time.")

    latency = cfg.get("latency", {})
    sb.subheader("Latency")
    c1, c2 = sb.columns(2)
    order_us = c1.number_input("Order (µs)", 0.0, 1_000_000.0, float(latency.get("order_us", 50)), step=10.0,
                               help="Strategy → exchange.")
    md_us = c2.number_input("Market data (µs)", 0.0, 1_000_000.0, float(latency.get("market_data_us", 20)), step=10.0,
                            help="Exchange → strategy. Adds to the order delay.")

    risk = cfg.get("risk", {})
    sb.subheader("Risk limits")
    max_position = sb.number_input("Max position (shares, 0 = none)", 0, 10_000_000, int(risk.get("max_position", 0)),
                                   step=100, help="Long or short, counting every open order as if filled.")
    max_capital = sb.number_input("Max capital per side ($, 0 = none)", 0.0, 1e10, float(risk.get("max_capital", 0)),
                                  step=10_000.0, help="Position at the current mid plus open orders at their prices.")
    c1, c2 = sb.columns(2)
    max_qty = c1.number_input("Max order qty", 1, 10_000_000, int(risk.get("max_order_qty", 100_000)), step=100)
    min_qty = c2.number_input("Min order qty", 1, 10_000_000, int(risk.get("min_order_qty", 1)), step=1)
    max_dev = sb.number_input("Max price deviation ($)", 0.01, 10_000.0, float(risk.get("max_price_deviation", 1.0)),
                              step=0.05, help="From the same side's best price; checked when an order arrives.")

    fees = cfg.get("fees", {})
    sb.subheader("Fees ($ per share, negative = rebate)")
    c1, c2 = sb.columns(2)
    maker = c1.number_input("Maker", -0.1, 0.1, float(fees.get("maker", 0.0)), step=0.0001, format="%.4f")
    taker = c2.number_input("Taker", -0.1, 0.1, float(fees.get("taker", 0.0)), step=0.0001, format="%.4f")

    model = cfg.get("model", {})
    sb.subheader("Replay model")
    passive = sb.checkbox("Passive impact", bool(model.get("passive_impact", True)),
                          help="Passive fills come out of the venue order queued behind the strategy.")
    stp = sb.checkbox("Self-trade prevention", bool(model.get("self_trade_prevention", False)),
                      help="Off (Nasdaq's default): the strategy's own orders can trade with each other.")
    pnl_ms = sb.number_input("PnL sample interval (ms)", 100.0, 3_600_000.0,
                             float(cfg.get("output", {}).get("pnl_sample_ms", 1_000)), step=100.0)

    return Settings(date=date, symbol=symbol, start=start.strftime("%H:%M:%S"), end=end.strftime("%H:%M:%S"),
                    timer_ms=timer_ms, order_latency_us=order_us, md_latency_us=md_us,
                    max_position=int(max_position), max_capital=max_capital, max_order_qty=int(max_qty),
                    min_order_qty=int(min_qty), max_price_deviation=max_dev, maker_fee=maker, taker_fee=taker,
                    passive_impact=passive, self_trade_prevention=stp, pnl_sample_ms=pnl_ms)


# --- main area: strategy ------------------------------------------------------------

STRATEGY_HELP = """
Subclass `Strategy` and override `on_timer(ctx)` (and optionally `on_start`, `on_end`).
Constructor keyword arguments are the parameters below.

| | |
|---|---|
| `ctx.book(levels=5)` | `Book(bids, asks)` of `Level(price, quantity)`, best first |
| `ctx.best_bid`, `ctx.best_ask`, `ctx.mid` | best levels, or `None` |
| `ctx.position`, `ctx.cash`, `ctx.pnl` | shares; dollars; cash + position × mid − fees |
| `ctx.capital_deployed("BUY")` | dollars on a side, as the capital limit measures it |
| `ctx.fills` | fills since the previous call |
| `ctx.open_orders`, `ctx.order(id)` | `Order`s: status, filled, remaining, … |
| `ctx.buy(price, qty)`, `ctx.sell(price, qty)` | limit orders on the one-cent grid; return an id |
| `ctx.cancel(id)`, `ctx.cancel_all()` | cancels travel with the latency too |
| `ctx.now`, `ctx.time` | exchange time (UTC ns / New York datetime) |

Orders reach the exchange after market-data + order latency. Rejections show up in
`ctx.order(id).reject_reason`. Full guide: `docs/backtesting.md`.
"""


def strategy_inputs() -> tuple[str, str]:
    if "code" not in st.session_state:
        st.session_state.code = EXAMPLE_STRATEGY.read_text()
        st.session_state.params = toml_params(example_config().get("strategy", {}).get("params", {}))

    uploaded = st.file_uploader("Load a strategy file (.py)", type=["py"])
    if uploaded is not None and st.session_state.get("uploaded_name") != uploaded.name + str(uploaded.size):
        st.session_state.code = uploaded.getvalue().decode(errors="replace")
        st.session_state.uploaded_name = uploaded.name + str(uploaded.size)

    left, right = st.columns([3, 1])
    with left:
        code = st.text_area("Strategy code", key="code", height=460)
    with right:
        params = st.text_area("Parameters (TOML)", key="params", height=460,
                              help="Passed to your Strategy's constructor as keyword arguments.")
    with st.expander("How to write a strategy"):
        st.markdown(STRATEGY_HELP)
    return code, params


# --- results ---------------------------------------------------------------------

RECONCILIATION = [
    ("ahead_fill_qty", "Shares the strategy got because it was queued ahead of an order a real execution hit."),
    ("crossing_add_qty", "Shares of real incoming orders that traded with the strategy's resting quotes."),
    ("sweep_qty", "Real fill quantity that had to move down the queue because the strategy took the order."),
    ("self_trade_qty", "Shares the strategy traded with itself (only when self-trade prevention is off)."),
    ("orphaned_orders", "Real orders kept alive by passive impact after Nasdaq retired them."),
    ("orphaned_qty", "Shares those orphaned orders still hold in the book."),
    ("clamped_cancels", "Real cancels larger than what the strategy had left of the order."),
    ("unfilled_venue_qty", "Real fill quantity with no liquidity left at its price in the replayed book."),
]

ERROR_TITLES = {
    "invalid": "Some settings need fixing",
    "busy": "The server is busy",
    "config": "The run's configuration was rejected",
    "strategy_load": "Your strategy file failed to load",
    "strategy_init": "Your strategy failed to start (check the parameters)",
    "strategy_runtime": "Your strategy raised an error while running",
    "time_limit": "Time limit reached",
    "cpu_limit": "CPU limit reached",
    "memory_limit": "Memory limit reached",
    "output_limit": "Output limit reached",
    "internal": "Something went wrong on the server",
}


def ny(ts_ns: pd.Series) -> pd.Series:
    return pd.to_datetime(ts_ns, unit="ns", utc=True).dt.tz_convert(NEW_YORK).dt.tz_localize(None)


def show_error(result: RunResult) -> None:
    err = result.error or {}
    st.error(f"**{ERROR_TITLES.get(err.get('kind'), 'The run failed')}**\n\n{err.get('message', '')}")
    if err.get("location"):
        st.markdown(f"At line **{err['location']['line']}**:")
        st.code(err["location"].get("text") or "", language="python")
    if err.get("traceback"):
        st.code("\n".join(err["traceback"]), language="text")
    if result.log.strip():
        with st.expander("Output of the run"):
            st.code(result.log, language="text")


def show_result(result: RunResult) -> None:
    if not result.ok:
        show_error(result)
        return
    r = result.result
    s, cfg = r["summary"], r["config"]
    st.subheader(f"{r['strategy']['name']} on {cfg['symbol']}, {cfg['date']}")
    st.caption(f"Window {cfg['start']}–{cfg['end']} New York · timer {cfg['timer_ms']:g} ms · latency "
               f"{cfg['order_latency_us']:g} + {cfg['md_latency_us']:g} µs · passive impact "
               f"{'on' if cfg['passive_impact'] else 'off'} · self-trade prevention "
               f"{'on' if cfg['self_trade_prevention'] else 'off'} · run {result.run_s:.1f} s, "
               f"waited {result.queue_wait_s:.1f} s, peak {result.peak_mb} MB")

    cols = st.columns(6)
    cols[0].metric("PnL (marked to mid)", f"${s['pnl']:,.2f}")
    cols[1].metric("Max drawdown", f"${s['max_drawdown']:,.2f}")
    cols[2].metric("Final position", f"{s['final_position']:,} sh")
    cols[3].metric("Fills", f"{s['fills']:,}")
    cols[4].metric("Fill ratio", f"{s['fill_ratio']:.1%}")
    cols[5].metric("Maker share", f"{s['maker_share']:.1%}")
    cols = st.columns(6)
    cols[0].metric("Fees", f"${s['fees']:,.2f}")
    cols[1].metric("Bought / sold", f"{s['bought']:,} / {s['sold']:,}")
    cols[2].metric("Notional traded", f"${s['notional']:,.0f}")
    cols[3].metric("Orders", f"{s['orders']:,}")
    cols[4].metric("Rejected", f"{s['rejected']:,}")
    cols[5].metric("Max long / short", f"{s['max_long']:,} / {s['max_short']:,}")

    equity = pd.DataFrame(r["equity"], columns=["ts", "position", "cash", "mid", "equity"])
    if not equity.empty:
        equity["time"] = ny(equity["ts"])
        c1, c2 = st.columns(2)
        with c1:
            st.markdown("**Equity ($, marked to mid)**")
            st.line_chart(equity, x="time", y="equity", height=260)
        with c2:
            st.markdown("**Position (shares)**")
            st.line_chart(equity, x="time", y="position", height=260)
        st.markdown("**Mid price ($)**")
        st.line_chart(equity.dropna(subset=["mid"]), x="time", y="mid", height=200)

    c1, c2 = st.columns(2)
    with c1:
        st.markdown("**Filled shares by source**")
        if r["filled_by_source"]:
            st.bar_chart(pd.Series(r["filled_by_source"], name="shares"), height=220)
        else:
            st.caption("No fills.")
    with c2:
        st.markdown("**Rejected orders by reason**")
        if r["rejects"]:
            st.dataframe(pd.DataFrame(list(r["rejects"].items()), columns=["reason", "orders"]), hide_index=True)
        else:
            st.caption("None rejected.")

    with st.expander("Replay reconciliation: how the real records were matched to the changed book"):
        stats = r["stats"]
        rows = [(name, f"{stats[name]:,}", meaning) for name, meaning in RECONCILIATION]
        rows.append(("venue_anomalies", f"{r['venue_anomalies']:,}", "Real records the replay could not apply. Should be 0."))
        st.dataframe(pd.DataFrame(rows, columns=["counter", "value", "meaning"]), hide_index=True, use_container_width=True)

    fills = pd.read_csv(io.BytesIO(result.fills_csv))
    orders = pd.read_csv(io.BytesIO(result.orders_csv))
    tab_fills, tab_orders, tab_more = st.tabs([f"Fills ({len(fills):,})", f"Orders ({len(orders):,})", "Run details"])
    with tab_fills:
        st.dataframe(fills.head(1_000), hide_index=True, use_container_width=True)
        st.download_button("Download all fills (CSV)", result.fills_csv, "fills.csv", "text/csv")
    with tab_orders:
        st.dataframe(orders.head(1_000), hide_index=True, use_container_width=True)
        st.download_button("Download all orders (CSV)", result.orders_csv, "orders.csv", "text/csv")
    with tab_more:
        st.json({"settings": cfg, "strategy": r["strategy"], "timings": r["timings"]}, expanded=False)
        if result.log.strip():
            st.markdown("**What the strategy printed**")
            st.code(result.log, language="text")


# --- page ----------------------------------------------------------------------------

def main() -> None:
    st.title("Nasdaq replay backtester")
    st.caption("Your strategy trades against a full day of real Nasdaq order-by-order data (Databento MBO), "
               "replayed through a C++ matching engine. Results appear below when the run finishes, "
               "usually in under 30 seconds.")

    settings = sidebar_settings()
    code, params_text = strategy_inputs()

    job: Job | None = st.session_state.get("job")
    running, waiting = runner().queue_length()
    c1, c2 = st.columns([1, 4])
    clicked = c1.button("Run backtest", type="primary", disabled=job is not None or settings is None,
                        use_container_width=True)
    c2.caption(f"Server: {running} running, {waiting} waiting. One run per visitor at a time, "
               f"{COOLDOWN_S} s apart; each run is limited to 120 s and 1.5 GB.")

    if clicked and job is None and settings is not None:
        since = time.time() - st.session_state.get("last_finished", 0)
        try:
            params = tomllib.loads(params_text)
        except tomllib.TOMLDecodeError as exc:
            st.error(f"The parameters are not valid TOML: {exc}")
        else:
            if since < COOLDOWN_S:
                st.warning(f"Please wait {COOLDOWN_S - since:.0f} s before the next run.")
            else:
                st.session_state.job = Job(settings, code, params)
                st.session_state.pop("result", None)
                st.rerun()

    progress_panel()
    if "result" in st.session_state:
        show_result(st.session_state.result)


@st.fragment(run_every=1.0)
def progress_panel() -> None:
    job: Job | None = st.session_state.get("job")
    if job is None:
        return
    if job.result is not None:   # finished: hand over and redraw the whole page with the results
        st.session_state.result = job.result
        st.session_state.last_finished = time.time()
        del st.session_state["job"]
        st.rerun(scope="app")
        return
    status = job.status
    if status.state == "queued":
        ahead = status.position - 1
        st.info("Waiting for a free slot: you're next." if ahead == 0 else
                f"Waiting for a free slot: {ahead} run{'s' if ahead > 1 else ''} ahead of you.")
    else:
        st.progress(status.fraction, text=f"Running: {status.fraction:.0%} of the trading window replayed, "
                                          f"{time.time() - job.started:.0f} s elapsed")


main()
