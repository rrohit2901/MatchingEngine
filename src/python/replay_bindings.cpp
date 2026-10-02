// Python access to the market replay (include/replay/).
//
// Columns cross the boundary as numpy arrays and are viewed in place: each must
// already have the exact dtype the C++ side reads and be C-contiguous. Nothing
// is converted silently, because a converted copy of a day of MBO data is
// hundreds of megabytes; a wrong dtype is a bug in the loader and raises instead.

#include "ReplayValidator.h"
#include "Simulator.h"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <span>
#include <string>

namespace py = pybind11;

namespace {

template <typename T>
std::span<const T> column(const py::dict& columns, const char* name) {
    if (!columns.contains(name)) throw py::key_error(std::string("missing column '") + name + "'");
    const auto array = py::reinterpret_borrow<py::array>(columns[name]);
    if (!array.dtype().is(py::dtype::of<T>())) {
        throw py::type_error(std::string("column '") + name + "' has dtype " +
                             py::str(array.dtype()).cast<std::string>() + ", expected " +
                             py::str(py::dtype::of<T>()).cast<std::string>());
    }
    if (array.ndim() != 1 || !(array.flags() & py::array::c_style)) {
        throw py::value_error(std::string("column '") + name + "' must be 1-D and C-contiguous");
    }
    return {static_cast<const T*>(array.data()), static_cast<size_t>(array.size())};
}

MboEvents mboFrom(const py::dict& c) {
    return {column<uint64_t>(c, "ts_recv"), column<uint8_t>(c, "action"), column<uint8_t>(c, "side"),
            column<int32_t>(c, "price_ticks"), column<uint32_t>(c, "size"), column<uint64_t>(c, "order_id"),
            column<uint8_t>(c, "flags"), column<uint32_t>(c, "sequence")};
}

Mbp1Events mbp1From(const py::dict& c) {
    return {column<uint64_t>(c, "ts_recv"), column<uint32_t>(c, "sequence"), column<uint8_t>(c, "flags"),
            column<int32_t>(c, "bid_px"), column<int32_t>(c, "ask_px"),
            column<uint32_t>(c, "bid_sz"), column<uint32_t>(c, "ask_sz")};
}

py::dict statsDict(const ReplayStats& s) {
    py::dict d;
    d["records"] = s.records;           d["events"] = s.events;
    d["adds"] = s.adds;                 d["cancels_full"] = s.cancels_full;
    d["cancels_partial"] = s.cancels_partial;
    d["modifies"] = s.modifies;         d["clears"] = s.clears;
    d["trades"] = s.trades;             d["fills"] = s.fills;               d["nones"] = s.nones;
    d["unknown_order"] = s.unknown_order;
    d["modify_unknown"] = s.modify_unknown;
    d["duplicate_add"] = s.duplicate_add;
    d["cancel_oversized"] = s.cancel_oversized;
    d["bad_side"] = s.bad_side;         d["bad_price"] = s.bad_price;
    d["unknown_action"] = s.unknown_action;
    return d;
}

py::dict reportDict(const ValidationReport& report);

template <typename Validate>
py::dict validateWithGilReleased(const py::dict& mbo_columns, const py::dict& mbp1_columns, size_t max_examples,
                                 Validate validate_fn) {
    const MboEvents mbo = mboFrom(mbo_columns);
    const Mbp1Events mbp = mbp1From(mbp1_columns);
    if (!mbo.consistent() || !mbp.consistent()) throw py::value_error("columns have different lengths");

    ValidationReport report;
    {
        // The arrays stay alive in the caller's dicts; the replay touches no Python objects.
        py::gil_scoped_release release;
        report = validate_fn(mbo, mbp, max_examples);
    }
    return reportDict(report);
}

py::dict validate(const py::dict& mbo_columns, const py::dict& mbp1_columns, size_t max_examples) {
    return validateWithGilReleased(mbo_columns, mbp1_columns, max_examples, validateAgainstMbp1);
}

py::dict validateSimulator(const py::dict& mbo_columns, const py::dict& mbp1_columns, size_t max_examples) {
    return validateWithGilReleased(mbo_columns, mbp1_columns, max_examples, validateSimulatorAgainstMbp1);
}

py::dict reportDict(const ValidationReport& report) {

    py::list examples;
    for (const auto& m : report.examples) {
        py::dict e;
        e["ts_recv"] = m.ts_recv;           e["sequence"] = m.sequence;
        e["book_bid_px"] = m.book_bid_px;   e["book_bid_sz"] = m.book_bid_sz;
        e["book_ask_px"] = m.book_ask_px;   e["book_ask_sz"] = m.book_ask_sz;
        e["venue_bid_px"] = m.venue_bid_px; e["venue_bid_sz"] = m.venue_bid_sz;
        e["venue_ask_px"] = m.venue_ask_px; e["venue_ask_sz"] = m.venue_ask_sz;
        examples.append(e);
    }
    py::dict out;
    out["replay"] = statsDict(report.replay);
    out["compared"] = report.compared;
    out["mismatched"] = report.mismatched;
    out["crossed"] = report.crossed;
    out["venue_unmatched"] = report.venue_unmatched;
    out["examples"] = examples;
    return out;
}

py::dict simStatsDict(const SimStats& s) {
    py::dict d;
    d["venue"] = statsDict(s.venue);
    d["timer_calls"] = s.timer_calls;
    d["orders_submitted"] = s.orders_submitted;
    d["orders_rejected"] = s.orders_rejected;
    d["orders_cancelled"] = s.orders_cancelled;
    d["fills"] = s.fills;
    d["maker_qty"] = s.maker_qty;
    d["taker_qty"] = s.taker_qty;
    d["ahead_fill_qty"] = s.ahead_fill_qty;
    d["sweep_qty"] = s.sweep_qty;
    d["unfilled_venue_qty"] = s.unfilled_venue_qty;
    d["clamped_cancels"] = s.clamped_cancels;
    d["orphaned_orders"] = s.orphaned_orders;
    d["orphaned_qty"] = s.orphaned_qty;
    d["crossing_add_qty"] = s.crossing_add_qty;
    return d;
}

py::tuple orderTuple(const StrategyOrder& o) {
    return py::make_tuple(o.client_id, o.side, o.price, o.quantity, o.filled,
                          to_string(o.status), o.reject_reason, o.ts_sent, o.ts_arrival);
}

py::tuple fillTuple(const StrategyFill& f) {
    return py::make_tuple(f.ts, f.client_id, f.side, f.price, f.quantity, f.maker, to_string(f.source));
}

py::list levels(const std::vector<LevelView>& view) {
    py::list out;
    for (const LevelView& level : view) out.append(py::make_tuple(level.price, level.quantity));
    return out;
}

// Keeps the MBO arrays alive and in place while a Simulator runs over them.
void runSimulator(Simulator& sim, const py::dict& mbo_columns, const py::function& on_timer) {
    const MboEvents events = mboFrom(mbo_columns);
    if (!events.consistent()) throw py::value_error("columns have different lengths");
    // The GIL stays held: on_timer is Python. An exception raised in it unwinds
    // through run() and surfaces here unchanged.
    sim.run(events, [&on_timer](Simulator&) { on_timer(); });
}

}  // namespace

void bindReplay(py::module_& m) {
    m.attr("PRICE_UNDEF") = mbo::PRICE_UNDEF;
    m.def("validate_replay", &validate, py::arg("mbo"), py::arg("mbp1"), py::arg("max_examples") = 20,
          "Replay MBO columns into an order book and compare its top of book with mbp-1\n"
          "after every event. Both arguments map column name -> numpy array, as loaded by\n"
          "matching_engine.replay. Returns a dict of counters and the first mismatches.");
    m.def("validate_simulator", &validateSimulator, py::arg("mbo"), py::arg("mbp1"), py::arg("max_examples") = 20,
          "validate_replay, but through the strategy Simulator with no strategy: its\n"
          "reconciliation logic must leave the book exactly as the plain replay does.");

    py::class_<SimConfig>(m, "SimConfig", "Simulator settings. Times in nanoseconds, prices in 1e-4 $ ticks.")
        .def(py::init<>())
        .def_readwrite("order_latency_ns", &SimConfig::order_latency_ns)
        .def_readwrite("md_latency_ns", &SimConfig::md_latency_ns)
        .def_readwrite("timer_interval_ns", &SimConfig::timer_interval_ns)
        .def_readwrite("trade_start_ns", &SimConfig::trade_start_ns)
        .def_readwrite("trade_end_ns", &SimConfig::trade_end_ns)
        .def_readwrite("price_increment", &SimConfig::price_increment)
        .def_readwrite("max_position", &SimConfig::max_position)
        .def_readwrite("risk", &SimConfig::risk)
        .def_readwrite("maker_fee", &SimConfig::maker_fee)
        .def_readwrite("taker_fee", &SimConfig::taker_fee)
        .def_readwrite("passive_impact", &SimConfig::passive_impact)
        .def_readwrite("pnl_sample_interval_ns", &SimConfig::pnl_sample_interval_ns);

    py::class_<Simulator>(m, "Simulator", R"doc(
One strategy trading into a replayed Nasdaq book (include/replay/Simulator.h).

Low-level: matching_engine.backtest wraps it with dollar prices and a Strategy
class. Prices here are int ticks of 1e-4 $, times are UTC nanoseconds.
)doc")
        .def(py::init<SimConfig>(), py::arg("config"))
        .def("run", &runSimulator, py::arg("mbo"), py::arg("on_timer"),
             "Replay the MBO columns, calling on_timer() every timer interval in the trading window.")
        .def("submit", &Simulator::submit, py::arg("side"), py::arg("price"), py::arg("quantity"),
             "Limit order; returns its client id. It reaches the exchange after the latency.")
        .def("cancel", &Simulator::cancel, py::arg("client_id"))
        .def("cancel_all", &Simulator::cancelAll)
        .def_property_readonly("now", &Simulator::now)
        .def_property_readonly("position", &Simulator::position)
        .def_property_readonly("cash_ticks", &Simulator::cashTicks)
        .def_property_readonly("fees", &Simulator::fees)
        .def("mark_to_market", &Simulator::markToMarket)
        .def("book", [](const Simulator& s, unsigned n) {
                 return py::make_tuple(levels(s.getBook().getBuySideView(static_cast<int>(n))),
                                       levels(s.getBook().getSellSideView(static_cast<int>(n))));
             }, py::arg("levels") = 5, "(bids, asks): lists of (price, quantity), best first.")
        .def("best", [](const Simulator& s, OrderSide side) -> py::object {
                 const auto level = s.getBook().getTopLevel(side);
                 if (!level) return py::none();
                 return py::make_tuple(level->price, level->quantity);
             }, py::arg("side"), "(price, quantity) of the best level on one side, or None.")
        .def_property_readonly("fill_count", [](const Simulator& s) { return s.fills().size(); })
        .def("fills", [](const Simulator& s, size_t start) {
                 py::list out;
                 const auto& all = s.fills();
                 for (size_t i = start; i < all.size(); ++i) out.append(fillTuple(all[i]));
                 return out;
             }, py::arg("start") = 0,
             "Fills from index `start` on, as (ts, client_id, side, price, quantity, maker, source).")
        .def("order", [](const Simulator& s, uint64_t client_id) -> py::object {
                 const auto it = s.orders().find(client_id);
                 if (it == s.orders().end()) return py::none();
                 return orderTuple(it->second);
             }, py::arg("client_id"),
             "(client_id, side, price, quantity, filled, status, reject_reason, ts_sent, ts_arrival).")
        .def("orders", [](const Simulator& s, bool live_only) {
                 py::list out;
                 if (live_only) {
                     for (const uint64_t id : s.liveOrderIds()) out.append(orderTuple(s.orders().at(id)));
                 } else {
                     std::vector<const StrategyOrder*> all;
                     all.reserve(s.orders().size());
                     for (const auto& [id, o] : s.orders()) all.push_back(&o);
                     std::sort(all.begin(), all.end(), [](auto* a, auto* b) { return a->client_id < b->client_id; });
                     for (const StrategyOrder* o : all) out.append(orderTuple(*o));
                 }
                 return out;
             }, py::arg("live_only") = true,
             "Orders as order() tuples, oldest first; by default only PENDING and OPEN ones.")
        .def("equity_curve", [](const Simulator& s) {
                 py::list out;
                 for (const auto& e : s.equityCurve()) out.append(py::make_tuple(e.ts, e.position, e.cash_ticks, e.mid_x2, e.fees));
                 return out;
             }, "Samples of (ts, position, cash_ticks, mid_x2, fees).")
        .def("stats", [](const Simulator& s) { return simStatsDict(s.getStats()); });
}
