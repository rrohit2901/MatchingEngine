// bench/bench_latency.cpp
//
// Per-operation latency harness. Reports tail percentiles (P50 / P99 / P99.9) in
// nanoseconds for the core OrderBook operations.
//
// Why this exists separately from bench_add / bench_modify / bench_cancel:
// Google Benchmark reports the *mean* time per op averaged over a large inner
// loop, which hides tail latency. For a matching engine the tail is what matters,
// so this harness times every operation individually, collects the samples, sorts
// them, and computes true percentiles.
//
// Run:   ./build-release/bench/bench_latency
//        ./build-release/bench/bench_latency 2000000   (override sample count)
//
// Build in Release for meaningful numbers:
//   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
//   cmake --build build-release --target bench_latency

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Order.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "lock_queue.h"

namespace {

using Clock = std::chrono::steady_clock;
using EventQueue = LockQueue<std::unique_ptr<Event>>;

// Prices are ints; this used to be a double that converted at every call site.
constexpr int kBasePrice = 100;
constexpr int kQuantity = 10;
constexpr int kPriceLevels = 100;  // bounded spread for add
constexpr int kBookLevels = 100;   // background depth for cancel/modify
constexpr int kWarmup = 50000;

// Volatile sink so the optimizer cannot discard the operation we are timing.
volatile int g_sink = 0;

double nanos(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::nano>(b - a).count();
}

struct Stats {
    std::size_t count = 0;
    double min = 0, p50 = 0, p99 = 0, p999 = 0, max = 0, mean = 0;
};

// Sorts in place and extracts percentiles (nearest-rank).
Stats summarize(std::vector<double>& s) {
    Stats out;
    if (s.empty()) {
        return out;
    }
    std::sort(s.begin(), s.end());
    const auto at = [&](double q) {
        const std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(s.size() - 1));
        return s[idx];
    };
    double sum = 0.0;
    for (const double v : s) {
        sum += v;
    }
    out.count = s.size();
    out.min = s.front();
    out.p50 = at(0.50);
    out.p99 = at(0.99);
    out.p999 = at(0.999);
    out.max = s.back();
    out.mean = sum / static_cast<double>(s.size());
    return out;
}

// Median cost of a back-to-back Clock::now() pair, so reported op latencies can be
// interpreted relative to the timer's own overhead.
double clockOverheadNs() {
    std::vector<double> s;
    s.reserve(kWarmup);
    for (int i = 0; i < kWarmup; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        s.push_back(nanos(a, b));
    }
    std::sort(s.begin(), s.end());
    return s[s.size() / 2];
}

// add: rest a limit order, spread across kPriceLevels distinct prices.
//
// Note: BookLevel::cancelOrder only tombstones an order; a level's order vector is
// freed only when the level fully drains. So we cap depth by periodically rebuilding
// the book (untimed) every kAddResetEvery orders, keeping the per-level vectors at a
// realistic size (~kAddResetEvery / kPriceLevels). This isolates add cost from the
// unbounded-vector-growth reallocs that would otherwise dominate the tail.
constexpr int kAddResetEvery = 100000;  // ~1k orders per level before rebuild

Stats measureAdd(int n) {
    OrderBook book;
    std::vector<double> s;
    s.reserve(static_cast<std::size_t>(n));
    int i = 0;
    for (int w = 0; w < kWarmup; ++w) {
        g_sink = book.addOrder(kBasePrice + (i++ % kPriceLevels), kQuantity, OrderType::LIMIT, OrderSide::BUY);
    }
    for (int k = 0; k < n; ++k) {
        if (k % kAddResetEvery == 0) {
            book = OrderBook{};  // untimed: bound per-level vector size
        }
        const double price = kBasePrice + (i++ % kPriceLevels);
        const auto t0 = Clock::now();
        const int id = book.addOrder(price, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        const auto t1 = Clock::now();
        g_sink = id;
        s.push_back(nanos(t0, t1));
    }
    return summarize(s);
}

// cancel: against a book holding kBookLevels of background liquidity. The order being
// cancelled is added untimed each iteration so only the cancel is measured.
Stats measureCancel(int n) {
    OrderBook book;
    for (int level = 0; level < kBookLevels; ++level) {
        book.addOrder(kBasePrice + level, kQuantity, OrderType::LIMIT, OrderSide::BUY);
    }
    std::vector<double> s;
    s.reserve(static_cast<std::size_t>(n));
    int i = 0;
    for (int w = 0; w < kWarmup; ++w) {
        const int id = book.addOrder(kBasePrice + kBookLevels + (i++ % kBookLevels), kQuantity, OrderType::LIMIT, OrderSide::BUY);
        g_sink = book.cancelOrder(id);
    }
    for (int k = 0; k < n; ++k) {
        const double price = kBasePrice + kBookLevels + (i++ % kBookLevels);
        const int id = book.addOrder(price, kQuantity, OrderType::LIMIT, OrderSide::BUY);  // untimed setup
        const auto t0 = Clock::now();
        const bool ok = book.cancelOrder(id);
        const auto t1 = Clock::now();
        g_sink = ok;
        s.push_back(nanos(t0, t1));
    }
    return summarize(s);
}

// modify: reprice to a new level (the common cancel-from-old + add-to-new path). Each
// order is rested untimed and cancelled untimed afterwards to keep the book bounded.
Stats measureModify(int n) {
    OrderBook book;
    std::vector<double> s;
    s.reserve(static_cast<std::size_t>(n));
    int i = 0;
    for (int w = 0; w < kWarmup; ++w) {
        const int id = book.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        const auto modified = book.modifyOrder(id, kQuantity + 1, kBasePrice + 1 + (i++ % kPriceLevels), OrderSide::BUY, OrderType::LIMIT);
        g_sink = modified.has_value();
        book.cancelOrder(modified.value_or(id));
    }
    for (int k = 0; k < n; ++k) {
        const int id = book.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);  // untimed setup
        const int newPrice = kBasePrice + 1 + (i++ % kPriceLevels);
        const auto t0 = Clock::now();
        const auto modified = book.modifyOrder(id, kQuantity + 1, newPrice, OrderSide::BUY, OrderType::LIMIT);
        const auto t1 = Clock::now();
        g_sink = modified.has_value();
        // A reprice mints a new id; cancelling `id` here would leak the repriced
        // order and let the book grow across all n samples.
        book.cancelOrder(modified.value_or(id));  // untimed cleanup keeps the book bounded
        s.push_back(nanos(t0, t1));
    }
    return summarize(s);
}

// --- MatchingEngine path ----------------------------------------------------
// The OrderBook rows above measure the book in isolation. These measure what a
// caller actually pays now: risk checks, event publishing, and matching on top
// of the same book operation.
//
// Events are drained by a background thread that discards them, mirroring the
// Logger without its file I/O — the point is the producer-side cost on the hot
// path, not how fast the consumer writes.
struct EventDrain {
    std::shared_ptr<EventQueue> queue{std::make_shared<EventQueue>()};
    std::thread worker;

    EventDrain() {
        worker = std::thread([q = queue] {
            while (true) {
                std::unique_ptr<Event> event;
                q->wait_and_pop(event);
                if (!event) break;
            }
        });
    }
    ~EventDrain() {
        queue->push(nullptr);
        worker.join();
    }
};

using Engine = MatchingEngine<LockQueue>;

Stats measureEngineAdd(int n) {
    EventDrain drain;
    auto engine = std::make_unique<Engine>(drain.queue);
    std::vector<double> s;
    s.reserve(static_cast<std::size_t>(n));
    int i = 0;
    for (int w = 0; w < kWarmup; ++w) {
        g_sink = engine->addOrder(kBasePrice + (i++ % kPriceLevels), kQuantity, OrderType::LIMIT, OrderSide::BUY).has_value();
    }
    for (int k = 0; k < n; ++k) {
        if (k % kAddResetEvery == 0) {
            engine = std::make_unique<Engine>(drain.queue);  // untimed: bound the book
        }
        const int price = kBasePrice + (i++ % kPriceLevels);
        const auto t0 = Clock::now();
        const auto id = engine->addOrder(price, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        const auto t1 = Clock::now();
        g_sink = id.has_value();
        s.push_back(nanos(t0, t1));
    }
    return summarize(s);
}

Stats measureEngineModify(int n) {
    EventDrain drain;
    Engine engine{drain.queue};
    std::vector<double> s;
    s.reserve(static_cast<std::size_t>(n));
    int i = 0;
    for (int w = 0; w < kWarmup; ++w) {
        const auto id = engine.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        const auto modified = engine.modifyOrder(id.value(), kQuantity + 1, kBasePrice + 1 + (i++ % kPriceLevels), OrderSide::BUY, OrderType::LIMIT);
        engine.cancelOrder(modified.value_or(id.value()));
    }
    for (int k = 0; k < n; ++k) {
        const auto id = engine.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);  // untimed setup
        const int newPrice = kBasePrice + 1 + (i++ % kPriceLevels);
        const auto t0 = Clock::now();
        const auto modified = engine.modifyOrder(id.value(), kQuantity + 1, newPrice, OrderSide::BUY, OrderType::LIMIT);
        const auto t1 = Clock::now();
        g_sink = modified.has_value();
        engine.cancelOrder(modified.value_or(id.value()));  // untimed cleanup
        s.push_back(nanos(t0, t1));
    }
    return summarize(s);
}

Stats measureEngineCancel(int n) {
    EventDrain drain;
    Engine engine{drain.queue};
    for (int level = 0; level < kBookLevels; ++level) {
        engine.addOrder(kBasePrice + level, kQuantity, OrderType::LIMIT, OrderSide::BUY);
    }
    std::vector<double> s;
    s.reserve(static_cast<std::size_t>(n));
    int i = 0;
    for (int w = 0; w < kWarmup; ++w) {
        const auto id = engine.addOrder(kBasePrice + kBookLevels + (i++ % kBookLevels), kQuantity, OrderType::LIMIT, OrderSide::BUY);
        g_sink = engine.cancelOrder(id.value());
    }
    for (int k = 0; k < n; ++k) {
        const int price = kBasePrice + kBookLevels + (i++ % kBookLevels);
        const auto id = engine.addOrder(price, kQuantity, OrderType::LIMIT, OrderSide::BUY);  // untimed setup
        const auto t0 = Clock::now();
        const bool ok = engine.cancelOrder(id.value());
        const auto t1 = Clock::now();
        g_sink = ok;
        s.push_back(nanos(t0, t1));
    }
    return summarize(s);
}

void printHeader() {
    std::printf("%-22s %10s %8s %8s %9s %8s %9s %9s\n",
                "operation", "samples", "P50", "P99", "P99.9", "min", "max", "mean");
    std::printf("%-22s %10s %8s %8s %9s %8s %9s %9s\n",
                "", "", "(ns)", "(ns)", "(ns)", "(ns)", "(ns)", "(ns)");
    std::printf("-------------------------------------------------------------------------------------------\n");
}

void printRow(const std::string& name, const Stats& st) {
    std::printf("%-22s %10zu %8.1f %8.1f %9.1f %8.1f %9.1f %9.1f\n",
                name.c_str(), st.count, st.p50, st.p99, st.p999, st.min, st.max, st.mean);
}

}  // namespace

int main(int argc, char** argv) {
    int n = 1000000;
    if (argc > 1) {
        n = std::atoi(argv[1]);
        if (n <= 0) {
            n = 1000000;
        }
    }

    const double overhead = clockOverheadNs();

    std::printf("Per-operation latency  (samples per op: %d)\n"
                "  OrderBook rows = book only; Engine rows add risk checks, event publishing and matching\n", n);
    std::printf("steady_clock overhead (median of a now() pair): %.1f ns\n\n", overhead);

    printHeader();
    printRow("OrderBook add", measureAdd(n));
    printRow("OrderBook modify", measureModify(n));
    printRow("OrderBook cancel", measureCancel(n));
    printRow("Engine add", measureEngineAdd(n));
    printRow("Engine modify", measureEngineModify(n));
    printRow("Engine cancel", measureEngineCancel(n));

    std::printf("\nNote: latencies include one steady_clock::now() pair (~%.1f ns); CPU "
                "frequency scaling is not pinned, so absolute numbers vary run to run.\n",
                overhead);
    return 0;
}
