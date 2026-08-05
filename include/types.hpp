#pragma once

#include <cstdint>

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint32_t;

using ParticipantId = std::uint64_t;
using SymbolId = std::uint32_t;
using TimestampNs = std::uint64_t;

inline constexpr ParticipantId UnknownParticipant = 0;
inline constexpr SymbolId DefaultSymbol = 1;

enum class Side {
    Buy,
    Sell
};

struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;

    ParticipantId participant_id{
        UnknownParticipant
    };

    SymbolId symbol_id{
        DefaultSymbol
    };
};