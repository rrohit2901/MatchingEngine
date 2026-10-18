"""Backtest strategies against a replayed Nasdaq book, on a C++ order book.

The strategy API lives in :mod:`matching_engine.backtest`, the data loaders in
:mod:`matching_engine.replay`, and the ``me-backtest`` command in
:mod:`matching_engine.cli`. The compiled core is :mod:`matching_engine._core`.
"""

from ._core import OrderSide, RiskParams

__version__ = "0.1.0"

__all__ = [
    "OrderSide",
    "RiskParams",
    "__version__",
]
