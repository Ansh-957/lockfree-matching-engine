#pragma once

/// @file types.h
/// @brief Core type definitions for the matching engine.
///
/// All prices are represented as integer ticks to avoid floating-point arithmetic
/// in the hot path. Each tick represents $0.01. This gives us exact arithmetic
/// and deterministic behavior across platforms.

#include <cstdint>
#include <cmath>

namespace engine {

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

/// Unique identifier for each order. Monotonically increasing.
using OrderId   = uint64_t;

/// Price in ticks. 1 tick = $0.01. Signed to allow representing price deltas.
using Price     = int64_t;

/// Order quantity in shares/units. uint32_t supports up to ~4.2 billion units.
using Quantity  = uint32_t;

/// Timestamp in nanoseconds since Unix epoch. uint64_t covers ~584 years.
using Timestamp = uint64_t;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// Order side: Bid (buy) or Ask (sell).
enum class Side : uint8_t {
    Bid = 0,  ///< Buy side
    Ask = 1   ///< Sell side
};

/// Order type: Limit (rests in book) or Market (immediate execution).
enum class OrderType : uint8_t {
    Limit  = 0,  ///< Limit order — rests at specified price if not immediately filled
    Market = 1   ///< Market order — fills at best available price, never rests in book
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Each tick represents $0.01.
inline constexpr Price TICK_SIZE = 1;

/// Maximum number of price levels: supports $0.00 to $99,999.99.
/// This defines the size of the price-level array in the order book.
/// At sizeof(PriceLevel) ≈ 24 bytes * 10M levels ≈ 240 MB.
inline constexpr Price MAX_PRICE_TICKS = 10'000'000;

/// Dollars per tick — used only in conversion helpers, never in hot path.
inline constexpr double DOLLARS_PER_TICK = 0.01;

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------

/// Convert a dollar price (e.g. 150.25) to integer ticks (15025).
/// @param price  Dollar price as a double.
/// @return Price in ticks, rounded to nearest tick.
[[nodiscard]] inline Price dollars_to_ticks(double price) noexcept {
    return static_cast<Price>(std::round(price / DOLLARS_PER_TICK));
}

/// Convert integer ticks back to a dollar price.
/// @param ticks  Price in ticks.
/// @return Dollar price as a double.
[[nodiscard]] inline double ticks_to_dollars(Price ticks) noexcept {
    return static_cast<double>(ticks) * DOLLARS_PER_TICK;
}

} // namespace engine
