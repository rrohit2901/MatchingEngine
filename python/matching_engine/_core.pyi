"""Type stubs for the compiled extension module."""

class OrderSide:
    BUY: OrderSide
    SELL: OrderSide
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
