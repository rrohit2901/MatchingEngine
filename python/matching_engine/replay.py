"""Load converted Databento data and run it through the C++ replay.

The Parquet files come from scripts/convert_mbo.py. Columns are handed to C++ as
numpy arrays that it reads in place, so their dtypes must match exactly. That's
what the dtype tables below pin down.

Needs pyarrow (``pip install '.[data]'``); it is imported on first use so the rest
of the package does not depend on it.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

from ._core import PRICE_UNDEF, validate_replay

MBO_COLUMNS = {
    "ts_recv": np.uint64,
    "action": np.uint8,
    "side": np.uint8,
    "price_ticks": np.int32,
    "size": np.uint32,
    "order_id": np.uint64,
    "flags": np.uint8,
    "sequence": np.uint32,
}

MBP1_COLUMNS = {
    "ts_recv": np.uint64,
    "sequence": np.uint32,
    "flags": np.uint8,
    "bid_px": np.int32,
    "ask_px": np.int32,
    "bid_sz": np.uint32,
    "ask_sz": np.uint32,
}

DEFAULT_DATA_DIR = Path("data/databento")

__all__ = ["PRICE_UNDEF", "load_mbo", "load_mbp1", "data_paths", "validate", "MBO_COLUMNS", "MBP1_COLUMNS"]


def _load(path: Path, columns: dict[str, type]) -> dict[str, np.ndarray]:
    import pyarrow.parquet as pq

    table = pq.read_table(path, columns=list(columns))
    out = {}
    for name, dtype in columns.items():
        # to_numpy on a multi-chunk column concatenates once into one contiguous array.
        array = np.ascontiguousarray(table.column(name).to_numpy())
        if array.dtype != dtype:
            raise TypeError(f"{path}: column {name} is {array.dtype}, expected {np.dtype(dtype)}")
        out[name] = array
    return out


def load_mbo(path: str | Path) -> dict[str, np.ndarray]:
    """MBO columns from a <SYMBOL>.mbo.parquet file, ready for the replay."""
    return _load(Path(path), MBO_COLUMNS)


def load_mbp1(path: str | Path) -> dict[str, np.ndarray]:
    """mbp-1 columns from a <SYMBOL>.mbp1.parquet file."""
    return _load(Path(path), MBP1_COLUMNS)


def data_paths(date: str, symbol: str, data_dir: str | Path = DEFAULT_DATA_DIR) -> tuple[Path, Path]:
    """(mbo, mbp1) Parquet paths for one symbol-day, as convert_mbo.py names them."""
    day = Path(data_dir) / date
    return day / f"{symbol}.mbo.parquet", day / f"{symbol}.mbp1.parquet"


def validate(date: str, symbol: str, data_dir: str | Path = DEFAULT_DATA_DIR, max_examples: int = 20) -> dict:
    """Replay one symbol-day and compare the rebuilt top of book with Nasdaq's mbp-1."""
    mbo_path, mbp1_path = data_paths(date, symbol, data_dir)
    return validate_replay(load_mbo(mbo_path), load_mbp1(mbp1_path), max_examples)
