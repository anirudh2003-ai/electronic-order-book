#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>

enum class EventType : std::uint8_t {
    Add = 1,
    Cancel = 2,
    Modify = 3,
    Execute = 4
};

struct MarketEvent {
    std::uint64_t sequence_number;
    std::uint64_t timestamp_ns;
    EventType type;
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;

    ParticipantId participant_id{
        UnknownParticipant
    };

    SymbolId symbol_id{
        DefaultSymbol
    };

    bool operator==(
        const MarketEvent&
    ) const = default;
};

MarketEvent makeAddEvent(
    std::uint64_t sequenceNumber,
    std::uint64_t timestampNs,
    const Order& order
);

MarketEvent makeCancelEvent(
    std::uint64_t sequenceNumber,
    std::uint64_t timestampNs,
    OrderId orderId,
    ParticipantId participantId =
        UnknownParticipant,
    SymbolId symbolId =
        DefaultSymbol
);

MarketEvent makeModifyEvent(
    std::uint64_t sequenceNumber,
    std::uint64_t timestampNs,
    OrderId orderId,
    Price newPrice,
    Quantity newQuantity,
    ParticipantId participantId =
        UnknownParticipant,
    SymbolId symbolId =
        DefaultSymbol
);

MarketEvent makeExecuteEvent(
    std::uint64_t sequenceNumber,
    std::uint64_t timestampNs,
    const Order& incomingOrder
);

bool validateMarketEvent(
    const MarketEvent& event,
    std::string& reason
);