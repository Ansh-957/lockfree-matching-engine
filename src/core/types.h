#pragma once

// Core type definitions. All prices are integer ticks (1 tick = $0.01) to
// avoid floating point in the hot path - exact arithmetic, deterministic
// comparisons, and the tick count doubles as the price level array index

#include <cstdint>
#include <cmath>

namespace engine {

using OrderId   = uint64_t;  // unique, monotonically increasing
using Price     = int64_t;   // ticks; signed so price deltas are representable
using Quantity  = uint32_t;
using Timestamp = uint64_t;  // ns since epoch

enum class Side : uint8_t {
    Bid = 0,
    Ask = 1
};

enum class OrderType : uint8_t {
    Limit  = 0,  // rests at its price if not immediately filled
    Market = 1   // fills at best available price, never rests
};

inline constexpr Price TICK_SIZE = 1;

// price level array size: covers $0.00 to $99,999.99 at $0.01 tick
inline constexpr Price MAX_PRICE_TICKS = 10'000'000;

// only used in conversion helpers at the system boundary, never in matching
inline constexpr double DOLLARS_PER_TICK = 0.01;

// 150.25 -> 15025, rounded to nearest tick
[[nodiscard]] inline Price dollars_to_ticks(double price) noexcept {
    return static_cast<Price>(std::round(price / DOLLARS_PER_TICK));
}

// 15025 -> 150.25
[[nodiscard]] inline double ticks_to_dollars(Price ticks) noexcept {
    return static_cast<double>(ticks) * DOLLARS_PER_TICK;
}

} // namespace engine
