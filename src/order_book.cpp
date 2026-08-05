#include "order_book.hpp"

#include <algorithm>
#include <iterator>


std::map<Price, PriceLevel, std::greater<Price>> bids;
std::map<Price, PriceLevel, std::less<Price>> asks;
std::unordered_map<OrderId, OrderLocation> orderIndex;


bool addOrder(const Order& order) {


    if (
    order.quantity == 0 ||
    order.price <= 0 ||
    order.symbol_id != DefaultSymbol ||
    orderIndex.contains(order.id)
) {
        return false;
}


    if (order.side == Side::Buy && order.quantity > 0 ) {

        PriceLevel& level = bids[order.price];
        //if (level.orders.)
        level.total_quantity += order.quantity;
        level.orders.push_back(order);

        auto orderIterator = std::prev(level.orders.end());

        orderIndex[order.id] = {
            order.side, order.price, orderIterator
        };


        return true;

    }
    if (order.side == Side::Sell && order.quantity > 0) {

        PriceLevel& level1 = asks[order.price];
        level1.orders.push_back(order);
        level1.total_quantity += order.quantity;

        auto orderIterator = std::prev(level1.orders.end());

        orderIndex[order.id] = {
            order.side, order.price, orderIterator
        };


        return true;
    }
    return false;

}



bool containsOrder(const OrderId id) {
    return orderIndex.contains(id);
}

std::optional<Price> bestBid() {
    if (bids.empty()) {
        return std::nullopt;
    }

    return bids.begin()->first;
}

std::optional<Price> bestAsk() {
    if (asks.empty()) {
        return std::nullopt;
    }

    return asks.begin()->first;
}

bool cancelOrder(OrderId id) {

    auto indexIterator = orderIndex.find(id);

    if (indexIterator == orderIndex.end()) {
        return false;
    }

    OrderLocation& location = indexIterator->second;

    if (location.side == Side::Buy) {
        auto levelIterator = bids.find(location.price);

        if (levelIterator == bids.end()) {
            return false;
        }

        PriceLevel& level = levelIterator->second;

        level.total_quantity -= location.iterator->quantity;
        level.orders.erase(location.iterator);

        if (level.orders.empty()) {
            bids.erase(levelIterator);
        }
    } else {
        auto levelIterator = asks.find(location.price);

        if (levelIterator == asks.end()) {
            return false;
        }

        PriceLevel& level = levelIterator->second;

        level.total_quantity -= location.iterator->quantity;
        level.orders.erase(location.iterator);

        if (level.orders.empty()) {
            asks.erase(levelIterator);
        }
    }

    orderIndex.erase(indexIterator);

    return true;
}

bool modifyOrder(
    OrderId id,
    Price newPrice,
    Quantity newQuantity
) {


    auto locationIterator =
        orderIndex.find(id);

    if (locationIterator == orderIndex.end()) {
        return false;
    }

    if (newQuantity == 0 || newPrice <= 0) {
        return false;
    }

    OrderLocation& location =
        locationIterator->second;

    Order& existingOrder =
        *location.iterator;

    if (
        location.price == newPrice &&
        existingOrder.quantity == newQuantity
    ) {
        return true;
    }

    /*
        A quantity reduction at the same price retains
        the order's original FIFO position.
    */
    if (
        location.price == newPrice &&
        newQuantity < existingOrder.quantity
    ) {
        Quantity reduction =
            existingOrder.quantity -
            newQuantity;

        if (location.side == Side::Buy) {
            auto levelIterator =
                bids.find(location.price);

            if (levelIterator == bids.end()) {
                return false;
            }

            if (
                levelIterator->second.total_quantity <
                reduction
            ) {
                return false;
            }

            levelIterator->second.total_quantity -=
                reduction;
        } else {
            auto levelIterator =
                asks.find(location.price);

            if (levelIterator == asks.end()) {
                return false;
            }

            if (
                levelIterator->second.total_quantity <
                reduction
            ) {
                return false;
            }

            levelIterator->second.total_quantity -=
                reduction;
        }

        existingOrder.quantity =
            newQuantity;

        return true;
    }

    /*
        A price change or a quantity increase loses
        FIFO priority, so cancel and reinsert.
    */
    Side side =
        location.side;

    ParticipantId participantId =
        existingOrder.participant_id;

    SymbolId symbolId =
        existingOrder.symbol_id;

    Order modifiedOrder{
        id,
        side,
        newPrice,
        newQuantity,
        participantId,
        symbolId
    };

    if (!cancelOrder(id)) {
        return false;
    }

    return addOrder(modifiedOrder);
}


std::vector<Trade> tradeHistory;

ExecutionResult executeOrder(Order incomingOrder) {


    if (
    incomingOrder.quantity == 0 ||
    incomingOrder.price <= 0 ||
    incomingOrder.symbol_id != DefaultSymbol ||
    orderIndex.contains(incomingOrder.id)
) {
        return ExecutionResult{
            false,
            0,
            incomingOrder.quantity,
            {}
        };
}

    Quantity originalQuantity = incomingOrder.quantity;
    std::vector<Trade> executionTrades;

    if (incomingOrder.side == Side::Buy) {
        while (incomingOrder.quantity > 0 && !asks.empty()) {
            auto levelIterator = asks.begin();

            // Lowest ask is above the buyer's limit.
            if (levelIterator->first > incomingOrder.price) {
                break;
            }

            PriceLevel& level = levelIterator->second;
            Order& restingOrder = level.orders.front();

            Quantity fillQuantity = std::min(
                incomingOrder.quantity,
                restingOrder.quantity
            );

            Trade trade{
                restingOrder.id,
                incomingOrder.id,
                restingOrder.price,
                fillQuantity,
                incomingOrder.side,
                restingOrder.participant_id,
                incomingOrder.participant_id,
                incomingOrder.symbol_id
            };

            executionTrades.push_back(trade);
            tradeHistory.push_back(trade);

            incomingOrder.quantity -= fillQuantity;
            restingOrder.quantity -= fillQuantity;
            level.total_quantity -= fillQuantity;

            if (restingOrder.quantity == 0) {
                OrderId makerId = restingOrder.id;

                orderIndex.erase(makerId);
                level.orders.pop_front();

                if (level.orders.empty()) {
                    asks.erase(levelIterator);
                }
            }
        }
    } else {
        while (incomingOrder.quantity > 0 && !bids.empty()) {
            auto levelIterator = bids.begin();

            // Highest bid is below the seller's limit.
            if (levelIterator->first < incomingOrder.price) {
                break;
            }

            PriceLevel& level = levelIterator->second;
            Order& restingOrder = level.orders.front();

            Quantity fillQuantity = std::min(
                incomingOrder.quantity,
                restingOrder.quantity
            );

            Trade trade{
                restingOrder.id,
                incomingOrder.id,
                restingOrder.price,
                fillQuantity,
                incomingOrder.side,
                restingOrder.participant_id,
                incomingOrder.participant_id,
                incomingOrder.symbol_id
            };

            executionTrades.push_back(trade);
            tradeHistory.push_back(trade);

            incomingOrder.quantity -= fillQuantity;
            restingOrder.quantity -= fillQuantity;
            level.total_quantity -= fillQuantity;

            if (restingOrder.quantity == 0) {
                OrderId makerId = restingOrder.id;

                orderIndex.erase(makerId);
                level.orders.pop_front();

                if (level.orders.empty()) {
                    bids.erase(levelIterator);
                }
            }
        }
    }

    // Any unfilled quantity becomes a resting order.
    if (incomingOrder.quantity > 0) {
        if (!addOrder(incomingOrder)) {
            return ExecutionResult{
                false,
                originalQuantity - incomingOrder.quantity,
                incomingOrder.quantity,
                executionTrades
            };
        }
    }

    Quantity executedQuantity =
        originalQuantity - incomingOrder.quantity;

    return ExecutionResult{
        true,
        executedQuantity,
        incomingOrder.quantity,
        executionTrades
    };
}

void resetOrderBook() {
    orderIndex.clear();
    bids.clear();
    asks.clear();
    tradeHistory.clear();
}