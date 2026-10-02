#!/usr/bin/env python3
"""Convert the raw Databento DBN files into Parquet for the replay.

Reads  data/databento/raw/<date>/<SYMBOL>.<schema>.dbn.zst
Writes data/databento/<date>/<SYMBOL>.mbo.parquet    (from mbo)
       data/databento/<date>/<SYMBOL>.mbp1.parquet   (from mbp-1)

Column choices, made for the C++ replay, which receives these columns as flat
numpy arrays:
  * Prices become int32 ticks of 1e-4 dollars (Databento's are int64 at 1e-9).
    Nasdaq prices are whole multiples of 1e-4, so the conversion is exact; any
    price that is not aborts the conversion rather than being rounded.
    An undefined price (Databento's UNDEF_PRICE, e.g. on a clear or an empty
    side of the book) becomes PRICE_UNDEF = 2**31 - 1.
  * action and side stay single ASCII bytes stored as uint8 ('A' = 65, ...).
  * Timestamps stay uint64 nanoseconds since the UNIX epoch, UTC.
  * Rows keep the original order. Databento orders records by ts_recv, and that
    order is the replay order.

Files are streamed in chunks, so a full day of a liquid symbol never has to fit
in memory. Existing outputs are skipped unless --force is given.

Usage:
    python3 scripts/convert_mbo.py --date 2026-09-29
"""

import argparse
from collections import Counter
from datetime import date
from pathlib import Path

import databento as db
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

UNDEF_PRICE = 2**63 - 1          # Databento's undefined-price sentinel
PRICE_UNDEF = 2**31 - 1          # ours, in int32 ticks
NANOS_PER_TICK = 100_000         # 1e-9 dollars -> 1e-4 dollars
CHUNK_ROWS = 1_000_000

# schema -> (output suffix, price columns to convert)
SCHEMAS = {
    "mbo": ("mbo", ["price"]),
    "mbp-1": ("mbp1", ["price", "bid_px_00", "ask_px_00"]),
}


def to_ticks(prices: np.ndarray, column: str) -> np.ndarray:
    undefined = prices == UNDEF_PRICE
    defined = prices[~undefined]
    if defined.size:
        if np.any(defined % NANOS_PER_TICK):
            bad = defined[defined % NANOS_PER_TICK != 0][0]
            raise ValueError(f"{column}: price {bad} is not a whole number of 1e-4 ticks")
        if defined.min() < 0 or defined.max() // NANOS_PER_TICK >= PRICE_UNDEF:
            raise ValueError(f"{column}: price outside the int32 tick range")
    ticks = np.where(undefined, PRICE_UNDEF, prices // NANOS_PER_TICK)
    return ticks.astype(np.int32)


def char_column(chunk: np.ndarray, name: str) -> np.ndarray:
    return np.ascontiguousarray(chunk[name]).view(np.uint8)


def mbo_table(chunk: np.ndarray) -> pa.Table:
    return pa.table({
        "ts_recv": chunk["ts_recv"],
        "ts_event": chunk["ts_event"],
        "action": char_column(chunk, "action"),
        "side": char_column(chunk, "side"),
        "price_ticks": to_ticks(chunk["price"], "price"),
        "size": chunk["size"],
        "order_id": chunk["order_id"],
        "flags": chunk["flags"],
        "sequence": chunk["sequence"],
    })


def mbp1_table(chunk: np.ndarray) -> pa.Table:
    return pa.table({
        "ts_recv": chunk["ts_recv"],
        "ts_event": chunk["ts_event"],
        "action": char_column(chunk, "action"),
        "side": char_column(chunk, "side"),
        "price_ticks": to_ticks(chunk["price"], "price"),
        "size": chunk["size"],
        "flags": chunk["flags"],
        "sequence": chunk["sequence"],
        "bid_px": to_ticks(chunk["bid_px_00"], "bid_px_00"),
        "ask_px": to_ticks(chunk["ask_px_00"], "ask_px_00"),
        "bid_sz": chunk["bid_sz_00"],
        "ask_sz": chunk["ask_sz_00"],
        "bid_ct": chunk["bid_ct_00"],
        "ask_ct": chunk["ask_ct_00"],
    })


def convert(src: Path, dst: Path, schema: str) -> tuple[int, Counter]:
    store = db.DBNStore.from_file(src)
    if str(store.schema) != schema:
        raise ValueError(f"{src}: expected schema {schema}, file holds {store.schema}")
    build = mbo_table if schema == "mbo" else mbp1_table

    rows = 0
    actions: Counter = Counter()
    partial = dst.with_name(dst.name + ".partial")
    writer = None
    try:
        for chunk in store.to_ndarray(count=CHUNK_ROWS):
            if len(chunk) == 0:
                continue
            table = build(chunk)
            if writer is None:
                writer = pq.ParquetWriter(partial, table.schema, compression="zstd")
            writer.write_table(table)
            rows += len(chunk)
            values, counts = np.unique(chunk["action"], return_counts=True)
            actions.update({v.decode(): int(c) for v, c in zip(values, counts)})
    finally:
        if writer is not None:
            writer.close()

    if writer is None:
        raise ValueError(f"{src}: no records")
    written = pq.ParquetFile(partial).metadata.num_rows
    if written != rows:
        raise ValueError(f"{dst}: wrote {written} rows, read {rows}")
    partial.replace(dst)
    return rows, actions


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--date", required=True, type=date.fromisoformat)
    parser.add_argument("--raw", type=Path, default=Path("data/databento/raw"))
    parser.add_argument("--out", type=Path, default=Path("data/databento"))
    parser.add_argument("--force", action="store_true", help="overwrite existing Parquet files")
    args = parser.parse_args()

    raw_dir = args.raw / args.date.isoformat()
    out_dir = args.out / args.date.isoformat()
    sources = sorted(raw_dir.glob("*.dbn.zst"))
    if not sources:
        raise SystemExit(f"no .dbn.zst files in {raw_dir}; run scripts/fetch_databento.py first")
    out_dir.mkdir(parents=True, exist_ok=True)

    for src in sources:
        symbol, schema = src.name.removesuffix(".dbn.zst").split(".", 1)
        if schema not in SCHEMAS:
            print(f"skip {src.name}: schema {schema} is not converted")
            continue
        dst = out_dir / f"{symbol}.{SCHEMAS[schema][0]}.parquet"
        if dst.exists() and not args.force:
            print(f"skip {dst} (exists)")
            continue
        rows, actions = convert(src, dst, schema)
        detail = "  ".join(f"{a}={n:,}" for a, n in sorted(actions.items()))
        print(f"{src.name} -> {dst}  {rows:,} rows   {detail}")


if __name__ == "__main__":
    main()
