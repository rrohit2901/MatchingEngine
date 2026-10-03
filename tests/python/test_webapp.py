"""Smoke test of the Streamlit page: render, run a backtest on a tiny day, see results."""

import re
import time
from datetime import time as dt_time
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from test_backtest import BOOK, DATE, tape

streamlit_testing = pytest.importorskip("streamlit.testing.v1")
APP = Path(__file__).resolve().parents[2] / "webapp" / "app.py"


LIFT = (
    "from matching_engine.backtest import Strategy\n"
    "class Lift(Strategy):\n"
    "    def on_timer(self, ctx):\n"
    "        if not getattr(self, 'sent', False):\n"
    "            self.sent = True\n"
    "            ctx.buy(ctx.best_ask.price, 5)\n")


def use_code(app, code):
    """What the editor reports back: the page runs st.session_state.code."""
    app.session_state["code"] = code


@pytest.fixture
def app(tmp_path, monkeypatch):
    day = tmp_path / "data" / DATE
    day.mkdir(parents=True)
    pq.write_table(pa.table(tape(BOOK + [(59_000, "N", "N", 0, 0, 0, True)])), day / "TEST.mbo.parquet")
    monkeypatch.setenv("ME_DATA_DIR", str(tmp_path / "data"))
    at = streamlit_testing.AppTest.from_file(str(APP), default_timeout=60)
    at.run()
    assert not at.exception, at.exception
    return at


def test_page_renders_with_defaults(app):
    assert app.title[0].value == "Nasdaq replay backtester"
    assert app.sidebar.selectbox[0].value == DATE
    assert app.sidebar.selectbox[1].value == "TEST"
    assert "class QuoteTheTouch" in app.session_state["code"]       # the simple example
    assert "max_position = 500" in app.text_area(key="params").value


def test_load_example_replaces_code_and_parameters(app):
    app.selectbox(key="example").set_value("Order-book alpha")
    app.button[0].click().run()
    assert not app.exception, app.exception
    assert "class OBAlpha" in app.session_state["code"]
    assert "requote_every" in app.text_area(key="params").value


def test_syntax_error_is_reported_without_running(app):
    use_code(app, "from matching_engine.backtest import Strategy\nclass Broken(Strategy:\n    pass\n")
    app.button(key="run").click().run()
    assert not app.exception, app.exception
    assert app.error and "SyntaxError" in app.error[0].value
    assert "job" not in app.session_state                         # nothing was queued


def test_run_shows_results(app):
    use_code(app, LIFT)
    app.text_area(key="params").set_value("")
    app.sidebar.time_input[0].set_value(dt_time(9, 31))
    app.sidebar.time_input[1].set_value(dt_time(9, 32))
    app.sidebar.number_input[0].set_value(100.0)   # timer ms
    app.button(key="run").click().run()
    assert not app.exception, app.exception

    deadline = time.monotonic() + 60
    while not app.metric and time.monotonic() < deadline:
        time.sleep(0.5)
        app.run()
        assert not app.exception, app.exception
    labels = {m.label: m.value for m in app.metric}
    assert labels["Final position"] == "5 sh", labels
    assert labels["Realized"] == "$0.00" and labels["Unrealized"] == "-$0.05", labels
    assert not app.error


def test_bad_parameters_are_reported(app):
    app.text_area(key="params").set_value("this is = = not toml")
    app.button(key="run").click().run()
    assert app.error and "not valid TOML" in app.error[0].value


def test_run_label_heads_the_results(app):
    use_code(app, LIFT)
    app.text_area(key="params").set_value("")
    app.text_input(key="label").set_value("Lift test **#1**")
    app.sidebar.time_input[0].set_value(dt_time(9, 31))
    app.sidebar.time_input[1].set_value(dt_time(9, 32))
    app.sidebar.number_input[0].set_value(100.0)
    app.button(key="run").click().run()

    deadline = time.monotonic() + 60
    while not app.metric and time.monotonic() < deadline:
        time.sleep(0.5)
        app.run()
        assert not app.exception, app.exception
    # The label is the heading, shown literally (no Markdown from it); the class name follows.
    assert app.subheader[0].value == r"Lift test \*\*\#1\*\*"
    assert any("Lift on TEST" in m.value for m in app.markdown)
    # Every $ in Markdown is escaped: two bare ones would turn the text between them into LaTeX.
    for caption in app.caption:
        assert not re.search(r"(?<!\\)\$", caption.value), caption.value
