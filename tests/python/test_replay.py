"""Tests for the market replay binding, on hand-built columns (no data files needed)."""

import numpy as np
import pytest

from matching_engine import _core
from matching_engine.replay import MBO_COLUMNS, MBP1_COLUMNS

F_LAST = 128
UNDEF = _core.PRICE_UNDEF


def mbo(records):
    """records: (action, side, price, size, order_id, last) tuples, one per row."""
    cols = {name: [] for name in MBO_COLUMNS}
    for i, (action, side, price, size, oid, last) in enumerate(records):
        cols["ts_recv"].append(1_000 + i)
        cols["sequence"].append(i)
        cols["action"].append(ord(action))
        cols["side"].append(ord(side))
        cols["price_ticks"].append(price)
        cols["size"].append(size)
        cols["order_id"].append(oid)
        cols["flags"].append(F_LAST if last else 0)
    return {name: np.array(values, dtype=MBO_COLUMNS[name]) for name, values in cols.items()}


def mbp1(book, rows):
    """rows: (mbo record index, bid_px, bid_sz, ask_px, ask_sz)."""
    cols = {name: [] for name in MBP1_COLUMNS}
    for record, bpx, bsz, apx, asz in rows:
        cols["ts_recv"].append(int(book["ts_recv"][record]))
        cols["sequence"].append(int(book["sequence"][record]))
        cols["flags"].append(F_LAST)
        cols["bid_px"].append(bpx)
        cols["bid_sz"].append(bsz)
        cols["ask_px"].append(apx)
        cols["ask_sz"].append(asz)
    return {name: np.array(values, dtype=MBP1_COLUMNS[name]) for name, values in cols.items()}


def test_replay_matches_venue_top_of_book():
    book = mbo([
        ("A", "B", 1000, 10, 1, True),
        ("A", "A", 1002, 4, 2, True),
        ("T", "B", 1002, 3, 0, False), ("F", "A", 1002, 3, 2, False), ("C", "A", 1002, 3, 2, True),
    ])
    venue = mbp1(book, [(0, 1000, 10, UNDEF, 0), (1, 1000, 10, 1002, 4), (4, 1000, 10, 1002, 1)])

    result = _core.validate_replay(book, venue)

    assert result["compared"] == 3
    assert result["mismatched"] == 0
    assert result["crossed"] == 0
    assert result["replay"]["cancels_partial"] == 1
    assert result["replay"]["fills"] == 1


def test_mismatch_is_reported_with_both_sides():
    book = mbo([("A", "B", 1000, 10, 1, True)])
    venue = mbp1(book, [(0, 1000, 7, UNDEF, 0)])

    result = _core.validate_replay(book, venue)

    assert result["mismatched"] == 1
    example = result["examples"][0]
    assert (example["book_bid_sz"], example["venue_bid_sz"]) == (10, 7)


def test_wrong_dtype_is_rejected_not_copied():
    book = mbo([("A", "B", 1000, 10, 1, True)])
    book["price_ticks"] = book["price_ticks"].astype(np.int64)
    with pytest.raises(TypeError, match="price_ticks"):
        _core.validate_replay(book, mbp1(mbo([("A", "B", 1, 1, 1, True)]), []))


def test_missing_column_is_rejected():
    book = mbo([("A", "B", 1000, 10, 1, True)])
    del book["flags"]
    with pytest.raises(KeyError):
        _core.validate_replay(book, mbp1(book, []))
