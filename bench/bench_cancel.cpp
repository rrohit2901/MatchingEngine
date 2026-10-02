// bench/bench_cancel.cpp
//
// Latency micro-benchmarks for OrderBook::cancelOrder. All timings in nanoseconds.
//
// cancelOrder consumes the order it removes, so each iteration first adds the order to cancel
// under Pause/ResumeTiming; only the cancel itself is inside the timed window.
//
// Run:   ./build/bench/bench_cancel
//        ./build/bench/bench_cancel --benchmark_filter=FromDeepBook
//
// Build in Release for meaningful numbers (the project defaults to Release):
//   cmake -S . -B build && cmake --build build --target bench_cancel

#include <benchmark/benchmark.h>

#include "Order.h"
#include "OrderBook.h"

namespace {

constexpr double kBasePrice = 100.0;
constexpr int kQuantity = 10;
constexpr int kLevelDepth = 100;  // resting orders sharing a level in OneOfManyOnLevel
constexpr int kBookLevels = 100;  // distinct levels held in FromDeepBook

// Cancel the only order at its price: removing it empties the level and triggers level teardown
// (priceLevels.erase).
void BM_Cancel_LastOrderOnLevel(benchmark::State& state) {
    OrderBook book;
    int i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        // Distinct price each iteration so this order is the sole occupant of its level.
        const double price = kBasePrice + i++;
        order_id_t id = book.addOrder(price, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        state.ResumeTiming();

        bool ok = book.cancelOrder(id);
        benchmark::DoNotOptimize(ok);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cancel_LastOrderOnLevel)->Unit(benchmark::kNanosecond);

// Cancel one order out of many resting at the same price: the level survives, so this measures
// removal from a populated level (no teardown).
void BM_Cancel_OneOfManyOnLevel(benchmark::State& state) {
    OrderBook book;
    // Pre-fill the level so it always has neighbours; refilled when it gets thin.
    auto fill = [&] {
        for (int n = 0; n < kLevelDepth; ++n) {
            book.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        }
    };
    fill();

    int added = kLevelDepth;
    for (auto _ : state) {
        state.PauseTiming();
        if (added <= 1) {
            fill();
            added += kLevelDepth;
        }
        order_id_t id = book.addOrder(kBasePrice, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        ++added;
        state.ResumeTiming();

        bool ok = book.cancelOrder(id);
        benchmark::DoNotOptimize(ok);
        --added;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cancel_OneOfManyOnLevel)->Unit(benchmark::kNanosecond);

// Cancel an order while the book holds many other price levels: measures cancel cost against a
// deep book (lookup + level erase in a populated tree).
void BM_Cancel_FromDeepBook(benchmark::State& state) {
    OrderBook book;
    // Background liquidity across many distinct levels, left resting for the whole run.
    for (int level = 0; level < kBookLevels; ++level) {
        book.addOrder(kBasePrice + level, kQuantity, OrderType::LIMIT, OrderSide::BUY);
    }

    int i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        // New level above the background book so the cancel tears its own level down.
        const double price = kBasePrice + kBookLevels + (i++ % kBookLevels);
        order_id_t id = book.addOrder(price, kQuantity, OrderType::LIMIT, OrderSide::BUY);
        state.ResumeTiming();

        bool ok = book.cancelOrder(id);
        benchmark::DoNotOptimize(ok);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cancel_FromDeepBook)->Unit(benchmark::kNanosecond);

}  // namespace

BENCHMARK_MAIN();
