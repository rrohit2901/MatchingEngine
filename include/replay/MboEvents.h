#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// One trading day of Databento data, as the flat columns that
// scripts/convert_mbo.py writes to Parquet. The replay never owns or parses
// files: the caller (Python, or a test) holds the arrays and hands over spans,
// so the same code runs on a day loaded from Parquet and on a hand-written
// event sequence.
//
// Units, as written by convert_mbo.py:
//   ts_recv      nanoseconds since the UNIX epoch, UTC
//   action/side  one ASCII byte: action A C M R T F N, side B A N
//   price        ticks of 1e-4 dollars; PRICE_UNDEF when the venue sent none
//   size         shares
//
// Every column must have the same length; size() is the length of ts_recv.

namespace mbo {
// Record flags (databento_dbn's FlagSet).
inline constexpr uint8_t F_LAST = 1u << 7;            // last record of a venue event
inline constexpr uint8_t F_TOB = 1u << 6;
inline constexpr uint8_t F_SNAPSHOT = 1u << 5;
inline constexpr uint8_t F_MBP = 1u << 4;
inline constexpr uint8_t F_BAD_TS_RECV = 1u << 3;
inline constexpr uint8_t F_MAYBE_BAD_BOOK = 1u << 2;

inline constexpr int32_t PRICE_UNDEF = INT32_MAX;
}  // namespace mbo

// Market-by-order: every add, cancel, modify, clear, trade and fill.
struct MboEvents {
    std::span<const uint64_t> ts_recv;
    std::span<const uint8_t> action;
    std::span<const uint8_t> side;
    std::span<const int32_t> price;
    std::span<const uint32_t> size;
    std::span<const uint64_t> order_id;
    std::span<const uint8_t> flags;
    std::span<const uint32_t> sequence;

    size_t count() const { return ts_recv.size(); }
    // True when every column has count() entries.
    bool consistent() const {
        const size_t n = count();
        return action.size() == n && side.size() == n && price.size() == n && size.size() == n &&
               order_id.size() == n && flags.size() == n && sequence.size() == n;
    }
};

// Nasdaq's own top of book after each update (Databento mbp-1), used only to
// validate the book the replay rebuilds. An empty side has price PRICE_UNDEF.
struct Mbp1Events {
    std::span<const uint64_t> ts_recv;
    std::span<const uint32_t> sequence;
    std::span<const uint8_t> flags;
    std::span<const int32_t> bid_px;
    std::span<const int32_t> ask_px;
    std::span<const uint32_t> bid_sz;
    std::span<const uint32_t> ask_sz;

    size_t count() const { return ts_recv.size(); }
    bool consistent() const {
        const size_t n = count();
        return sequence.size() == n && flags.size() == n && bid_px.size() == n && ask_px.size() == n &&
               bid_sz.size() == n && ask_sz.size() == n;
    }
};
