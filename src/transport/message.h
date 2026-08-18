#pragma once

// Inter-thread message types. Everything here must be fixed-size and
// trivially copyable so it can pass through the SPSC ring buffers

#include <type_traits>
#include <variant>

#include "core/types.h"

namespace engine {

// feed -> engine

struct NewOrderMessage {
    OrderId   id        = 0;
    Side      side      = Side::Bid;
    OrderType type      = OrderType::Limit;
    Price     price     = 0;  // ticks; ignored for market orders
    Quantity  quantity  = 0;
    Timestamp timestamp = 0;
};

struct CancelMessage {
    OrderId   id        = 0;
    Timestamp timestamp = 0;
};

// engine -> metrics/logging

struct FillMessage {
    OrderId   aggressive_id = 0;  // incoming (taker) order
    OrderId   passive_id    = 0;  // resting (maker) order
    Price     price         = 0;  // execution price in ticks
    Quantity  quantity      = 0;
    Timestamp timestamp     = 0;
};

struct BookUpdateMessage {
    Side     side               = Side::Bid;
    Price    price              = 0;
    Quantity new_total_quantity = 0;
};

// tagged unions for the queues. A variant over trivially copyable types is
// itself trivially copyable, which the SPSC queue static_asserts
using EngineMessage = std::variant<NewOrderMessage, CancelMessage>;
using OutputMessage = std::variant<FillMessage, BookUpdateMessage>;

static_assert(std::is_trivially_copyable_v<EngineMessage>);
static_assert(std::is_trivially_copyable_v<OutputMessage>);

} // namespace engine
