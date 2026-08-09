// Python bindings for the matching engine.
//
// The engine cannot be handed to Python as-is. MatchingEngine's constructor takes
// a shared_ptr to an event queue, and nothing drains that queue unless a Logger is
// running on its own thread — so a caller given the raw class could trivially build
// an engine whose unbounded queue grows until it exhausts memory. Teardown is
// equally load-bearing: ~MatchingEngine publishes SESSION_CLOSE, and Logger::stop()
// publishes the sentinel that ends the drain loop, so the engine must die BEFORE
// stop() or the last record never reaches the file. src/main.cpp gets this right by
// hand; PyMatchingEngine below is that same wiring made non-optional.
//
// Only MatchingEngine<LockQueue> is bound: the class is templated on a template
// template parameter with a requires-clause, and pybind11 binds concrete
// instantiations only. That is also the only instantiation used anywhere in the
// repo.

#include "MatchingEngine.h"
#include "Logger.h"
#include "lock_queue.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>   // vector/pair/optional/string conversions

#include <pthread.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace {

using EventQueue  = LockQueue<std::unique_ptr<Event>>;
using Engine      = MatchingEngine<LockQueue>;
using EventLogger = Logger<LockQueue>;

// ---------------------------------------------------------------------------
// Book views
// ---------------------------------------------------------------------------

// BookLevel is a live handle onto the book, not a snapshot: it holds a
// shared_ptr<OrderManager> and re-reads it on every accessor, so a Python object
// wrapping one would report whatever the book looks like when it is read rather
// than when it was fetched. Flat values captured at call time avoid that whole
// class of surprise.
struct PriceLevel {
    int price;
    int quantity;
    std::vector<order_id_t> order_ids;
};

std::vector<PriceLevel> snapshot(const std::vector<std::shared_ptr<BookLevel>>& levels) {
    std::vector<PriceLevel> out;
    out.reserve(levels.size());
    for (const auto& level : levels) {
        if (!level) continue;
        out.push_back(PriceLevel{level->getPrice(), level->getTotalQuantity(), level->getOrders()});
    }
    return out;
}

// ---------------------------------------------------------------------------
// The facade
// ---------------------------------------------------------------------------

class PyMatchingEngine {
  public:
    PyMatchingEngine(std::string log_file, RiskParams risk_params)
        : log_file_(std::move(log_file)), risk_(risk_params) {
        // Logger opens the file on its own thread and, if the open fails, only
        // warns on stderr and then drains every event into the void. From Python
        // that would look like a silently empty log, so prove the path is usable
        // here, while the failure can still be an exception.
        prepareLogFile(log_file_);

        queue_  = std::make_shared<EventQueue>();
        logger_ = std::make_unique<EventLogger>(queue_, log_file_);
        logger_thread_ = std::make_unique<std::thread>(
            [logger = logger_.get()] { logger->readWriteLogs(); });

        try {
            engine_ = std::make_unique<Engine>(queue_, risk_);   // publishes SESSION_OPEN
        } catch (...) {
            // The logger thread is already blocked on the queue. Letting the
            // members unwind now would destroy a joinable std::thread, which
            // calls std::terminate — wind it down first.
            logger_->stop();
            logger_thread_->join();
            throw;
        }

        track(this);
    }

    ~PyMatchingEngine() {
        untrack(this);
        // A throwing destructor during Python garbage collection terminates the
        // process, so nothing may escape here.
        try {
            closeImpl(false);
        } catch (...) {
        }
        // Last resort: destroying a joinable std::thread also terminates, so if
        // closeImpl threw before the join, cut the thread loose instead.
        if (logger_thread_ && logger_thread_->joinable()) logger_thread_->detach();
    }

    PyMatchingEngine(const PyMatchingEngine&)            = delete;
    PyMatchingEngine& operator=(const PyMatchingEngine&) = delete;

    // Called from Python, where the GIL is held on entry.
    void close() { closeImpl(true); }

    bool closed() const { return closed_; }
    const std::string& logFile() const { return log_file_; }
    RiskParams riskParams() const { return risk_; }

    std::optional<order_id_t> addOrder(int price, int quantity, OrderSide side, OrderType type) {
        return engine().addOrder(price, quantity, type, side);
    }

    bool cancelOrder(order_id_t order_id) { return engine().cancelOrder(order_id); }

    std::optional<order_id_t> modifyOrder(order_id_t order_id, int quantity, int price,
                                          OrderSide side, OrderType type) {
        return engine().modifyOrder(order_id, quantity, price, side, type);
    }

    std::vector<PriceLevel> buyLevels(int num_levels) {
        return snapshot(engine().getBuySideView(checkLevels(num_levels)));
    }

    std::vector<PriceLevel> sellLevels(int num_levels) {
        return snapshot(engine().getSellSideView(checkLevels(num_levels)));
    }

    std::pair<std::vector<PriceLevel>, std::vector<PriceLevel>> book(int num_levels) {
        const auto view = engine().getOrderBookView(checkLevels(num_levels));
        return {snapshot(view.first), snapshot(view.second)};
    }

    // Runs from the module's teardown hook, where the interpreter is finalizing;
    // closeImpl is told not to touch the GIL there.
    static void closeAll() {
        std::lock_guard<std::mutex> lock(registryMutex());
        for (auto* engine : registry()) {
            try {
                engine->closeImpl(false);
            } catch (...) {
            }
        }
    }

    // Registered once from the module initializer.
    static void installForkHandlers() {
        pthread_atfork(&PyMatchingEngine::beforeFork,
                       &PyMatchingEngine::afterForkParent,
                       &PyMatchingEngine::afterForkChild);
    }

  private:
    Engine& engine() {
        if (forked_)
            throw std::runtime_error(
                "MatchingEngine cannot be used in a process forked from the one that "
                "created it: the logger thread does not survive fork()");
        if (closed_ || !engine_) throw std::runtime_error("MatchingEngine is closed");
        return *engine_;
    }

    // OrderBook::getBuySideView takes an int but forwards to
    // OrderBookSide::getBookSideView(unsigned), so a negative count wraps to about
    // four billion and quietly returns the entire book instead of failing.
    static int checkLevels(int num_levels) {
        if (num_levels < 1) throw std::invalid_argument("num_levels must be >= 1");
        return num_levels;
    }

    // allow_gil_release is false when the interpreter is finalizing: releasing the
    // GIL during teardown is not safe, and by then nothing else is running Python
    // anyway.
    void closeImpl(bool allow_gil_release) {
        if (closed_) return;
        closed_ = true;   // set first, so a throw below cannot cause a second join

        if (forked_) {
            abandonAfterFork();
            return;
        }

        engine_.reset();    // ~MatchingEngine publishes SESSION_CLOSE
        logger_->stop();    // the sentinel goes in behind it

        // The GIL is deliberately held across engine_.reset() above: another
        // thread may be inside a bound method dereferencing engine_, and it can
        // only be parked there while we hold the GIL. The join is the one
        // blocking step, so it is the only thing worth releasing the GIL for.
        if (allow_gil_release) {
            py::gil_scoped_release unlocked;
            logger_thread_->join();
        } else {
            logger_thread_->join();
        }

        logger_thread_.reset();
        logger_.reset();
    }

    // A forked child inherits the queue and the engine but not the logger thread,
    // and the queue's mutexes may have been held mid-push at the instant of the
    // fork. Nothing here can be torn down: publishing SESSION_CLOSE could block on
    // a mutex that will never be released, and joining a thread that does not
    // exist in this process throws. Leak all of it, deliberately.
    void abandonAfterFork() {
        new std::shared_ptr<EventQueue>(queue_);   // pin the queue for good
        (void) engine_.release();
        (void) logger_.release();
        (void) logger_thread_.release();
    }

    static void prepareLogFile(const std::string& path) {
        const std::filesystem::path file{path};
        if (file.has_parent_path() && !file.parent_path().empty()) {
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            if (ec)
                throw std::runtime_error("cannot create log directory '" +
                                         file.parent_path().string() + "': " + ec.message());
        }
        std::ofstream probe{path, std::ios::out};
        if (!probe) throw std::runtime_error("cannot open log file '" + path + "' for writing");
    }

    // Function-local statics: the registry has to be alive before the first engine
    // is constructed and after the last one is destroyed, and this ordering is
    // guaranteed where a namespace-scope static's is not.
    static std::mutex& registryMutex() {
        static std::mutex mutex;
        return mutex;
    }

    static std::unordered_set<PyMatchingEngine*>& registry() {
        static std::unordered_set<PyMatchingEngine*> engines;
        return engines;
    }

    static void track(PyMatchingEngine* engine) {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry().insert(engine);
    }

    static void untrack(PyMatchingEngine* engine) {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry().erase(engine);
    }

    // Standard pthread_atfork trio. The lock is taken before the fork and released
    // on both sides, so the child never inherits a registry mutex that was held by
    // a thread which no longer exists.
    static void beforeFork() { registryMutex().lock(); }
    static void afterForkParent() { registryMutex().unlock(); }
    static void afterForkChild() {
        for (auto* engine : registry()) engine->forked_ = true;
        registryMutex().unlock();
    }

    std::string log_file_;
    RiskParams  risk_;
    bool        closed_ = false;
    bool        forked_ = false;

    std::shared_ptr<EventQueue>  queue_;
    std::unique_ptr<EventLogger> logger_;          // Logger is immovable
    // A unique_ptr, not a plain std::thread: in a forked child the thread does not
    // exist, and both join() and detach() throw on it while destroying a joinable
    // std::thread terminates. Only a pointer can be abandoned.
    std::unique_ptr<std::thread> logger_thread_;
    std::unique_ptr<Engine>      engine_;          // MatchingEngine is immovable
};

std::string riskParamsRepr(const RiskParams& params) {
    std::ostringstream out;
    out << "RiskParams(max_allowed_quantity_quote=" << params.max_allowed_quantity_quote
        << ", min_allowed_quantity_quote=" << params.min_allowed_quantity_quote
        << ", max_price_book_top_deviation=" << params.max_price_book_top_deviation << ")";
    return out.str();
}

std::string priceLevelRepr(const PriceLevel& level) {
    std::ostringstream out;
    out << "PriceLevel(price=" << level.price << ", quantity=" << level.quantity
        << ", orders=" << level.order_ids.size() << ")";
    return out.str();
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "Single-symbol C++ matching engine.";

    PyMatchingEngine::installForkHandlers();

    py::enum_<OrderSide>(m, "OrderSide")
        .value("BUY", OrderSide::BUY)
        .value("SELL", OrderSide::SELL);

    py::enum_<OrderType>(m, "OrderType")
        .value("LIMIT", OrderType::LIMIT)
        .value("MARKET", OrderType::MARKET);

    const RiskParams defaults{};
    py::class_<RiskParams>(m, "RiskParams", "Pre-trade risk limits, fixed at engine construction.")
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

    py::class_<PriceLevel>(m, "PriceLevel",
                           "One price level, captured at the moment it was requested.")
        .def_readonly("price", &PriceLevel::price)
        .def_readonly("quantity", &PriceLevel::quantity)
        .def_readonly("order_ids", &PriceLevel::order_ids)
        .def("__repr__", &priceLevelRepr);

    py::class_<PyMatchingEngine>(m, "MatchingEngine", R"doc(
A single-symbol matching engine writing its event log to `log_file`.

Every operation publishes an event -- trades, adds, cancels, rejects -- to a
background logger thread that writes them to the log file. Nothing comes back to
Python: the log is the record.

The engine owns that thread, so it must be shut down. Use it as a context manager
or call close(); the log file is only complete once close() has returned.

Not thread-safe: the book has no internal locking, and these methods deliberately
hold the GIL, which is what serialises them. The engine also cannot be used in a
process forked from the one that created it -- the logger thread does not survive
fork() -- and will raise if you try.
)doc")
        .def(py::init<std::string, RiskParams>(),
             py::arg("log_file"), py::arg("risk_params") = RiskParams{})
        .def("add_order", &PyMatchingEngine::addOrder,
             py::arg("price"), py::arg("quantity"), py::arg("side"),
             py::arg("order_type") = OrderType::LIMIT,
             "Submit an order and match it against the book.\n\n"
             "Returns the new order id, or None if pre-trade risk rejected it. The\n"
             "reject reason is written to the log file and is not available here;\n"
             "the limits that produced it are on the risk_params property.")
        .def("cancel_order", &PyMatchingEngine::cancelOrder, py::arg("order_id"),
             "Cancel a resting order. Returns False if it is not in the book.")
        .def("modify_order", &PyMatchingEngine::modifyOrder,
             py::arg("order_id"), py::arg("quantity"), py::arg("price"), py::arg("side"),
             py::arg("order_type") = OrderType::LIMIT,
             "Amend a resting order, then match it.\n\n"
             "Returns the surviving order id -- a reprice retires the original and\n"
             "books a replacement under a fresh id, so this is not always the id you\n"
             "passed in. Returns None if risk rejected the amendment or the order is\n"
             "not in the book; the two are indistinguishable here.")
        .def("buy_levels", &PyMatchingEngine::buyLevels, py::arg("num_levels") = 1,
             "The top num_levels bids, best first.")
        .def("sell_levels", &PyMatchingEngine::sellLevels, py::arg("num_levels") = 1,
             "The top num_levels asks, best first.")
        .def("book", &PyMatchingEngine::book, py::arg("num_levels") = 1,
             "Both sides at once as (bids, asks).")
        // Deliberately no call_guard: close() needs the GIL for most of its work
        // and releases it itself, only around the thread join.
        .def("close", &PyMatchingEngine::close,
             "Shut down the engine and flush the log. Idempotent.")
        .def_property_readonly("closed", &PyMatchingEngine::closed)
        .def_property_readonly("log_file", &PyMatchingEngine::logFile)
        .def_property_readonly("risk_params", &PyMatchingEngine::riskParams)
        .def("__enter__", [](PyMatchingEngine& self) -> PyMatchingEngine& { return self; },
             py::return_value_policy::reference_internal)
        .def("__exit__",
             [](PyMatchingEngine& self, const py::object&, const py::object&, const py::object&) {
                 self.close();
                 return false;
             })
        .def("__repr__", [](const PyMatchingEngine& self) {
            return "<MatchingEngine log_file='" + self.logFile() + "'" +
                   (self.closed() ? " closed>" : ">");
        });

    // Backstop for an engine still referenced when the interpreter exits: the
    // capsule's destructor runs at module teardown. Without it the process would
    // exit without joining the logger thread, losing the tail of the log.
    m.add_object("_close_all_at_exit", py::capsule([] { PyMatchingEngine::closeAll(); }));
}
