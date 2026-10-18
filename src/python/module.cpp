// Python bindings: the types the replay and Simulator share, then the replay
// itself (src/python/replay_bindings.cpp).
//
// The live MatchingEngine is deliberately not bound. The backtester drives the
// OrderBook through the Simulator, and nothing in Python needs the engine's
// event queue and logger thread.

#include "Order.h"
#include "RiskManager.h"

#include <pybind11/pybind11.h>

#include <sstream>
#include <string>

namespace py = pybind11;

// src/python/replay_bindings.cpp
void bindReplay(py::module_& m);

namespace {

std::string riskParamsRepr(const RiskParams& params) {
    std::ostringstream out;
    out << "RiskParams(max_allowed_quantity_quote=" << params.max_allowed_quantity_quote
        << ", min_allowed_quantity_quote=" << params.min_allowed_quantity_quote
        << ", max_price_book_top_deviation=" << params.max_price_book_top_deviation << ")";
    return out.str();
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "Market replay and strategy Simulator over a single-symbol C++ order book.";

    py::enum_<OrderSide>(m, "OrderSide")
        .value("BUY", OrderSide::BUY)
        .value("SELL", OrderSide::SELL);

    const RiskParams defaults{};
    py::class_<RiskParams>(m, "RiskParams", "Pre-trade risk limits applied to strategy orders.")
        // A lambda rather than py::init<int,int,int>() so this does not depend on
        // parenthesised aggregate initialisation.
        .def(py::init([](int max_qty, int min_qty, int max_dev) {
                 return RiskParams{max_qty, min_qty, max_dev};
             }),
             py::arg("max_allowed_quantity_quote")   = defaults.max_allowed_quantity_quote,
             py::arg("min_allowed_quantity_quote")   = defaults.min_allowed_quantity_quote,
             py::arg("max_price_book_top_deviation") = defaults.max_price_book_top_deviation)
        .def_readwrite("max_allowed_quantity_quote", &RiskParams::max_allowed_quantity_quote)
        .def_readwrite("min_allowed_quantity_quote", &RiskParams::min_allowed_quantity_quote)
        .def_readwrite("max_price_book_top_deviation", &RiskParams::max_price_book_top_deviation)
        .def("__repr__", &riskParamsRepr);

    // After OrderSide and RiskParams, which the replay's signatures use.
    bindReplay(m);
}
