"""Quote the touch: the simplest market maker.

Rest one bid at the best bid and one offer at the best ask. When the touch moves
away from a quote, cancel it; a new one goes out at the next timer. Stop adding
to a side once the position reaches max_position shares.

    me-backtest run --config strategies/quote_touch.toml
"""

from matching_engine.backtest import Strategy


class QuoteTheTouch(Strategy):
    def __init__(self, size=100, max_position=500):
        super().__init__(size=size, max_position=max_position)
        self.size, self.max_position = size, max_position

    def on_timer(self, ctx):
        bid, ask = ctx.best_bid, ctx.best_ask
        if bid is None or ask is None:
            return
        quotes = {"BUY": [], "SELL": []}
        for o in ctx.open_orders:
            quotes[o.side].append(o)
        # Cancel quotes the touch has moved away from.
        for o in quotes["BUY"]:
            if o.price != bid.price:
                ctx.cancel(o.order_id)
        for o in quotes["SELL"]:
            if o.price != ask.price:
                ctx.cancel(o.order_id)
        if not quotes["BUY"] and ctx.position < self.max_position:
            ctx.buy(bid.price, self.size)
        if not quotes["SELL"] and ctx.position > -self.max_position:
            ctx.sell(ask.price, self.size)
