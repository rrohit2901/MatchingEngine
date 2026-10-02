"""Type stubs for the compiled extension module."""

from types import TracebackType
from typing import Optional

class OrderSide:
    BUY: OrderSide
    SELL: OrderSide
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class OrderType:
    LIMIT: OrderType
    MARKET: OrderType
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class RiskParams:
    max_allowed_quantity_quote: int
    min_allowed_quantity_quote: int
    max_price_book_top_deviation: int
    def __init__(
        self,
        max_allowed_quantity_quote: int = ...,
        min_allowed_quantity_quote: int = ...,
        max_price_book_top_deviation: int = ...,
    ) -> None: ...

class PriceLevel:
    @property
    def price(self) -> int: ...
    @property
    def quantity(self) -> int: ...

class MatchingEngine:
    def __init__(self, log_file: str, risk_params: RiskParams = ...) -> None: ...
    def add_order(
        self,
        price: int,
        quantity: int,
        side: OrderSide,
        order_type: OrderType = ...,
    ) -> Optional[int]:
        """The new order id, or None if pre-trade risk rejected it."""

    def cancel_order(self, order_id: int) -> bool: ...
    def modify_order(
        self,
        order_id: int,
        quantity: int,
        price: int,
        side: OrderSide,
        order_type: OrderType = ...,
    ) -> Optional[int]:
        """The surviving order id, or None if rejected or not in the book."""

    def buy_levels(self, num_levels: int = 1) -> list[PriceLevel]: ...
    def sell_levels(self, num_levels: int = 1) -> list[PriceLevel]: ...
    def book(self, num_levels: int = 1) -> tuple[list[PriceLevel], list[PriceLevel]]: ...
    def close(self) -> None: ...
    @property
    def closed(self) -> bool: ...
    @property
    def log_file(self) -> str: ...
    @property
    def risk_params(self) -> RiskParams: ...
    def __enter__(self) -> MatchingEngine: ...
    def __exit__(
        self,
        exc_type: Optional[type[BaseException]],
        exc_value: Optional[BaseException],
        traceback: Optional[TracebackType],
    ) -> bool: ...
