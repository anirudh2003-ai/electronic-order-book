#pragma once

#include "types.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

using OrderQueue = std::list<Order>;

using AggregateQuantity =
    std::uint64_t;

struct PriceLevel {
    AggregateQuantity total_quantity{0};
    OrderQueue orders;
};

struct OrderLocation {
    Side side;
    Price price;
    OrderQueue::iterator iterator;
};

extern std::map<
    Price,
    PriceLevel,
    std::greater<Price>
> bids;

extern std::map<
    Price,
    PriceLevel,
    std::less<Price>
> asks;

extern std::unordered_map<
    OrderId,
    OrderLocation
> orderIndex;

bool addOrder(
    const Order& order
);

bool containsOrder(
    OrderId id
);

std::optional<Price> bestBid();

std::optional<Price> bestAsk();

bool cancelOrder(
    OrderId id
);

bool modifyOrder(
    OrderId id,
    Price newPrice,
    Quantity newQuantity
);

struct Trade {
    OrderId maker_order_id;
    OrderId taker_order_id;
    Price price;
    Quantity quantity;
    Side aggressor_side;

    ParticipantId maker_participant_id{
        UnknownParticipant
    };

    ParticipantId taker_participant_id{
        UnknownParticipant
    };

    SymbolId symbol_id{
        DefaultSymbol
    };
};

extern std::vector<Trade> tradeHistory;

struct ExecutionResult {
    bool accepted;
    Quantity executed_quantity;
    Quantity remaining_quantity;
    std::vector<Trade> trades;
};

ExecutionResult executeOrder(
    Order incomingOrder
);

void resetOrderBook();