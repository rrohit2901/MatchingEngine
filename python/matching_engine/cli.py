"""me-backtest: run a strategy on a replayed Nasdaq day from the command line.

    me-backtest run --config strategies/ob_alpha.toml
    me-backtest run --config strategies/ob_alpha.toml --symbol NVDA --order-latency-us 100
    me-backtest run --strategy my_strategy.py --date 2026-09-29 --symbol AAPL --param size=200
    me-backtest validate --date 2026-09-29 --symbols AAPL NVDA TSLA

`run` takes its settings from a TOML file (see strategies/ob_alpha.toml for every
key) and lets flags override any of them. The strategy file must define exactly
one subclass of matching_engine.backtest.Strategy (or name one with
--strategy-class).
"""

from __future__ import annotations

import argparse
import ast
import csv
import importlib.util
import inspect
import json
import os
import resource
import sys
import time
import traceback
from dataclasses import asdict, fields
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from . import replay
from .backtest import NEW_YORK, BacktestConfig, BacktestResult, Strategy, run_backtest

try:
    import tomllib
except ModuleNotFoundError:   # Python < 3.11
    import tomli as tomllib


# TOML [section] key -> BacktestConfig field
CONFIG_KEYS = {
    ("data", "date"): "date",
    ("data", "symbol"): "symbol",
    ("data", "dir"): "data_dir",
    ("session", "start"): "start",
    ("session", "end"): "end",
    ("session", "timer_ms"): "timer_ms",
    ("latency", "order_us"): "order_latency_us",
    ("latency", "market_data_us"): "md_latency_us",
    ("risk", "max_position"): "max_position",
    ("risk", "max_capital"): "max_capital",
    ("risk", "max_order_qty"): "max_order_qty",
    ("risk", "min_order_qty"): "min_order_qty",
    ("risk", "max_price_deviation"): "max_price_deviation",
    ("fees", "maker"): "maker_fee",
    ("fees", "taker"): "taker_fee",
    ("output", "pnl_sample_ms"): "pnl_sample_ms",
    ("model", "passive_impact"): "passive_impact",
    ("model", "self_trade_prevention"): "self_trade_prevention",
}


def _parse_value(text: str) -> Any:
    try:
        return ast.literal_eval(text)
    except (ValueError, SyntaxError):
        return text


class StrategyLoadError(Exception):
    """The strategy file failed to import; __cause__ is what it raised."""


def load_strategy_class(path: Path, class_name: str | None) -> type[Strategy]:
    spec = importlib.util.spec_from_file_location(f"_strategy_{path.stem}", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot import strategy file {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    except Exception as exc:
        raise StrategyLoadError(str(path)) from exc

    candidates = {
        name: obj for name, obj in vars(module).items()
        if inspect.isclass(obj) and issubclass(obj, Strategy) and obj is not Strategy and obj.__module__ == spec.name
    }
    if class_name:
        if class_name not in candidates:
            raise SystemExit(f"{path} has no Strategy subclass named {class_name}; found {sorted(candidates) or 'none'}")
        return candidates[class_name]
    if len(candidates) != 1:
        raise SystemExit(f"{path} must define exactly one Strategy subclass (found {sorted(candidates) or 'none'}); "
                         "pick one with --strategy-class")
    return next(iter(candidates.values()))


def build_run(args: argparse.Namespace) -> tuple[BacktestConfig, type[Strategy], dict[str, Any], dict[str, Any]]:
    settings: dict[str, Any] = {}
    strategy_path = None
    strategy_class = None
    params: dict[str, Any] = {}
    output: dict[str, Any] = {}
    base = Path.cwd()

    if args.config:
        config_path = Path(args.config)
        with open(config_path, "rb") as fh:
            doc = tomllib.load(fh)
        for (section, key), name in CONFIG_KEYS.items():
            if key in doc.get(section, {}):
                settings[name] = doc[section][key]
        strategy_doc = doc.get("strategy", {})
        if "path" in strategy_doc:
            strategy_path = Path(strategy_doc["path"])
        strategy_class = strategy_doc.get("class")
        params.update(strategy_doc.get("params", {}))
        output.update(doc.get("output", {}))

    # Flags win over the file.
    overrides = {
        "date": args.date, "symbol": args.symbol, "data_dir": args.data, "start": args.start, "end": args.end,
        "timer_ms": args.timer_ms, "order_latency_us": args.order_latency_us, "md_latency_us": args.md_latency_us,
        "max_position": args.max_position, "max_capital": args.max_capital, "passive_impact": args.passive_impact,
        "self_trade_prevention": args.self_trade_prevention,
    }
    settings.update({k: v for k, v in overrides.items() if v is not None})
    if args.strategy:
        strategy_path = Path(args.strategy)
    if args.strategy_class:
        strategy_class = args.strategy_class
    for item in args.param or []:
        key, sep, value = item.partition("=")
        if not sep:
            raise SystemExit(f"--param expects key=value, got {item!r}")
        params[key] = _parse_value(value)
    for key in ("fills_csv", "equity_csv", "orders_csv"):
        if getattr(args, key):
            output[key] = getattr(args, key)

    missing = [k for k in ("date", "symbol") if k not in settings]
    if missing:
        raise SystemExit(f"missing {', '.join(missing)}: set them in [data] or with --{' --'.join(missing)}")
    if strategy_path is None:
        raise SystemExit("no strategy: set [strategy] path in the config or pass --strategy")

    known = {f.name for f in fields(BacktestConfig)}
    config = BacktestConfig(**{k: v for k, v in settings.items() if k in known})
    config.data_dir = Path(config.data_dir)
    if not strategy_path.is_absolute():
        strategy_path = base / strategy_path
    return config, load_strategy_class(strategy_path, strategy_class), params, output


def _ny(ts: int) -> str:
    return datetime.fromtimestamp(ts / 1e9, tz=timezone.utc).astimezone(NEW_YORK).strftime("%H:%M:%S.%f")


def write_outputs(result: BacktestResult, output: dict[str, Any], announce: bool = True) -> None:
    if output.get("fills_csv"):
        with open(output["fills_csv"], "w", newline="") as fh:
            writer = csv.writer(fh)
            writer.writerow(["time_ny", "ts_ns", "order_id", "side", "price", "quantity", "maker", "source"])
            for f in result.fills:
                writer.writerow([_ny(f.ts), f.ts, f.order_id, f.side, f"{f.price:.4f}", f.quantity, int(f.maker), f.source])
        if announce:
            print(f"fills   -> {output['fills_csv']}")
    if output.get("equity_csv"):
        with open(output["equity_csv"], "w", newline="") as fh:
            writer = csv.writer(fh)
            writer.writerow(["time_ny", "ts_ns", "position", "cash", "mid", "equity"])
            for ts, position, cash, mid, equity in result.equity:
                writer.writerow([_ny(ts), ts, position, f"{cash:.4f}", "" if mid is None else f"{mid:.4f}", f"{equity:.4f}"])
        if announce:
            print(f"equity  -> {output['equity_csv']}")
    if output.get("orders_csv"):
        with open(output["orders_csv"], "w", newline="") as fh:
            writer = csv.writer(fh)
            writer.writerow(["order_id", "side", "price", "quantity", "filled", "status", "reject_reason",
                             "sent_ny", "arrival_ny"])
            for o in result.orders:
                writer.writerow([o.order_id, o.side, f"{o.price:.4f}", o.quantity, o.filled, o.status,
                                 o.reject_reason, _ny(o.ts_sent), _ny(o.ts_arrival) if o.ts_arrival else ""])
        if announce:
            print(f"orders  -> {output['orders_csv']}")


def reject_counts(result: BacktestResult) -> dict[str, int]:
    reasons: dict[str, int] = {}
    for o in result.orders:
        if o.status == "REJECTED":
            reasons[o.reject_reason] = reasons.get(o.reject_reason, 0) + 1
    return dict(sorted(reasons.items()))


def filled_by_source(result: BacktestResult) -> dict[str, int]:
    sources: dict[str, int] = {}
    for f in result.fills:
        sources[f.source] = sources.get(f.source, 0) + f.quantity
    return dict(sorted(sources.items()))


PROGRESS_PREFIX = "ME-PROGRESS"

VENUE_ANOMALIES = ("unknown_order", "modify_unknown", "duplicate_add", "cancel_oversized",
                   "bad_side", "bad_price", "unknown_action")


def result_payload(result: BacktestResult, strategy_name: str, params: dict[str, Any]) -> dict[str, Any]:
    """Everything the report shows, as JSON-ready data (written as result.json by --out)."""
    config = asdict(result.config)
    config["data_dir"] = str(config["data_dir"])
    return {
        "ok": True,
        "strategy": {"name": strategy_name, "params": params},
        "config": config,
        "summary": result.summary,
        "rejects": reject_counts(result),
        "filled_by_source": filled_by_source(result),
        "stats": result.stats,
        "venue_anomalies": sum(result.stats["venue"][k] for k in VENUE_ANOMALIES),
        "timings": result.timings,
        "peak_rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024,
        # (ts_ns, position, cash $, mid $ or null, equity $)
        "equity": [list(row) for row in result.equity],
    }


def error_payload(kind: str, exc: BaseException, strategy_path: Path | None) -> dict[str, Any]:
    """A failure the UI can show: what went wrong, located in the strategy's own code.

    `kind` is config, strategy_load, strategy_init or strategy_runtime. The
    traceback keeps only frames from the strategy file, so the reader is not
    walked through the backtester's internals; if there are none (the error
    surfaced inside the library), the last frame is kept instead.
    """
    message = f"{type(exc).__name__}: {exc}" if kind != "config" else str(exc)
    location = None
    if isinstance(exc, SyntaxError) and exc.lineno:
        location = {"line": exc.lineno, "text": (exc.text or "").rstrip()}
    frames = traceback.extract_tb(exc.__traceback__)
    if strategy_path is not None:
        own = [f for f in frames if Path(f.filename).resolve() == strategy_path.resolve()]
    else:
        own = []
    shown = own or frames[-1:]
    lines = [f"line {f.lineno}, in {f.name}: {(f.line or '').strip()}" for f in shown]
    if shown and location is None and own:
        location = {"line": own[-1].lineno, "text": (own[-1].line or "").strip()}
    return {"ok": False, "error": {"kind": kind, "message": message, "location": location,
                                   "traceback": lines if kind != "config" else []}}


def _write_json_atomic(path: Path, data: dict[str, Any]) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, allow_nan=False, default=str))
    os.replace(tmp, path)


def print_report(result: BacktestResult, strategy_name: str, params: dict[str, Any]) -> None:
    c, s, st = result.config, result.summary, result.stats
    print(f"\n{strategy_name} on {c.symbol} {c.date}   window {c.start}-{c.end} ET   timer {c.timer_ms:g} ms   "
          f"latency order {c.order_latency_us:g} us + market data {c.md_latency_us:g} us   "
          f"passive impact {'on' if c.passive_impact else 'off'}   "
          f"self-trade prevention {'on' if c.self_trade_prevention else 'off'}")
    if params:
        print("params  " + "  ".join(f"{k}={v}" for k, v in params.items()))

    rows = [
        ("PnL (mark to mid)", f"${s['pnl']:,.2f}"),
        ("fees", f"${s['fees']:,.2f}"),
        ("max drawdown", f"${s['max_drawdown']:,.2f}"),
        ("final position", f"{s['final_position']:,} sh"),
        ("max long / short", f"{s['max_long']:,} / {s['max_short']:,} sh"),
        ("bought / sold", f"{s['bought']:,} / {s['sold']:,} sh"),
        ("notional traded", f"${s['notional']:,.0f}"),
        ("orders (rejected)", f"{s['orders']:,} ({s['rejected']:,})"),
        ("fills", f"{s['fills']:,}"),
        ("fill ratio", f"{s['fill_ratio']:.1%} of accepted quantity"),
        ("maker share", f"{s['maker_share']:.1%} of filled quantity"),
    ]
    width = max(len(name) for name, _ in rows)
    print()
    for name, value in rows:
        print(f"  {name:<{width}}  {value}")

    if s["rejected"]:
        print("  rejects  " + "  ".join(f"{k}={v:,}" for k, v in reject_counts(result).items()))

    sources = filled_by_source(result)
    if sources:
        print("  filled qty by source  " + "  ".join(f"{k}={v:,}" for k, v in sources.items()))

    print("\nreplay reconciliation (venue records vs the book the strategy changed)")
    print(f"  ahead-of-queue fills {st['ahead_fill_qty']:,}   sweeps {st['sweep_qty']:,}   "
          f"crossing adds {st['crossing_add_qty']:,}   self-trades {st['self_trade_qty']:,}   "
          f"unfilled venue qty {st['unfilled_venue_qty']:,}")
    print(f"  clamped cancels {st['clamped_cancels']:,}   orphaned orders {st['orphaned_orders']:,} "
          f"({st['orphaned_qty']:,} sh)   venue anomalies "
          f"{sum(st['venue'][k] for k in VENUE_ANOMALIES):,}")
    t = result.timings
    print(f"\n{st['venue']['records']:,} venue records, {st['timer_calls']:,} strategy calls   "
          f"load {t.get('load_s', 0):.1f}s   replay {t.get('replay_s', 0):.1f}s")


def _strategy_path(args: argparse.Namespace) -> Path | None:
    if args.strategy:
        return Path(args.strategy)
    if args.config:
        try:
            with open(args.config, "rb") as fh:
                path = tomllib.load(fh).get("strategy", {}).get("path")
            return Path(path) if path else None
        except (OSError, tomllib.TOMLDecodeError):
            return None
    return None


def cmd_run(args: argparse.Namespace) -> int:
    out = Path(args.out) if args.out else None
    if out is not None:
        out.mkdir(parents=True, exist_ok=True)
    strategy_path = _strategy_path(args)

    def fail(kind: str, exc: BaseException) -> int:
        payload = error_payload(kind, exc, strategy_path)
        if out is not None:
            _write_json_atomic(out / "result.json", payload)
        err = payload["error"]
        print(f"error ({err['kind']}): {err['message']}", file=sys.stderr)
        for line in err["traceback"]:
            print(f"  {line}", file=sys.stderr)
        return 2

    # Each phase fails differently: a bad config, a strategy file that does not
    # import, a constructor that rejects its parameters, or code that raises
    # while running.
    try:
        config, strategy_cls, params, output = build_run(args)
        config.sim_config()   # validates times and limits before anything runs
    except StrategyLoadError as exc:
        return fail("strategy_load", exc.__cause__ or exc)
    except (SystemExit, Exception) as exc:   # bad TOML, missing keys, bad values
        return fail("config", exc)
    # SystemExit too: a strategy calling sys.exit() is the strategy's doing, not a crash.
    try:
        strategy = strategy_cls(**params)
    except (Exception, SystemExit) as exc:
        return fail("strategy_init", exc)

    on_progress = None
    if args.progress == "-":
        # Progress as lines on stderr. A sandbox (isolate) makes the run's files
        # unreadable from outside until it ends, but a stream can be read live.
        def on_progress(fraction: float) -> None:
            print(f"{PROGRESS_PREFIX} {fraction:.4f}", file=sys.stderr, flush=True)
    elif args.progress:
        progress_path = Path(args.progress)
        started = time.monotonic()

        def on_progress(fraction: float) -> None:
            _write_json_atomic(progress_path, {"fraction": round(fraction, 4),
                                               "elapsed_s": round(time.monotonic() - started, 2)})
    try:
        result = run_backtest(strategy, config, on_progress=on_progress)
    except (Exception, SystemExit) as exc:
        return fail("strategy_runtime", exc)

    if not args.quiet:
        print_report(result, strategy_cls.__name__, params)
    if out is not None:
        output = {**output, "fills_csv": out / "fills.csv", "orders_csv": out / "orders.csv"}
    write_outputs(result, output, announce=not args.quiet)
    if out is not None:
        _write_json_atomic(out / "result.json", result_payload(result, strategy_cls.__name__, params))
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    check = replay.validate_simulator if args.through_simulator else replay.validate_replay
    clean = True
    for symbol in args.symbols:
        mbo_path, mbp1_path = replay.data_paths(args.date, symbol, args.data)
        result = check(replay.load_mbo(mbo_path), replay.load_mbp1(mbp1_path))
        ok = result["mismatched"] == 0 and result["crossed"] == 0
        clean &= ok
        print(f"{symbol} {args.date}: {'MATCHES' if ok else 'DIFFERS'}   compared {result['compared']:,}   "
              f"mismatched {result['mismatched']:,}   crossed {result['crossed']:,}   "
              f"mbp-1 unmatched {result['venue_unmatched']:,}")
    return 0 if clean else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="me-backtest", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="backtest a strategy on one symbol-day")
    run.add_argument("--config", help="TOML file; flags override its values")
    run.add_argument("--strategy", help="strategy .py file")
    run.add_argument("--strategy-class", help="which Strategy subclass, if the file has several")
    run.add_argument("--param", action="append", metavar="KEY=VALUE", help="strategy parameter (repeatable)")
    run.add_argument("--date")
    run.add_argument("--symbol")
    run.add_argument("--data", type=Path, help=f"converted data directory (default {replay.DEFAULT_DATA_DIR})")
    run.add_argument("--start", help="trading window start, New York time HH:MM:SS")
    run.add_argument("--end", help="trading window end, New York time HH:MM:SS")
    run.add_argument("--timer-ms", type=float)
    run.add_argument("--order-latency-us", type=float)
    run.add_argument("--md-latency-us", type=float)
    run.add_argument("--max-position", type=int)
    run.add_argument("--max-capital", type=float, help="dollars deployed per side; 0 = no limit")
    run.add_argument("--passive-impact", action=argparse.BooleanOptionalAction, default=None,
                     help="take passive strategy fills out of the venue order behind (default on)")
    run.add_argument("--self-trade-prevention", action=argparse.BooleanOptionalAction, default=None,
                     help="reject orders that would trade with the strategy's own (default off: they execute)")
    run.add_argument("--fills-csv")
    run.add_argument("--equity-csv")
    run.add_argument("--orders-csv")
    run.add_argument("--out", help="write result.json, fills.csv and orders.csv into this directory "
                                   "(on failure, result.json holds the error)")
    run.add_argument("--progress", help="keep this JSON file updated with how far the replay is (0-1); "
                                        "'-' prints 'ME-PROGRESS <fraction>' lines on stderr instead")
    run.add_argument("--quiet", action="store_true", help="do not print the report")
    run.set_defaults(func=cmd_run)

    validate = sub.add_parser("validate", help="check the replay rebuilds Nasdaq's book exactly")
    validate.add_argument("--date", required=True)
    validate.add_argument("--symbols", nargs="+", required=True)
    validate.add_argument("--data", type=Path, default=replay.DEFAULT_DATA_DIR)
    validate.add_argument("--through-simulator", action="store_true",
                          help="replay through the strategy Simulator (no strategy) instead of the plain replayer")
    validate.set_defaults(func=cmd_validate)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
