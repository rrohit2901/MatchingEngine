// Python access to the market replay (include/replay/).
//
// Columns cross the boundary as numpy arrays and are viewed in place: each must
// already have the exact dtype the C++ side reads and be C-contiguous. Nothing
// is converted silently, because a converted copy of a day of MBO data is
// hundreds of megabytes; a wrong dtype is a bug in the loader and raises instead.

#include "ReplayValidator.h"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

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

py::dict validate(const py::dict& mbo_columns, const py::dict& mbp1_columns, size_t max_examples) {
    const MboEvents mbo = mboFrom(mbo_columns);
    const Mbp1Events mbp = mbp1From(mbp1_columns);
    if (!mbo.consistent() || !mbp.consistent()) throw py::value_error("columns have different lengths");

    ValidationReport report;
    {
        // The arrays stay alive in the caller's dicts; the replay touches no Python objects.
        py::gil_scoped_release release;
        report = validateAgainstMbp1(mbo, mbp, max_examples);
    }

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

}  // namespace

void bindReplay(py::module_& m) {
    m.attr("PRICE_UNDEF") = mbo::PRICE_UNDEF;
    m.def("validate_replay", &validate, py::arg("mbo"), py::arg("mbp1"), py::arg("max_examples") = 20,
          "Replay MBO columns into an order book and compare its top of book with mbp-1\n"
          "after every event. Both arguments map column name -> numpy array, as loaded by\n"
          "matching_engine.replay. Returns a dict of counters and the first mismatches.");
}
