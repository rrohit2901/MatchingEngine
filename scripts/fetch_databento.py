#!/usr/bin/env python3
"""Download one trading day of Nasdaq order-level data from Databento, once.

For each symbol and schema this writes one raw DBN file:

    data/databento/raw/<date>/<SYMBOL>.<schema>.dbn.zst

These raw files are the source of truth; scripts/convert_mbo.py turns them into
Parquet for the replay. A file that already exists is never downloaded again,
so re-running the script costs nothing unless --force is given.

Before anything is downloaded the script prints Databento's own quote (record
count, billable size and USD cost per symbol and schema) and asks for
confirmation. Pass --yes to skip the prompt, or --quote-only to print the
quote and stop (useful when the script runs without a terminal to answer in).

The API key is read from the DATABENTO_API_KEY environment variable.

Usage:
    python3 scripts/fetch_databento.py --date 2026-09-29
    python3 scripts/fetch_databento.py --date 2026-09-29 --symbols AAPL NVDA --schemas mbo
"""

import argparse
import os
import sys
from datetime import date, datetime, time, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

import databento as db
import pandas as pd

DATASET = "XNAS.ITCH"
DEFAULT_SYMBOLS = ["AAPL", "NVDA", "TSLA"]
# mbo drives the replay; mbp-1 (Nasdaq's own top of book) validates the rebuilt book.
DEFAULT_SCHEMAS = ["mbo", "mbp-1"]
NEW_YORK = ZoneInfo("America/New_York")


def session_window(day: date) -> tuple[datetime, datetime]:
    """From UTC midnight of `day` to New York midnight at the end of it.

    Start: Databento places a synthetic snapshot of the full order book at UTC
    midnight in MBO data, so a request starting there begins from a complete
    book (see https://databento.com/docs/schemas-and-data-formats/mbo#snapshots).
    That is 19:00 or 20:00 ET the evening before, ahead of the session.

    End: Nasdaq runs 04:00-20:00 ET, and 20:00 ET is 00:00 or 01:00 UTC the next
    day depending on DST, so a UTC calendar day would cut the evening session
    short. Ending at New York midnight keeps the whole session.
    """
    start = datetime.combine(day, time(0, 0), tzinfo=timezone.utc)
    end = datetime.combine(day + timedelta(days=1), time(0, 0), tzinfo=NEW_YORK)
    return start, end


def raw_path(out_dir: Path, day: date, symbol: str, schema: str) -> Path:
    return out_dir / day.isoformat() / f"{symbol}.{schema}.dbn.zst"


def check_date_available(client: db.Historical, day: date) -> None:
    available = client.metadata.get_dataset_range(dataset=DATASET)
    # Nanosecond ISO strings, which datetime.fromisoformat rejects before Python 3.11.
    first = pd.Timestamp(available["start"]).date()
    last = pd.Timestamp(available["end"]).date()
    # The end of the range is exclusive and may sit inside the requested day, in
    # which case the day is only partially published.
    if not (first <= day and day < last):
        raise SystemExit(
            f"{day} is outside {DATASET}'s available range "
            f"[{available['start']}, {available['end']}). Pick an earlier day."
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--date", required=True, type=date.fromisoformat, help="trading day, YYYY-MM-DD")
    parser.add_argument("--symbols", nargs="+", default=DEFAULT_SYMBOLS)
    parser.add_argument("--schemas", nargs="+", default=DEFAULT_SCHEMAS)
    parser.add_argument("--out", type=Path, default=Path("data/databento/raw"))
    parser.add_argument("--yes", action="store_true", help="skip the cost confirmation prompt")
    parser.add_argument("--force", action="store_true", help="re-download files that already exist")
    parser.add_argument("--quote-only", action="store_true", help="print the cost quote and download nothing")
    args = parser.parse_args()

    if args.date.weekday() >= 5:
        raise SystemExit(f"{args.date} is a weekend; there is no session to download.")
    if not os.environ.get("DATABENTO_API_KEY"):
        raise SystemExit("DATABENTO_API_KEY is not set.")

    client = db.Historical()
    check_date_available(client, args.date)
    start, end = session_window(args.date)

    # Everything still to fetch, so that existing files are neither quoted nor paid for.
    todo = []
    for symbol in args.symbols:
        for schema in args.schemas:
            path = raw_path(args.out, args.date, symbol, schema)
            if path.exists() and not args.force:
                print(f"skip {path} (already downloaded)")
                continue
            todo.append((symbol, schema, path))
    if not todo:
        print("nothing to download")
        return

    print(f"\n{DATASET}  {args.date}  window {start.isoformat()} -> {end.isoformat()}\n")
    print(f"{'symbol':<8}{'schema':<8}{'records':>15}{'size (MB)':>12}{'cost (USD)':>12}")
    total_cost = 0.0
    for symbol, schema, _ in todo:
        query = dict(dataset=DATASET, start=start, end=end, symbols=[symbol], schema=schema)
        records = client.metadata.get_record_count(**query)
        size = client.metadata.get_billable_size(**query)
        cost = client.metadata.get_cost(**query)
        total_cost += cost
        print(f"{symbol:<8}{schema:<8}{records:>15,}{size / 1e6:>12,.1f}{cost:>12,.2f}")
        if records == 0:
            raise SystemExit(f"\nno {schema} records for {symbol} on {args.date}: market holiday or wrong symbol?")
    print(f"{'total':<31}{'':>12}{total_cost:>12,.2f}\n")

    if args.quote_only:
        return
    if not args.yes:
        if input("Download? [y/N] ").strip().lower() != "y":
            raise SystemExit("aborted; nothing downloaded")

    for symbol, schema, path in todo:
        path.parent.mkdir(parents=True, exist_ok=True)
        # Write to a temporary name first, so an interrupted download never
        # leaves a truncated file that a later run would skip as complete.
        partial = path.with_name(path.name + ".partial")
        print(f"downloading {symbol} {schema} -> {path}")
        client.timeseries.get_range(
            dataset=DATASET, start=start, end=end, symbols=[symbol], schema=schema, path=partial,
        )
        partial.replace(path)

    print("\ndone")


if __name__ == "__main__":
    try:
        main()
    except db.BentoError as error:
        sys.exit(f"Databento error: {error}")
