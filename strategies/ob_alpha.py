"""OB alpha: rest constant-value orders on the top levels of the book.

On every requote the strategy looks at the best `levels` price levels of each
side it quotes and keeps exactly one order resting at each of those prices. An
order's size is a constant dollar value, scaled by a per-level weight so the
levels nearest the touch carry the most (weights 5, 4, 3, 2, 1 by default):

    shares at level i = floor(order_value * weights[i] / price)

Orders at prices that have dropped out of the top levels are cancelled; a
partly filled order is left alone until its price drops out. A side stops
quoting while the position, counting every open order on that side as if it
filled, would pass `max_position`.

Run it:
    me-backtest run --config strategies/ob_alpha.toml
"""

from matching_engine.backtest import Context, Strategy


class OBAlpha(Strategy):
    def __init__(self, side: str = "both", levels: int = 5, order_value: float = 5_000.0,
                 weights: list[float] | None = None, max_position: int = 1_000, requote_every: int = 10):
        super().__init__(side=side, levels=levels, order_value=order_value, weights=weights,
                         max_position=max_position, requote_every=requote_every)
        if side not in ("buy", "sell", "both"):
            raise ValueError("side must be buy, sell or both")
        self.sides = ["BUY", "SELL"] if side == "both" else [side.upper()]
        self.levels = int(levels)
        self.order_value = float(order_value)
        self.weights = list(weights) if weights else [float(self.levels - i) for i in range(self.levels)]
        if len(self.weights) < self.levels:
            raise ValueError(f"need {self.levels} weights, got {len(self.weights)}")
        self.max_position = int(max_position)
        self.requote_every = max(1, int(requote_every))
        self.calls = 0

    def on_timer(self, ctx: Context) -> None:
        self.calls += 1
        if self.calls % self.requote_every:
            return

        book = ctx.book(self.levels)
        live = ctx.open_orders
        for side in self.sides:
            levels = book.bids if side == "BUY" else book.asks
            mine = {o.price: o for o in live if o.side == side}
            exposure = ctx.position if side == "BUY" else -ctx.position
            exposure += sum(o.remaining for o in mine.values())

            # Target size per price on this side.
            target: dict[float, int] = {}
            for level, weight in zip(levels, self.weights):
                shares = int(self.order_value * weight // level.price)
                if shares > 0:
                    target[level.price] = shares

            for price, order in mine.items():
                if price not in target:
                    ctx.cancel(order.order_id)
            for price, shares in target.items():
                if price in mine or exposure + shares > self.max_position:
                    continue
                (ctx.buy if side == "BUY" else ctx.sell)(price, shares)
                exposure += shares

    def on_end(self, ctx: Context) -> None:
        # Orders were cancelled at the close of the window; the position stays and is marked at mid.
        pass
