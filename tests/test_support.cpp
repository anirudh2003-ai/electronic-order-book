#include "test_support.hpp"

#include <cstdint>

std::vector<OrderId> getQueueIds(const OrderQueue& queue) {
    std::vector<OrderId> ids;

    for (const Order& order : queue) {
        ids.push_back(order.id);
    }

    return ids;
}

void checkTrade(
    const Trade& trade,
    OrderId expectedMaker,
    OrderId expectedTaker,
    Price expectedPrice,
    Quantity expectedQuantity,
    Side expectedAggressor
) {
    CHECK(trade.maker_order_id == expectedMaker);
    CHECK(trade.taker_order_id == expectedTaker);
    CHECK(trade.price == expectedPrice);
    CHECK(trade.quantity == expectedQuantity);
    CHECK(trade.aggressor_side == expectedAggressor);
}

void checkExecutionArithmetic(
    const ExecutionResult& result,
    Quantity originalQuantity
) {
    std::uint64_t sumOfTrades = 0;

    for (const Trade& trade : result.trades) {
        sumOfTrades += trade.quantity;
    }

    CHECK(sumOfTrades == result.executed_quantity);

    CHECK(
        originalQuantity ==
        result.executed_quantity + result.remaining_quantity
    );
}