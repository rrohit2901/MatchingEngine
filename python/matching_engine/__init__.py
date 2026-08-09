"""A single-symbol matching engine, implemented in C++.

Submit orders to a :class:`MatchingEngine` and it maintains a price-time priority
book, matching crossing orders on arrival and applying pre-trade risk checks:

    >>> from matching_engine import MatchingEngine, OrderSide
    >>> with MatchingEngine("logs/session.log") as engine:
    ...     buy = engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)
    ...     engine.add_order(price=1005, quantity=10, side=OrderSide.SELL)  # crosses
    ...     bids, asks = engine.book(num_levels=5)

Three things are worth knowing before you rely on it.

**The engine owns a thread, so it has to be shut down.** Every operation publishes
an event -- trades, adds, cancels, rejects -- to a background logger that writes to
`log_file`. Use the engine as a context manager, or call :meth:`MatchingEngine.close`
yourself. The log file is only complete once ``close()`` has returned.

**The log is the only record of what happened.** Nothing is returned to Python
beyond order ids and book views; in particular a rejected order comes back as
``None`` and the reason for it appears solely in the log.

**One engine is not thread-safe.** The book has no internal locking, and the
methods deliberately hold the GIL, which is what keeps concurrent calls from
corrupting it. An engine also cannot be used in a process forked from the one that
created it -- the logger thread does not survive ``fork()`` -- and raises if tried.
"""

from ._core import MatchingEngine, OrderSide, OrderType, PriceLevel, RiskParams

__version__ = "0.1.0"

__all__ = [
    "MatchingEngine",
    "OrderSide",
    "OrderType",
    "PriceLevel",
    "RiskParams",
    "__version__",
]
