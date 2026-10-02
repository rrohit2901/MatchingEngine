"""Tests for the pybind11 bindings.

Two kinds of assertion here, because the engine reports almost nothing back to
Python. What a call returns -- an order id, None, a book view -- is checked
directly. Everything else the engine did is only visible in the log file, which is
parsed after the engine is closed.
"""

import pytest

from matching_engine import MatchingEngine, OrderSide, OrderType, RiskParams


@pytest.fixture
def log_path(tmp_path):
    return tmp_path / "engine.log"


def read_log(path):
    return [line for line in path.read_text().splitlines() if line]


# ---------------------------------------------------------------------------
# Orders
# ---------------------------------------------------------------------------


def test_add_order_returns_id_and_rests_in_book(log_path):
    with MatchingEngine(str(log_path)) as engine:
        order_id = engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)

        assert isinstance(order_id, int)
        levels = engine.buy_levels(num_levels=5)
        assert len(levels) == 1
        assert levels[0].price == 1005
        assert levels[0].quantity == 10


def test_crossing_order_trades_and_empties_the_book(log_path):
    with MatchingEngine(str(log_path)) as engine:
        engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)
        engine.add_order(price=1005, quantity=10, side=OrderSide.SELL)

        bids, asks = engine.book(num_levels=5)
        assert bids == []
        assert asks == []


def test_partial_fill_leaves_the_residual_resting(log_path):
    with MatchingEngine(str(log_path)) as engine:
        engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)
        engine.add_order(price=1005, quantity=4, side=OrderSide.SELL)

        bids, asks = engine.book(num_levels=5)
        assert asks == []
        assert len(bids) == 1
        assert bids[0].quantity == 6


def test_sell_side_is_reported_separately(log_path):
    with MatchingEngine(str(log_path)) as engine:
        engine.add_order(price=1010, quantity=7, side=OrderSide.SELL)

        assert engine.buy_levels() == []
        asks = engine.sell_levels(num_levels=5)
        assert len(asks) == 1
        assert asks[0].price == 1010
        assert asks[0].quantity == 7


def test_market_order_is_accepted(log_path):
    with MatchingEngine(str(log_path)) as engine:
        engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)
        order_id = engine.add_order(
            price=1005, quantity=10, side=OrderSide.SELL, order_type=OrderType.MARKET
        )

        assert isinstance(order_id, int)
    assert any("MARKET_ORDER_ADDED" in line for line in read_log(log_path))


# ---------------------------------------------------------------------------
# Cancel and modify
# ---------------------------------------------------------------------------


def test_cancel_succeeds_once_then_fails(log_path):
    with MatchingEngine(str(log_path)) as engine:
        order_id = engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)

        assert engine.cancel_order(order_id) is True
        assert engine.cancel_order(order_id) is False
        assert engine.buy_levels(num_levels=5) == []


def test_modify_returns_the_surviving_id(log_path):
    with MatchingEngine(str(log_path)) as engine:
        order_id = engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)

        # A requote can retire the original order and book a replacement, so the
        # id that comes back is not necessarily the one passed in.
        new_id = engine.modify_order(
            order_id=order_id, quantity=15, price=1005, side=OrderSide.BUY
        )

        assert isinstance(new_id, int)
        levels = engine.buy_levels(num_levels=5)
        assert len(levels) == 1
        assert levels[0].quantity == 15


def test_modify_of_unknown_order_returns_none(log_path):
    with MatchingEngine(str(log_path)) as engine:
        assert (
            engine.modify_order(
                order_id=987654, quantity=10, price=1005, side=OrderSide.BUY
            )
            is None
        )


# ---------------------------------------------------------------------------
# Risk
# ---------------------------------------------------------------------------


def test_zero_quantity_is_rejected_and_leaves_the_book_alone(log_path):
    with MatchingEngine(str(log_path)) as engine:
        assert engine.add_order(price=1005, quantity=0, side=OrderSide.BUY) is None
        assert engine.buy_levels(num_levels=5) == []


def test_price_far_from_top_of_book_is_rejected(log_path):
    with MatchingEngine(str(log_path)) as engine:
        engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)

        assert engine.add_order(price=9999, quantity=10, side=OrderSide.BUY) is None


def test_custom_risk_params_are_applied_and_readable(log_path):
    params = RiskParams(max_allowed_quantity_quote=50)
    with MatchingEngine(str(log_path), params) as engine:
        assert engine.risk_params.max_allowed_quantity_quote == 50
        assert engine.add_order(price=1005, quantity=51, side=OrderSide.BUY) is None
        assert engine.add_order(price=1005, quantity=50, side=OrderSide.BUY) is not None


def test_risk_params_defaults():
    params = RiskParams()
    assert params.max_allowed_quantity_quote == 100000
    assert params.min_allowed_quantity_quote == 1
    assert params.max_price_book_top_deviation == 1000


# ---------------------------------------------------------------------------
# Argument validation
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("num_levels", [0, -1])
def test_non_positive_num_levels_raises(log_path, num_levels):
    # Regression test: the C++ view takes an int but forwards to an unsigned
    # parameter, so a negative count used to wrap and return the whole book.
    with MatchingEngine(str(log_path)) as engine:
        with pytest.raises(ValueError):
            engine.buy_levels(num_levels=num_levels)
        with pytest.raises(ValueError):
            engine.sell_levels(num_levels=num_levels)
        with pytest.raises(ValueError):
            engine.book(num_levels=num_levels)


# ---------------------------------------------------------------------------
# Lifecycle
# ---------------------------------------------------------------------------


def test_context_manager_closes(log_path):
    with MatchingEngine(str(log_path)) as engine:
        assert engine.closed is False
    assert engine.closed is True


def test_close_is_idempotent(log_path):
    engine = MatchingEngine(str(log_path))
    engine.close()
    engine.close()
    assert engine.closed is True


def test_use_after_close_raises(log_path):
    engine = MatchingEngine(str(log_path))
    engine.close()

    with pytest.raises(RuntimeError):
        engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)
    with pytest.raises(RuntimeError):
        engine.cancel_order(1)
    with pytest.raises(RuntimeError):
        engine.buy_levels()


def test_log_file_property(log_path):
    with MatchingEngine(str(log_path)) as engine:
        assert engine.log_file == str(log_path)


def test_missing_log_directory_is_created(tmp_path):
    nested = tmp_path / "a" / "b" / "engine.log"
    with MatchingEngine(str(nested)) as engine:
        engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)

    assert nested.exists()


def test_unwritable_log_path_raises_at_construction(tmp_path):
    # A directory can never be opened for writing, so this fails at the probe in
    # the constructor rather than silently dropping every event on the logger
    # thread.
    with pytest.raises(RuntimeError):
        MatchingEngine(str(tmp_path))


# ---------------------------------------------------------------------------
# The event log
# ---------------------------------------------------------------------------


def test_session_markers_bracket_the_log(log_path):
    with MatchingEngine(str(log_path)) as engine:
        engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)

    lines = read_log(log_path)
    # SESSION_CLOSE landing last is the whole teardown contract in one assertion:
    # the engine has to be destroyed before the logger is told to stop, or the
    # sentinel overtakes this record and it never reaches the file.
    assert "SESSION_OPEN" in lines[0]
    assert "SESSION_CLOSE" in lines[-1]


def test_trade_is_logged_with_both_order_ids(log_path):
    with MatchingEngine(str(log_path)) as engine:
        buy_id = engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)
        sell_id = engine.add_order(price=1005, quantity=10, side=OrderSide.SELL)

    trades = [line for line in read_log(log_path) if "TRADE_EVENT" in line]
    assert len(trades) == 1
    assert f"BUY_ID: {buy_id}" in trades[0]
    assert f"SELL_ID: {sell_id}" in trades[0]
    assert "TRADE_QTY: 10" in trades[0]
    assert "TRADE_PRICE: 1005" in trades[0]


def test_reject_reason_is_recoverable_from_the_log(log_path):
    # add_order only says None. This is where the reason actually lives.
    with MatchingEngine(str(log_path)) as engine:
        engine.add_order(price=1005, quantity=0, side=OrderSide.BUY)

    rejects = [line for line in read_log(log_path) if "ORDER_REJECTED" in line]
    assert len(rejects) == 1
    assert "REASON: QUANTITY_BELOW_MIN" in rejects[0]


def test_cancel_is_logged(log_path):
    with MatchingEngine(str(log_path)) as engine:
        order_id = engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)
        engine.cancel_order(order_id)

    cancels = [line for line in read_log(log_path) if "ORDER_CANCELLED" in line]
    assert len(cancels) == 1
    assert f"ORDER_ID: {order_id}" in cancels[0]


def test_log_is_complete_only_after_close(log_path):
    engine = MatchingEngine(str(log_path))
    engine.add_order(price=1005, quantity=10, side=OrderSide.BUY)
    engine.close()

    lines = read_log(log_path)
    assert any("LIMIT_ORDER_ADDED" in line for line in lines)
    assert "SESSION_CLOSE" in lines[-1]
