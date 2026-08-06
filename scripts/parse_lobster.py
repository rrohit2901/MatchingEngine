#!/usr/bin/env python3
"""Prepare LOBSTER message files for replay against the matching engine.

Two transformations:
  1. Drop execution messages (event types 4 and 5). A visible execution (4) is
     the counterpart of a trade the engine produces itself, and a hidden
     execution (5) refers to liquidity that never appears in the visible book,
     so neither can be replayed as an input event.
  2. Add the column header, which LOBSTER files ship without.

Reads data/lobster/*.csv and writes data/lobster/parsed/. Originals are left
untouched.

Usage:  python3 scripts/parse_lobster.py [--in DIR] [--out DIR]
"""

import argparse
import csv
from pathlib import Path

# LOBSTER message schema. time is seconds after midnight, price is dollars
# scaled by 10_000 (an integer, matching the engine's integer ticks), and
# direction is 1 for buy, -1 for sell.
HEADER = ["time", "event_type", "order_id", "size", "price", "direction"]

EVENT_NAMES = {
    1: "new limit order",
    2: "partial cancel",
    3: "full delete",
    4: "execution (visible)",
    5: "execution (hidden)",
    6: "trading halt",
    7: "auction trade",
}

DROPPED_EVENT_TYPES = {4, 5}


def parse_file(src: Path, dst: Path) -> tuple[int, int, dict[int, int]]:
    """Copy src to dst with a header, minus execution rows.

    Returns (rows_in, rows_out, count_by_dropped_type).
    """
    rows_in = 0
    rows_out = 0
    dropped: dict[int, int] = {}

    with src.open(newline="") as fin, dst.open("w", newline="") as fout:
        reader = csv.reader(fin)
        writer = csv.writer(fout)
        writer.writerow(HEADER)

        for row in reader:
            if not row:
                continue
            rows_in += 1

            event_type = int(row[1])
            if event_type in DROPPED_EVENT_TYPES:
                dropped[event_type] = dropped.get(event_type, 0) + 1
                continue

            writer.writerow(row)
            rows_out += 1

    return rows_in, rows_out, dropped


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--in", dest="in_dir", default="data/lobster", type=Path)
    parser.add_argument("--out", dest="out_dir", default="data/lobster/parsed", type=Path)
    args = parser.parse_args()

    sources = sorted(p for p in args.in_dir.glob("*.csv") if p.is_file())
    if not sources:
        raise SystemExit(f"no CSV files found in {args.in_dir}")

    args.out_dir.mkdir(parents=True, exist_ok=True)

    total_in = total_out = 0
    for src in sources:
        dst = args.out_dir / src.name
        rows_in, rows_out, dropped = parse_file(src, dst)
        total_in += rows_in
        total_out += rows_out

        detail = ", ".join(
            f"{count:,} {EVENT_NAMES.get(event_type, event_type)}"
            for event_type, count in sorted(dropped.items())
        )
        print(f"{src.name}")
        print(f"  {rows_in:>9,} in -> {rows_out:>9,} out   (dropped {detail or 'nothing'})")

    print(f"\ntotal: {total_in:,} in -> {total_out:,} out "
          f"({total_in - total_out:,} execution rows removed)")
    print(f"written to {args.out_dir}/")


if __name__ == "__main__":
    main()
