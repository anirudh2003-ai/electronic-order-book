#include "market_event.hpp"

MarketEvent makeAddEvent(
    std::uint64_t sequenceNumber,
    std::uint64_t timestampNs,
    const Order& order
) {
    return MarketEvent{
        sequenceNumber,
        timestampNs,
        EventType::Add,
        order.id,
        order.side,
        order.price,
        order.quantity,
        order.participant_id,
        order.symbol_id
    };
}

MarketEvent makeCancelEvent(
    std::uint64_t sequenceNumber,
    std::uint64_t timestampNs,
    OrderId orderId,
    ParticipantId participantId,
    SymbolId symbolId
) {
    return MarketEvent{
        sequenceNumber,
        timestampNs,
        EventType::Cancel,
        orderId,
        Side::Buy,
        0,
        0,
        participantId,
        symbolId
    };
}

MarketEvent makeModifyEvent(
    std::uint64_t sequenceNumber,
    std::uint64_t timestampNs,
    OrderId orderId,
    Price newPrice,
    Quantity newQuantity,
    ParticipantId participantId,
    SymbolId symbolId
) {
    return MarketEvent{
        sequenceNumber,
        timestampNs,
        EventType::Modify,
        orderId,
        Side::Buy,
        newPrice,
        newQuantity,
        participantId,
        symbolId
    };
}

MarketEvent makeExecuteEvent(
    std::uint64_t sequenceNumber,
    std::uint64_t timestampNs,
    const Order& incomingOrder
) {
    return MarketEvent{
        sequenceNumber,
        timestampNs,
        EventType::Execute,
        incomingOrder.id,
        incomingOrder.side,
        incomingOrder.price,
        incomingOrder.quantity,
        incomingOrder.participant_id,
        incomingOrder.symbol_id
    };
}

bool validateMarketEvent(
    const MarketEvent& event,
    std::string& reason
) {
    if (event.sequence_number == 0) {
        reason =
            "sequence number cannot be zero";

        return false;
    }

    if (event.order_id == 0) {
        reason =
            "order ID cannot be zero";

        return false;
    }

    if (event.symbol_id != DefaultSymbol) {
        reason =
            "this order book supports only DefaultSymbol";

        return false;
    }

    switch (event.type) {
        case EventType::Add:
        case EventType::Execute: {
            if (event.price <= 0) {
                reason =
                    "add/execute price must be positive";

                return false;
            }

            if (event.quantity == 0) {
                reason =
                    "add/execute quantity must be positive";

                return false;
            }

            return true;
        }

        case EventType::Modify: {
            if (event.price <= 0) {
                reason =
                    "modify price must be positive";

                return false;
            }

            if (event.quantity == 0) {
                reason =
                    "modify quantity must be positive";

                return false;
            }

            return true;
        }

        case EventType::Cancel:
            return true;
    }

    reason = "unknown event type";
    return false;
}