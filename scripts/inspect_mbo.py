#!/usr/bin/env python3
"""Check, on the real data, the MBO behaviour the replayer will rely on.

Databento's own source code states that Trade (T) and Fill (F) records do not
change the book. The rest of what the replayer assumes comes from third-party
code, so this script measures it on a converted day before anything is built
on top of it:

  1. Cancel size is the amount removed, not the amount left.
     The script keeps every order's outstanding size under that reading and
     counts every record that contradicts it (a cancel or fill larger than the
     order, an event for an unknown order id).
  2. An execution is T, then F (the resting orders hit), then the C or M that
     takes the size off the book, all within one F_LAST event.
  3. T with no F in the same event: executions against hidden liquidity.
  4. How ITCH replace messages appear: as M (price changes), or as C + A.
  5. Data-quality flags: F_BAD_TS_RECV, F_MAYBE_BAD_BOOK.
  6. Whether the rebuilt book ever crosses at an event boundary (a preview of
     the Phase 1 replay).

Reads data/databento/<date>/<SYMBOL>.mbo.parquet as written by
scripts/convert_mbo.py. Prints a report; nothing is written.

Usage:
    python3 scripts/inspect_mbo.py --date 2026-09-29 --symbol AAPL
    python3 scripts/inspect_mbo.py --file path/to/AAPL.mbo.parquet
"""

import argparse
import heapq
from collections import Counter, defaultdict
from datetime import date, datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

import pyarrow.parquet as pq

F_LAST = 1 << 7
F_TOB = 1 << 6
F_SNAPSHOT = 1 << 5
F_MBP = 1 << 4
F_BAD_TS_RECV = 1 << 3
F_MAYBE_BAD_BOOK = 1 << 2
FLAG_NAMES = {
    F_LAST: "F_LAST", F_TOB: "F_TOB", F_SNAPSHOT: "F_SNAPSHOT", F_MBP: "F_MBP",
    F_BAD_TS_RECV: "F_BAD_TS_RECV", F_MAYBE_BAD_BOOK: "F_MAYBE_BAD_BOOK",
}
NEW_YORK = ZoneInfo("America/New_York")
BATCH_ROWS = 1_000_000
MAX_EXAMPLES = 5


def ny_time(ts_ns: int) -> str:
    return datetime.fromtimestamp(ts_ns / 1e9, tz=timezone.utc).astimezone(NEW_YORK).strftime("%H:%M:%S.%f")


def run_length(actions: list[str]) -> str:
    """'TFFCC' -> 'T F+ C+': the shape of an event without its exact counts."""
    out = []
    for a in actions:
        if out and out[-1].rstrip("+") == a:
            out[-1] = a + "+"
        else:
            out.append(a)
    return " ".join(out)


class Book:
    """Aggregate size per price on each side, with lazy-deletion heaps for the best price."""

    def __init__(self) -> None:
        self.levels = {"B": defaultdict(int), "A": defaultdict(int)}
        self.heaps = {"B": [], "A": []}

    def clear(self) -> None:
        self.__init__()

    def change(self, side: str, price: int, delta: int) -> None:
        if side not in self.levels:
            return
        level = self.levels[side]
        before = level[price]
        level[price] = before + delta
        if before <= 0 < level[price]:
            heapq.heappush(self.heaps[side], -price if side == "B" else price)
        if level[price] <= 0:
            del level[price]

    def best(self, side: str):
        heap, level = self.heaps[side], self.levels[side]
        while heap:
            price = -heap[0] if side == "B" else heap[0]
            if price in level:
                return price
            heapq.heappop(heap)
        return None


class Inspector:
    def __init__(self) -> None:
        self.orders: dict[int, list] = {}      # order_id -> [side, price, size]
        self.book = Book()
        self.counts = Counter()
        self.actions = Counter()
        self.sides_by_action = Counter()
        self.flags = Counter()
        self.patterns = Counter()
        self.examples: dict[str, list] = defaultdict(list)
        self.event: list[tuple] = []          # (action, side, price, size, order_id, reduced_by)
        self.unfollowed_fills: dict[int, int] = {}
        self.rows = 0
        self.first_ts = self.last_ts = None
        self.clears: list[int] = []

    def example(self, key: str, text: str) -> None:
        if len(self.examples[key]) < MAX_EXAMPLES:
            self.examples[key].append(text)

    # -- per record ---------------------------------------------------------

    def record(self, ts, action, side, price, size, oid, flags) -> None:
        self.rows += 1
        if self.first_ts is None:
            self.first_ts = ts
        elif ts < self.last_ts:
            self.counts["ts_recv_decreasing"] += 1
        self.last_ts = ts

        self.actions[action] += 1
        self.sides_by_action[(action, side)] += 1
        for bit in FLAG_NAMES:
            if flags & bit:
                self.flags[bit] += 1

        reduced_by = 0
        order = self.orders.get(oid)

        if action == "A":
            if order is not None:
                self.counts["add_existing_id"] += 1
            self.orders[oid] = [side, price, size]
            self.book.change(side, price, size)

        elif action == "C":
            if order is None:
                self.counts["cancel_unknown_id"] += 1
                self.example("cancel_unknown_id", f"{ny_time(ts)} oid={oid} size={size}")
            else:
                if price != order[1]:
                    self.counts["cancel_price_mismatch"] += 1
                if size > order[2]:
                    self.counts["cancel_larger_than_order"] += 1
                    self.example("cancel_larger_than_order", f"{ny_time(ts)} oid={oid} cancel={size} outstanding={order[2]}")
                    size = order[2]
                reduced_by = size
                self.counts["cancel_full" if size == order[2] else "cancel_partial"] += 1
                self.reduce(oid, order, size)

        elif action == "M":
            if order is None:
                self.counts["modify_unknown_id"] += 1
                self.example("modify_unknown_id", f"{ny_time(ts)} oid={oid} price={price} size={size}")
                self.orders[oid] = [side, price, size]
                self.book.change(side, price, size)
            else:
                old_side, old_price, old_size = order
                if price != old_price:
                    self.counts["modify_price_change"] += 1
                if size > old_size:
                    self.counts["modify_size_up"] += 1
                elif size < old_size:
                    self.counts["modify_size_down"] += 1
                    if price == old_price:
                        reduced_by = old_size - size
                elif price == old_price:
                    self.counts["modify_no_change"] += 1
                self.book.change(old_side, old_price, -old_size)
                self.book.change(side, price, size)
                order[0], order[1], order[2] = side, price, size
                if size == 0:
                    self.counts["modify_to_zero"] += 1
                    del self.orders[oid]

        elif action == "F":
            if order is None:
                self.counts["fill_unknown_id"] += 1
            else:
                if size > order[2]:
                    self.counts["fill_larger_than_order"] += 1
                if price != order[1]:
                    self.counts["fill_price_mismatch"] += 1

        elif action == "R":
            self.clears.append(ts)
            if self.orders:
                self.counts["clear_with_orders_live"] += 1
            self.orders.clear()
            self.book.clear()

        # A C or M that reduces an order whose earlier fill was not reduced in its
        # own event: the removal arrived in a later event.
        if reduced_by and oid in self.unfollowed_fills:
            self.counts["fill_reduced_in_later_event"] += 1
            del self.unfollowed_fills[oid]

        self.event.append((action, side, price, size, oid, reduced_by))
        if flags & F_LAST:
            self.end_event(ts)

    def reduce(self, oid, order, size) -> None:
        order[2] -= size
        self.book.change(order[0], order[1], -size)
        if order[2] == 0:
            del self.orders[oid]

    # -- per F_LAST event ---------------------------------------------------

    def end_event(self, ts) -> None:
        event = self.event
        self.event = []
        self.counts["events"] += 1
        self.patterns[run_length([e[0] for e in event])] += 1

        trades = [e for e in event if e[0] == "T"]
        fills = [e for e in event if e[0] == "F"]
        if trades:
            self.counts["events_with_trade"] += 1
            if not fills:
                self.counts["trade_without_fill"] += 1
                if len(self.examples["trade_without_fill"]) < MAX_EXAMPLES:
                    t = trades[0]
                    self.example("trade_without_fill", f"{ny_time(ts)} side={t[1]} price={t[2]} size={t[3]}")
            else:
                t_size = sum(t[3] for t in trades)
                f_size = sum(f[3] for f in fills)
                self.counts["trade_size_eq_fill_size" if t_size == f_size
                            else "trade_size_gt_fill_size" if t_size > f_size
                            else "trade_size_lt_fill_size"] += 1
                if any(f[2] != t[2] for t in trades[:1] for f in fills):
                    self.counts["fill_price_differs_from_trade"] += 1
        if fills and not trades:
            self.counts["fill_without_trade"] += 1

        # For every fill: is the size taken off the same order by the next C/M on
        # it in this event? Only the next one: one order filled twice in an event
        # (T F C T F C) has a separate reduction for each fill.
        for i, f in enumerate(event):
            if f[0] != "F":
                continue
            removed = next((e[5] for e in event[i + 1:] if e[4] == f[4] and e[0] in "CM"), 0)
            if f[2] != next((e[2] for e in event[i + 1:] if e[4] == f[4] and e[0] in "CM"), f[2]):
                self.counts["fill_price_differs_from_order"] += 1
            if removed == f[3]:
                self.counts["fill_then_reduce_same_event_exact"] += 1
            elif removed:
                self.counts["fill_then_reduce_same_event_other_size"] += 1
            else:
                self.counts["fill_not_reduced_in_event"] += 1
                self.unfollowed_fills[f[4]] = f[3]

        # Order of the first T, F and C/M within execution events.
        if trades and fills:
            first = {}
            for idx, e in enumerate(event):
                first.setdefault(e[0], idx)
            reduce_idx = min([first[a] for a in "CM" if a in first], default=None)
            if first["T"] < first["F"] and (reduce_idx is None or first["F"] < reduce_idx):
                self.counts["execution_order_T_F_CM"] += 1
            else:
                self.counts["execution_order_other"] += 1
                self.example("execution_order_other", f"{ny_time(ts)} {''.join(e[0] for e in event)}")

        bid, ask = self.book.best("B"), self.book.best("A")
        if bid is not None and ask is not None and bid >= ask:
            self.counts["crossed_book_at_event_end"] += 1
            self.example("crossed_book_at_event_end", f"{ny_time(ts)} bid={bid} ask={ask}")

    # -- report -------------------------------------------------------------

    def report(self) -> None:
        c = self.counts
        print(f"rows {self.rows:,}   events (F_LAST) {c['events']:,}")
        print(f"first {ny_time(self.first_ts)} ET   last {ny_time(self.last_ts)} ET")
        if self.event:
            print(f"!! {len(self.event)} trailing records after the last F_LAST")
        print(f"clears (R) at: {', '.join(ny_time(t) for t in self.clears) or 'none'}")

        print("\nactions:  " + "  ".join(f"{a}={n:,}" for a, n in sorted(self.actions.items())))
        print("sides:    " + "  ".join(f"{a}/{s}={n:,}" for (a, s), n in sorted(self.sides_by_action.items())))
        print("flags:    " + "  ".join(f"{FLAG_NAMES[b]}={self.flags[b]:,}" for b in FLAG_NAMES))

        print("\ntop event shapes (run-length; '+' = repeated):")
        for shape, n in self.patterns.most_common(15):
            print(f"  {n:>12,}  {shape}")

        def line(key):
            print(f"  {key:<42}{c[key]:>12,}")

        print("\n1. cancel size = amount removed (contradictions should be 0):")
        for key in ["cancel_full", "cancel_partial", "cancel_larger_than_order", "cancel_unknown_id",
                    "cancel_price_mismatch", "fill_larger_than_order", "fill_unknown_id", "add_existing_id"]:
            line(key)
        print("\n2. execution = T, then F, then C/M in the same event:")
        for key in ["events_with_trade", "execution_order_T_F_CM", "execution_order_other",
                    "fill_then_reduce_same_event_exact", "fill_then_reduce_same_event_other_size",
                    "fill_not_reduced_in_event", "fill_reduced_in_later_event", "fill_without_trade",
                    "trade_size_eq_fill_size", "trade_size_gt_fill_size", "trade_size_lt_fill_size",
                    "fill_price_differs_from_trade", "fill_price_differs_from_order"]:
            line(key)
        print("\n3. hidden executions:")
        line("trade_without_fill")
        print("\n4. modifies / replaces:")
        for key in ["modify_price_change", "modify_size_up", "modify_size_down", "modify_no_change",
                    "modify_to_zero", "modify_unknown_id"]:
            line(key)
        print("\n5. data quality:")
        for key in ["ts_recv_decreasing", "clear_with_orders_live"]:
            line(key)
        print("\n6. book:")
        line("crossed_book_at_event_end")
        print(f"  live orders at end of file{len(self.orders):>27,}")

        for key, lines in self.examples.items():
            print(f"\nexamples: {key}")
            for text in lines:
                print(f"  {text}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--date", type=date.fromisoformat)
    parser.add_argument("--symbol")
    parser.add_argument("--data", type=Path, default=Path("data/databento"))
    parser.add_argument("--file", type=Path, help="a .mbo.parquet file, instead of --date/--symbol")
    args = parser.parse_args()

    if args.file is None:
        if not (args.date and args.symbol):
            parser.error("give --file, or both --date and --symbol")
        args.file = args.data / args.date.isoformat() / f"{args.symbol}.mbo.parquet"

    source = pq.ParquetFile(args.file)
    total = source.metadata.num_rows
    inspector = Inspector()
    columns = ["ts_recv", "action", "side", "price_ticks", "size", "order_id", "flags"]
    for batch in source.iter_batches(batch_size=BATCH_ROWS, columns=columns):
        cols = [batch.column(name).to_numpy().tolist() for name in columns]
        cols[1] = [chr(a) for a in cols[1]]
        cols[2] = [chr(s) for s in cols[2]]
        for row in zip(*cols):
            inspector.record(*row)
        if total > BATCH_ROWS:
            print(f"  ... {inspector.rows:,} / {total:,}", flush=True)

    print(f"\n== {args.file} ==")
    inspector.report()


if __name__ == "__main__":
    main()
