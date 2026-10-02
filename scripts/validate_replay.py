#!/usr/bin/env python3
"""Phase 1 check: does the replay rebuild Nasdaq's book exactly?

Replays a converted symbol-day (scripts/convert_mbo.py) through the C++
MarketReplayer and, after every venue event, compares the rebuilt best bid and
ask (price and size) with Nasdaq's own mbp-1 record for the same event. Also
counts crossed books and every record the replay could not apply cleanly.

Exit status is 0 only if nothing mismatched, nothing crossed and no anomaly was
counted.

Usage:
    python3 scripts/validate_replay.py --date 2026-09-29 --symbols AAPL NVDA TSLA
"""

import argparse
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

from matching_engine import replay

ANOMALIES = ["unknown_order", "modify_unknown", "duplicate_add", "cancel_oversized",
             "bad_side", "bad_price", "unknown_action"]
NEW_YORK = ZoneInfo("America/New_York")


def px(ticks: int) -> str:
    return "-" if ticks == replay.PRICE_UNDEF else f"{ticks / 10_000:.4f}"


def ny_time(ts_ns: int) -> str:
    return datetime.fromtimestamp(ts_ns / 1e9, tz=timezone.utc).astimezone(NEW_YORK).strftime("%H:%M:%S.%f")


def report_symbol(date: str, symbol: str, data_dir: Path) -> bool:
    started = time.perf_counter()
    mbo_path, mbp1_path = replay.data_paths(date, symbol, data_dir)
    mbo, mbp1 = replay.load_mbo(mbo_path), replay.load_mbp1(mbp1_path)
    loaded = time.perf_counter()
    result = replay.validate_replay(mbo, mbp1)
    done = time.perf_counter()

    stats = result["replay"]
    anomalies = {k: stats[k] for k in ANOMALIES if stats[k]}
    clean = result["mismatched"] == 0 and result["crossed"] == 0 and not anomalies

    print(f"== {symbol} {date}: {'MATCHES' if clean else 'DIFFERS'} ==")
    print(f"  records {stats['records']:,}   events {stats['events']:,}   "
          f"load {loaded - started:.1f}s   replay+compare {done - loaded:.2f}s "
          f"({stats['records'] / (done - loaded) / 1e6:.1f} M records/s)")
    print(f"  compared with mbp-1 {result['compared']:,}   mismatched {result['mismatched']:,}   "
          f"crossed {result['crossed']:,}   mbp-1 unmatched {result['venue_unmatched']:,}")
    print("  applied   " + "  ".join(f"{k}={stats[k]:,}" for k in
                                     ["adds", "cancels_full", "cancels_partial", "modifies", "clears", "trades", "fills"]))
    print("  anomalies " + ("  ".join(f"{k}={v:,}" for k, v in anomalies.items()) or "none"))
    for e in result["examples"][:10]:
        print(f"    {ny_time(e['ts_recv'])} seq {e['sequence']}: "
              f"book {px(e['book_bid_px'])} x{e['book_bid_sz']} / {px(e['book_ask_px'])} x{e['book_ask_sz']}   "
              f"venue {px(e['venue_bid_px'])} x{e['venue_bid_sz']} / {px(e['venue_ask_px'])} x{e['venue_ask_sz']}")
    return clean


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--date", required=True)
    parser.add_argument("--symbols", nargs="+", required=True)
    parser.add_argument("--data", type=Path, default=replay.DEFAULT_DATA_DIR)
    args = parser.parse_args()

    results = [report_symbol(args.date, symbol, args.data) for symbol in args.symbols]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
