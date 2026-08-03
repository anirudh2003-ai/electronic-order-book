#include <cstdint>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <iterator>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <string>
#include <vector>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>
#include <cmath>
#include "analytics/telemetry_recorder.hpp"
#include <unordered_set>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <thread>

using OrderId = std::uint64_t;
using Price = std::int64_t;       // Store price in pence/ticks
using Quantity = std::uint32_t;

using ParticipantId = std::uint64_t;
using SymbolId = std::uint32_t;
using TimestampNs = std::uint64_t;
constexpr ParticipantId UnknownParticipant = 0;
constexpr SymbolId DefaultSymbol = 1;
class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};

        int result =
            WSAStartup(
                MAKEWORD(2, 2),
                &data
            );

        if (result != 0) {
            throw std::runtime_error(
                "WSAStartup failed: " +
                std::to_string(result)
            );
        }
    }

    ~WinsockRuntime() {
        WSACleanup();
    }

    WinsockRuntime(
        const WinsockRuntime&
    ) = delete;

    WinsockRuntime& operator=(
        const WinsockRuntime&
    ) = delete;
};

template<
    typename T,
    std::size_t Capacity
>
class SpscRingBuffer {
public:
    static_assert(
        Capacity >= 2
    );

    static_assert(
        (Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of two"
    );

    bool tryPush(const T& value) {
        std::size_t write =
            write_index_.load(
                std::memory_order_relaxed
            );

        std::size_t next =
            (write + 1) &
            (Capacity - 1);

        if (
            next ==
            read_index_.load(
                std::memory_order_acquire
            )
        ) {
            return false;
        }

        values_[write] = value;

        write_index_.store(
            next,
            std::memory_order_release
        );

        return true;
    }

    bool tryPop(T& value) {
        std::size_t read =
            read_index_.load(
                std::memory_order_relaxed
            );

        if (
            read ==
            write_index_.load(
                std::memory_order_acquire
            )
        ) {
            return false;
        }

        value = values_[read];

        read_index_.store(
            (read + 1) &
                (Capacity - 1),
            std::memory_order_release
        );

        return true;
    }

    std::size_t approximateSize() const {
        std::size_t write =
            write_index_.load(
                std::memory_order_acquire
            );

        std::size_t read =
            read_index_.load(
                std::memory_order_acquire
            );

        if (write >= read) {
            return write - read;
        }

        return Capacity - read + write;
    }

private:
    std::array<T, Capacity> values_{};

    alignas(64)
    std::atomic<std::size_t>
        write_index_{0};

    alignas(64)
    std::atomic<std::size_t>
        read_index_{0};
};


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

std::vector<Trade> tradeHistory;

struct ExecutionResult {
    bool accepted;
    Quantity executed_quantity;
    Quantity remaining_quantity;
    std::vector<Trade> trades;
};

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


// ============================================================
// NORMALISED MARKET EVENTS
// ============================================================

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

    bool operator==(const MarketEvent&) const = default;
};

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
    ParticipantId participantId =
        UnknownParticipant,
    SymbolId symbolId =
        DefaultSymbol
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
    ParticipantId participantId =
        UnknownParticipant,
    SymbolId symbolId =
        DefaultSymbol
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


struct LobsterActiveOrder {
    Side side;
    Price price;
    Quantity quantity;
};

std::vector<MarketEvent> loadLobsterMessageFile(
    const std::filesystem::path& filePath
) {
    std::ifstream input{filePath};

    if (!input.is_open()) {
        throw std::runtime_error(
            "Unable to open LOBSTER file: " +
            filePath.string()
        );
    }

    std::vector<MarketEvent> convertedEvents;

    std::unordered_map<
        OrderId,
        LobsterActiveOrder
    > activeOrders;

    std::string line;
    std::uint64_t nextSequence = 1;
    std::size_t sourceRow = 0;
    std::size_t skippedMissingOrders = 0;
    std::size_t hiddenExecutions = 0;
    std::size_t ignoredControlEvents = 0;

    /*
        Reduce the quantity of an existing resting order.

        LOBSTER event types 2 and 4 report the quantity
        removed, not the new remaining quantity.
    */
    auto reduceExistingOrder =
        [&](
            OrderId orderId,
            Quantity removedQuantity,
            TimestampNs timestamp
        ) {
            auto iterator =
    activeOrders.find(orderId);

            if (iterator == activeOrders.end()) {
                ++skippedMissingOrders;

                std::cerr
                    << "LOBSTER warning: row "
                    << sourceRow
                    << " references missing order "
                    << orderId
                    << " during partial cancellation/execution.\n";

                return;
            }

            LobsterActiveOrder& existingOrder =
                iterator->second;

            if (
    removedQuantity >
    existingOrder.quantity
) {
                throw std::runtime_error(
                    "LOBSTER removal exceeds remaining quantity "
                    "at source row " +
                    std::to_string(sourceRow) +
                    ", order ID " +
                    std::to_string(orderId)
                );
}

            if (
                removedQuantity ==
                existingOrder.quantity
            ) {
                convertedEvents.push_back(
                    makeCancelEvent(
                        nextSequence++,
                        timestamp,
                        orderId,
                        UnknownParticipant,
                        DefaultSymbol
                    )
                );

                activeOrders.erase(iterator);
                return;
            }

            Quantity remainingQuantity =
                existingOrder.quantity -
                removedQuantity;

            convertedEvents.push_back(
                makeModifyEvent(
                    nextSequence++,
                    timestamp,
                    orderId,
                    existingOrder.price,
                    remainingQuantity,
                    UnknownParticipant,
                    DefaultSymbol
                )
            );

            existingOrder.quantity =
                remainingQuantity;
        };

    while (std::getline(input, line)) {
        ++sourceRow;

        if (line.empty()) {
            continue;
        }

        std::stringstream rowStream{line};

        std::array<std::string, 6> columns;

        for (
            std::size_t column = 0;
            column < columns.size();
            ++column
        ) {
            if (!std::getline(
                rowStream,
                columns[column],
                ','
            )) {
                throw std::runtime_error(
                    "Malformed LOBSTER row " +
                    std::to_string(sourceRow)
                );
            }
        }

        long double timeSeconds =
            std::stold(columns[0]);

        int lobsterEventType =
            std::stoi(columns[1]);

        OrderId orderId =
            static_cast<OrderId>(
                std::stoull(columns[2])
            );

        std::uint64_t rawSize =
            std::stoull(columns[3]);

        Price rawPrice =
            static_cast<Price>(
                std::stoll(columns[4])
            );

        int direction =
            std::stoi(columns[5]);

        if (
            rawSize >
            std::numeric_limits<Quantity>::max()
        ) {
            throw std::runtime_error(
                "LOBSTER quantity is too large at row " +
                std::to_string(sourceRow)
            );
        }

        Quantity quantity =
            static_cast<Quantity>(rawSize);

        TimestampNs timestamp =
            static_cast<TimestampNs>(
                std::llround(
                    timeSeconds *
                    1'000'000'000.0L
                )
            );

        Side side;

        if (direction == 1) {
            side = Side::Buy;
        } else if (direction == -1) {
            side = Side::Sell;
        } else {
            /*
                Control events such as trading halts may
                not contain a normal buy/sell direction.
            */
            ++ignoredControlEvents;
            continue;
        }

        switch (lobsterEventType) {
            /*
                1 = submission of a new limit order.
            */
            case 1: {
                if (activeOrders.contains(orderId)) {
                    throw std::runtime_error(
                        "Duplicate LOBSTER order ID at row " +
                        std::to_string(sourceRow)
                    );
                }

                Order order{
                    orderId,
                    side,
                    rawPrice,
                    quantity,
                    UnknownParticipant,
                    DefaultSymbol
                };

                convertedEvents.push_back(
                    makeAddEvent(
                        nextSequence++,
                        timestamp,
                        order
                    )
                );

                activeOrders.emplace(
                    orderId,
                    LobsterActiveOrder{
                        side,
                        rawPrice,
                        quantity
                    }
                );

                break;
            }

            /*
                2 = partial cancellation.

                The Size column is the number of shares
                removed, not the new total quantity.
            */
            case 2: {
                reduceExistingOrder(
                    orderId,
                    quantity,
                    timestamp
                );

                break;
            }

            /*
                3 = complete deletion.
            */
            case 3: {
                auto iterator =
    activeOrders.find(orderId);

                if (iterator == activeOrders.end()) {
                    ++skippedMissingOrders;

                    std::cerr
                        << "LOBSTER warning: row "
                        << sourceRow
                        << " references missing order "
                        << orderId
                        << " during complete deletion.\n";

                    break;
                }

                convertedEvents.push_back(
                    makeCancelEvent(
                        nextSequence++,
                        timestamp,
                        orderId,
                        UnknownParticipant,
                        DefaultSymbol
                    )
                );

                activeOrders.erase(iterator);
                break;
            }

            /*
                4 = visible execution.

                For initial order-book reconstruction,
                reduce the quantity of the resting order.

                This keeps book state correct but does not yet
                generate a Trade/P&L entry.
            */
            case 4: {
                reduceExistingOrder(
                    orderId,
                    quantity,
                    timestamp
                );

                break;
            }

            /*
                5 = hidden execution.

                It does not remove a visible resting order from
                the displayed book, so skip it for the initial
                reconstruction.
            */
            case 5: {
                ++hiddenExecutions;
                break;
            }

            /*
                6 = cross trade
                7 = trading halt/control event
            */
            case 6:
            case 7: {
                ++ignoredControlEvents;
                break;
            }

            default: {
                ++ignoredControlEvents;
                break;
            }
        }
    }

    std::cout
        << "\nLOBSTER conversion completed\n"
        << "----------------------------\n"
        << "Source rows: "
        << sourceRow
        << '\n'
        << "Converted events: "
        << convertedEvents.size()
        << '\n'
        << "Orders still active: "
        << activeOrders.size()
        << '\n'
        << "Missing order references skipped: "
        << skippedMissingOrders
        << '\n'
        << "Hidden executions skipped: "
        << hiddenExecutions
        << '\n'
        << "Control events ignored: "
        << ignoredControlEvents
        << '\n';

    if (skippedMissingOrders > 0) {
        std::cerr
            << "\nWARNING: "
            << skippedMissingOrders
            << " LOBSTER rows referenced orders that were "
               "not present in the reconstructed local book.\n"
            << "The replay may still be useful for event-flow "
               "analysis, but the resulting book must not be "
               "described as a complete L3 reconstruction.\n";
    }

    return convertedEvents;
}

bool validateMarketEvent(
    const MarketEvent& event,
    std::string& reason
) {
    if (event.sequence_number == 0) {
        reason = "sequence number cannot be zero";
        return false;
    }

    if (event.order_id == 0) {
        reason = "order ID cannot be zero";
        return false;
    }
    if (event.symbol_id != DefaultSymbol) {
        reason =
            "this order book supports only DefaultSymbol";

        return false;
    }

    switch (event.type) {
        case EventType::Add:
        case EventType::Execute:
            if (event.price <= 0) {
                reason = "add/execute price must be positive";
                return false;
            }

            if (event.quantity == 0) {
                reason = "add/execute quantity must be positive";
                return false;
            }

            return true;

        case EventType::Modify:
            if (event.price <= 0) {
                reason = "modify price must be positive";
                return false;
            }

            if (event.quantity == 0) {
                reason = "modify quantity must be positive";
                return false;
            }

            return true;

        case EventType::Cancel:
            return true;
    }

    reason = "unknown event type";
    return false;
}

// ============================================================
// BINARY FORMAT
// ============================================================

namespace BinaryFormat {

    constexpr std::array<std::uint8_t, 8> FileMagic{
        'O', 'B', 'R', 'E', 'P', 'L', 'A', 'Y'
    };

    constexpr std::uint16_t FileVersion = 2;
    constexpr std::uint16_t FileHeaderSize = 20;

    constexpr std::uint32_t RecordMagic =
        0x544E5645;

    constexpr std::uint8_t RecordVersion = 2;

    constexpr std::uint16_t RecordSize = 64;
    constexpr std::size_t RecordPayloadSize = 60;

} // namespace BinaryFormat
struct UdpDatagram {
    std::array<
        std::uint8_t,
        BinaryFormat::RecordSize
    > bytes{};

    std::size_t size{0};

    std::uint64_t received_timestamp_ns{0};
};

using NetworkQueue =
    SpscRingBuffer<
        UdpDatagram,
        4096
    >;

std::uint64_t steadyTimestampNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<
            std::chrono::nanoseconds
        >(
            std::chrono::steady_clock::
                now().time_since_epoch()
        ).count()
    );
}

template<typename T>
void appendLittleEndian(
    std::vector<std::uint8_t>& buffer,
    T value
) {
    static_assert(std::is_integral_v<T>);

    using UnsignedType = std::make_unsigned_t<T>;

    UnsignedType bits = 0;
    std::memcpy(&bits, &value, sizeof(T));

    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
        buffer.push_back(
            static_cast<std::uint8_t>(
                (bits >> (byte * 8)) & 0xFF
            )
        );
    }
}

template<typename T>
bool readLittleEndian(
    const std::vector<std::uint8_t>& buffer,
    std::size_t& offset,
    T& output
) {
    static_assert(std::is_integral_v<T>);

    if (offset + sizeof(T) > buffer.size()) {
        return false;
    }

    using UnsignedType = std::make_unsigned_t<T>;

    UnsignedType bits = 0;

    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
        bits |= (
            static_cast<UnsignedType>(buffer[offset + byte])
            << (byte * 8)
        );
    }

    std::memcpy(&output, &bits, sizeof(T));
    offset += sizeof(T);

    return true;
}

std::uint32_t calculateChecksum(
    const std::vector<std::uint8_t>& bytes,
    std::size_t count
) {
    constexpr std::uint32_t FnvOffsetBasis = 2166136261u;
    constexpr std::uint32_t FnvPrime = 16777619u;

    std::uint32_t hash = FnvOffsetBasis;

    for (
        std::size_t index = 0;
        index < count;
        ++index
    ) {
        hash ^= bytes[index];
        hash *= FnvPrime;
    }

    return hash;
}

std::vector<std::uint8_t> serialiseEvent(
    const MarketEvent& event
) {
    std::vector<std::uint8_t> record;
    record.reserve(BinaryFormat::RecordSize);

    appendLittleEndian(
        record,
        BinaryFormat::RecordMagic
    );

    appendLittleEndian(
        record,
        BinaryFormat::RecordSize
    );

    appendLittleEndian(
        record,
        BinaryFormat::RecordVersion
    );

    appendLittleEndian(
        record,
        static_cast<std::uint8_t>(event.type)
    );

    appendLittleEndian(
        record,
        event.sequence_number
    );

    appendLittleEndian(
        record,
        event.timestamp_ns
    );

    appendLittleEndian(
        record,
        event.order_id
    );

    appendLittleEndian(
        record,
        event.price
    );

    appendLittleEndian(
        record,
        event.quantity
    );

    appendLittleEndian(
        record,
        static_cast<std::uint8_t>(event.side)
    );
    appendLittleEndian(
    record,
    event.participant_id
);

    appendLittleEndian(
        record,
        event.symbol_id
    );

    // Reserved bytes for future format extensions.
    appendLittleEndian(record, std::uint8_t{0});
    appendLittleEndian(record, std::uint8_t{0});
    appendLittleEndian(record, std::uint8_t{0});

    std::uint32_t checksum = calculateChecksum(
        record,
        record.size()
    );

    appendLittleEndian(record, checksum);

    return record;
}

UdpDatagram makeUdpDatagram(
    const MarketEvent& event
) {
    std::vector<std::uint8_t> record =
        serialiseEvent(event);

    if (
        record.size() !=
        BinaryFormat::RecordSize
    ) {
        throw std::runtime_error(
            "Unable to create UDP datagram"
        );
    }

    UdpDatagram datagram;

    std::copy(
        record.begin(),
        record.end(),
        datagram.bytes.begin()
    );

    datagram.size = record.size();

    datagram.received_timestamp_ns =
        steadyTimestampNs();

    return datagram;
}



// ============================================================
// BINARY WRITER
// ============================================================

class BinaryEventWriter {
public:
    static bool write(
        const std::filesystem::path& filePath,
        const std::vector<MarketEvent>& events,
        std::string& error
    ) {
        for (
            std::size_t index = 0;
            index < events.size();
            ++index
        ) {
            std::string validationReason;

            if (!validateMarketEvent(
                events[index],
                validationReason
            )) {
                error =
                    "Invalid event at index " +
                    std::to_string(index) +
                    ": " +
                    validationReason;

                return false;
            }
        }

        std::ofstream output(
            filePath,
            std::ios::binary | std::ios::trunc
        );

        if (!output.is_open()) {
            error =
                "Unable to open binary output file: " +
                filePath.string();

            return false;
        }

        output.write(
            reinterpret_cast<const char*>(
                BinaryFormat::FileMagic.data()
            ),
            static_cast<std::streamsize>(
                BinaryFormat::FileMagic.size()
            )
        );

        std::vector<std::uint8_t> header;

        appendLittleEndian(
            header,
            BinaryFormat::FileVersion
        );

        appendLittleEndian(
            header,
            BinaryFormat::FileHeaderSize
        );

        appendLittleEndian(
            header,
            static_cast<std::uint64_t>(events.size())
        );

        output.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size())
        );

        for (const MarketEvent& event : events) {
            std::vector<std::uint8_t> record =
                serialiseEvent(event);

            if (record.size() != BinaryFormat::RecordSize) {
                error = "Internal binary record-size error";
                return false;
            }

            output.write(
                reinterpret_cast<const char*>(record.data()),
                static_cast<std::streamsize>(record.size())
            );

            if (!output.good()) {
                error = "Failed while writing binary event";
                return false;
            }
        }

        output.flush();

        if (!output.good()) {
            error = "Failed to flush binary output file";
            return false;
        }

        return true;
    }
};

// ============================================================
// BINARY PARSER
// ============================================================

struct ParseStatistics {
    std::uint64_t records_declared{0};
    std::uint64_t records_read{0};
    std::uint64_t valid_records{0};
    std::uint64_t corrupt_records{0};
    std::uint64_t truncated_records{0};
};

struct ParseResult {
    bool success{false};
    std::vector<MarketEvent> events;
    std::vector<std::string> errors;
    ParseStatistics statistics;
};

class BinaryEventParser {
public:
    static ParseResult parse(
        const std::filesystem::path& filePath
    ) {
        ParseResult result;

        std::ifstream input(
            filePath,
            std::ios::binary
        );

        if (!input.is_open()) {
            result.errors.push_back(
                "Unable to open binary input file: " +
                filePath.string()
            );

            return result;
        }

        std::array<std::uint8_t, 8> magic{};

        input.read(
            reinterpret_cast<char*>(magic.data()),
            static_cast<std::streamsize>(magic.size())
        );

        if (
            input.gcount() !=
            static_cast<std::streamsize>(magic.size())
        ) {
            result.errors.push_back(
                "File is too small to contain a valid header"
            );

            return result;
        }

        if (magic != BinaryFormat::FileMagic) {
            result.errors.push_back(
                "Invalid binary file magic"
            );

            return result;
        }

        constexpr std::size_t RemainingHeaderSize =
            BinaryFormat::FileHeaderSize -
            BinaryFormat::FileMagic.size();

        std::vector<std::uint8_t> header(
            RemainingHeaderSize
        );

        input.read(
            reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size())
        );

        if (
            input.gcount() !=
            static_cast<std::streamsize>(header.size())
        ) {
            result.errors.push_back(
                "Truncated binary file header"
            );

            return result;
        }

        std::size_t headerOffset = 0;

        std::uint16_t fileVersion = 0;
        std::uint16_t headerSize = 0;
        std::uint64_t recordCount = 0;

        if (
            !readLittleEndian(
                header,
                headerOffset,
                fileVersion
            ) ||
            !readLittleEndian(
                header,
                headerOffset,
                headerSize
            ) ||
            !readLittleEndian(
                header,
                headerOffset,
                recordCount
            )
        ) {
            result.errors.push_back(
                "Unable to decode binary file header"
            );

            return result;
        }

        if (fileVersion != BinaryFormat::FileVersion) {
            result.errors.push_back(
                "Unsupported binary file version: " +
                std::to_string(fileVersion)
            );

            return result;
        }

        if (headerSize != BinaryFormat::FileHeaderSize) {
            result.errors.push_back(
                "Unexpected binary header size"
            );

            return result;
        }

        result.statistics.records_declared = recordCount;
        result.events.reserve(
            static_cast<std::size_t>(recordCount)
        );

        for (
            std::uint64_t recordIndex = 0;
            recordIndex < recordCount;
            ++recordIndex
        ) {
            std::vector<std::uint8_t> record(
                BinaryFormat::RecordSize
            );

            input.read(
                reinterpret_cast<char*>(record.data()),
                static_cast<std::streamsize>(record.size())
            );

            if (
                input.gcount() !=
                static_cast<std::streamsize>(record.size())
            ) {
                ++result.statistics.truncated_records;

                result.errors.push_back(
                    "Truncated record at index " +
                    std::to_string(recordIndex)
                );

                break;
            }

            ++result.statistics.records_read;

            std::uint32_t storedChecksum = 0;
            std::size_t checksumOffset =
                BinaryFormat::RecordPayloadSize;

            if (!readLittleEndian(
                record,
                checksumOffset,
                storedChecksum
            )) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Unable to decode checksum at record " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            std::uint32_t calculatedChecksum =
                calculateChecksum(
                    record,
                    BinaryFormat::RecordPayloadSize
                );

            if (storedChecksum != calculatedChecksum) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Checksum mismatch at record " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            std::size_t offset = 0;

            std::uint32_t recordMagic = 0;
            std::uint16_t recordSize = 0;
            std::uint8_t recordVersion = 0;
            std::uint8_t eventTypeValue = 0;
            std::uint64_t sequenceNumber = 0;
            std::uint64_t timestampNs = 0;
            OrderId orderId = 0;
            Price price = 0;
            Quantity quantity = 0;
            std::uint8_t sideValue = 0;

            ParticipantId participantId =
    UnknownParticipant;

            SymbolId symbolId =
                DefaultSymbol;

            std::uint8_t reserved0 = 0;
            std::uint8_t reserved1 = 0;
            std::uint8_t reserved2 = 0;

            bool decoded =
                readLittleEndian(
                    record,
                    offset,
                    recordMagic
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    recordSize
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    recordVersion
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    eventTypeValue
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    sequenceNumber
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    timestampNs
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    orderId
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    price
                ) &&
                    readLittleEndian(
        record,
        offset,
        quantity
    ) &&
    readLittleEndian(
        record,
        offset,
        sideValue
    ) &&
    readLittleEndian(
        record,
        offset,
        participantId
    ) &&
    readLittleEndian(
        record,
        offset,
        symbolId
    ) &&
    readLittleEndian(
        record,
        offset,
        reserved0
    ) &&
    readLittleEndian(
        record,
        offset,
        reserved1
    ) &&
    readLittleEndian(
        record,
        offset,
        reserved2
    );

            if (!decoded) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Unable to decode record " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (recordMagic != BinaryFormat::RecordMagic) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid record magic at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (recordSize != BinaryFormat::RecordSize) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid record size at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (
                recordVersion !=
                BinaryFormat::RecordVersion
            ) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Unsupported record version at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (
                eventTypeValue <
                    static_cast<std::uint8_t>(
                        EventType::Add
                    ) ||
                eventTypeValue >
                    static_cast<std::uint8_t>(
                        EventType::Execute
                    )
            ) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid event type at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (sideValue > 1) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid side value at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            MarketEvent event{
                sequenceNumber,
                timestampNs,
                static_cast<EventType>(eventTypeValue),
                orderId,
                static_cast<Side>(sideValue),
                price,
                quantity,
                participantId,
                symbolId
            };

            std::string validationReason;

            if (!validateMarketEvent(
                event,
                validationReason
            )) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid event at record " +
                    std::to_string(recordIndex) +
                    ": " +
                    validationReason
                );

                continue;
            }

            result.events.push_back(event);
            ++result.statistics.valid_records;
        }

        result.success = result.errors.empty();
        return result;
    }
};

// ============================================================
// REPLAY ENGINE
// ============================================================

struct ReplayOptions {
    bool reset_book_before_replay{true};

    bool stop_on_rejection{false};

    std::uint64_t expected_first_sequence{0};
};

struct ReplayStatistics {
    std::uint64_t total_events{0};
    std::uint64_t applied_events{0};
    std::uint64_t rejected_events{0};

    std::uint64_t add_events{0};
    std::uint64_t cancel_events{0};
    std::uint64_t modify_events{0};
    std::uint64_t execute_events{0};

    std::uint64_t trades_generated{0};
    std::uint64_t executed_quantity{0};

    std::uint64_t sequence_gap_events{0};
    std::uint64_t missing_sequences{0};
    std::uint64_t duplicate_or_out_of_order_sequences{0};

    std::uint64_t first_sequence{0};
    std::uint64_t last_sequence{0};
};

struct ReplayResult {
    bool completed{true};
    ReplayStatistics statistics;
    std::vector<std::string> errors;
};

struct ApplyEventResult {
    bool accepted{false};
    std::uint64_t trades_generated{0};
    std::uint64_t executed_quantity{0};
    std::string error;
};

ApplyEventResult applyMarketEvent(
    const MarketEvent& event
) {
    ApplyEventResult result;

    std::string validationReason;

    if (!validateMarketEvent(
        event,
        validationReason
    )) {
        result.error = validationReason;
        return result;
    }

    switch (event.type) {
        case EventType::Add: {
            result.accepted =
                addOrder(
                    Order{
                        event.order_id,
                        event.side,
                        event.price,
                        event.quantity,
                        event.participant_id,
                        event.symbol_id
                    }
                );

            break;
        }

        case EventType::Cancel: {
            result.accepted =
                cancelOrder(event.order_id);

            break;
        }

        case EventType::Modify: {
            result.accepted =
                modifyOrder(
                    event.order_id,
                    event.price,
                    event.quantity
                );

            break;
        }

        case EventType::Execute: {
            ExecutionResult execution =
                executeOrder(
                    Order{
                        event.order_id,
                        event.side,
                        event.price,
                        event.quantity,
                        event.participant_id,
                        event.symbol_id
                    }
                );

            result.accepted =
                execution.accepted;

            result.trades_generated =
                execution.trades.size();

            result.executed_quantity =
                execution.executed_quantity;

            break;
        }
    }

    if (!result.accepted) {
        result.error =
            "order-book operation rejected event";
    }

    return result;
}

class ReplayEngine {
public:
    ReplayResult replay(
        const std::vector<MarketEvent>& events,
        ReplayOptions options = {}
    ) {
        ReplayResult result;

        if (options.reset_book_before_replay) {
            resetOrderBook();
        }

        std::uint64_t expectedSequence =
            options.expected_first_sequence;

        bool expectedSequenceInitialised =
            expectedSequence != 0;

        for (
            std::size_t index = 0;
            index < events.size();
            ++index
        ) {
            const MarketEvent& event = events[index];

            ++result.statistics.total_events;

            if (result.statistics.first_sequence == 0) {
                result.statistics.first_sequence =
                    event.sequence_number;
            }

            result.statistics.last_sequence =
                event.sequence_number;

            switch (event.type) {
                case EventType::Add:
                    ++result.statistics.add_events;
                    break;

                case EventType::Cancel:
                    ++result.statistics.cancel_events;
                    break;

                case EventType::Modify:
                    ++result.statistics.modify_events;
                    break;

                case EventType::Execute:
                    ++result.statistics.execute_events;
                    break;
            }

            if (!expectedSequenceInitialised) {
                expectedSequence = event.sequence_number;
                expectedSequenceInitialised = true;
            }

            if (event.sequence_number != expectedSequence) {
                if (event.sequence_number > expectedSequence) {
                    ++result.statistics.sequence_gap_events;

                    result.statistics.missing_sequences +=
                        event.sequence_number -
                        expectedSequence;

                    result.errors.push_back(
                        "Sequence gap before event index " +
                        std::to_string(index) +
                        ": expected " +
                        std::to_string(expectedSequence) +
                        ", received " +
                        std::to_string(
                            event.sequence_number
                        )
                    );
                } else {
                    ++result.statistics
                        .duplicate_or_out_of_order_sequences;

                    result.errors.push_back(
                        "Duplicate/out-of-order sequence at "
                        "event index " +
                        std::to_string(index) +
                        ": expected " +
                        std::to_string(expectedSequence) +
                        ", received " +
                        std::to_string(
                            event.sequence_number
                        )
                    );
                }

                /*
    Offline replay cannot safely continue after an
    unresolved sequence error.
*/
                result.completed = false;
                break;
            }

            std::string validationReason;

            if (!validateMarketEvent(
                event,
                validationReason
            )) {
                ++result.statistics.rejected_events;

                result.errors.push_back(
                    "Rejected event index " +
                    std::to_string(index) +
                    ": " +
                    validationReason
                );

                if (options.stop_on_rejection) {
                    result.completed = false;
                    break;
                }

                updateExpectedSequence(
                    event.sequence_number,
                    expectedSequence
                );

                continue;
            }

            bool applied = false;

            switch (event.type) {
                case EventType::Add: {
                    applied = addOrder(
                    Order{
event.order_id,
event.side,
event.price,
event.quantity,
event.participant_id,
event.symbol_id
}
                    );

                    break;
                }

                case EventType::Cancel: {
                    applied = cancelOrder(
                        event.order_id
                    );

                    break;
                }

                case EventType::Modify: {
                    applied = modifyOrder(
                        event.order_id,
                        event.price,
                        event.quantity
                    );

                    break;
                }

                case EventType::Execute: {
                    ExecutionResult execution =
                        executeOrder(
                        Order{
event.order_id,
event.side,
event.price,
event.quantity,
event.participant_id,
event.symbol_id
}
                        );

                    applied = execution.accepted;

                    result.statistics.trades_generated +=
                        execution.trades.size();

                    result.statistics.executed_quantity +=
                        execution.executed_quantity;

                    break;
                }
            }

            if (applied) {
                ++result.statistics.applied_events;
            } else {
                ++result.statistics.rejected_events;

                result.errors.push_back(
                    "Order-book operation rejected event "
                    "index " +
                    std::to_string(index) +
                    ", sequence " +
                    std::to_string(event.sequence_number)
                );

                if (options.stop_on_rejection) {
                    result.completed = false;
                    break;
                }
            }

            updateExpectedSequence(
                event.sequence_number,
                expectedSequence
            );
        }

        return result;
    }

private:
    static void updateExpectedSequence(
        std::uint64_t receivedSequence,
        std::uint64_t& expectedSequence
    ) {
        if (
            receivedSequence >= expectedSequence &&
            receivedSequence !=
                std::numeric_limits<std::uint64_t>::max()
        ) {
            expectedSequence = receivedSequence + 1;
        }
    }
};

// ============================================================
// REPLAY REPORTING
// ============================================================

void printParseStatistics(
    const ParseResult& result
) {
    std::cout << "\nBinary parse statistics\n";
    std::cout << "-----------------------\n";

    std::cout
        << "Records declared: "
        << result.statistics.records_declared
        << '\n';

    std::cout
        << "Records read: "
        << result.statistics.records_read
        << '\n';

    std::cout
        << "Valid records: "
        << result.statistics.valid_records
        << '\n';

    std::cout
        << "Corrupt records: "
        << result.statistics.corrupt_records
        << '\n';

    std::cout
        << "Truncated records: "
        << result.statistics.truncated_records
        << '\n';

    if (!result.errors.empty()) {
        std::cout << "Errors:\n";

        for (const std::string& error : result.errors) {
            std::cout << "  - " << error << '\n';
        }
    }
}

void printReplayStatistics(
    const ReplayResult& result
) {
    const ReplayStatistics& stats = result.statistics;

    std::cout << "\nReplay statistics\n";
    std::cout << "-----------------\n";

    std::cout
        << "Completed: "
        << std::boolalpha
        << result.completed
        << '\n';

    std::cout
        << "Total events: "
        << stats.total_events
        << '\n';

    std::cout
        << "Applied events: "
        << stats.applied_events
        << '\n';

    std::cout
        << "Rejected events: "
        << stats.rejected_events
        << '\n';

    std::cout
        << "Add events: "
        << stats.add_events
        << '\n';

    std::cout
        << "Cancel events: "
        << stats.cancel_events
        << '\n';

    std::cout
        << "Modify events: "
        << stats.modify_events
        << '\n';

    std::cout
        << "Execute events: "
        << stats.execute_events
        << '\n';

    std::cout
        << "Trades generated: "
        << stats.trades_generated
        << '\n';

    std::cout
        << "Executed quantity: "
        << stats.executed_quantity
        << '\n';

    std::cout
        << "Sequence gaps: "
        << stats.sequence_gap_events
        << '\n';

    std::cout
        << "Missing sequences: "
        << stats.missing_sequences
        << '\n';

    std::cout
        << "Duplicate/out-of-order sequences: "
        << stats.duplicate_or_out_of_order_sequences
        << '\n';

    if (!result.errors.empty()) {
        std::cout << "Replay errors:\n";

        for (const std::string& error : result.errors) {
            std::cout << "  - " << error << '\n';
        }
    }
}

// Used to prove replay determinism in tests.
std::string createOrderBookSnapshot() {
    std::ostringstream output;

    output << "BIDS|";

    for (const auto& [price, level] : bids) {
        output
            << price
            << ':'
            << level.total_quantity
            << '[';

        for (const Order& order : level.orders) {
            output
    << order.id
    << ','
    << order.quantity
    << ','
    << order.participant_id
    << ','
    << order.symbol_id
    << ';';
        }

        output << ']';
    }

    output << "|ASKS|";

    for (const auto& [price, level] : asks) {
        output
            << price
            << ':'
            << level.total_quantity
            << '[';

        for (const Order& order : level.orders) {
            output
    << order.id
    << ','
    << order.quantity
    << ','
    << order.participant_id
    << ','
    << order.symbol_id
    << ';';
        }

        output << ']';
    }

    output << "|TRADES|";

    for (const Trade& trade : tradeHistory) {
        output
    << trade.maker_order_id
    << ','
    << trade.taker_order_id
    << ','
    << trade.price
    << ','
    << trade.quantity
    << ','
    << static_cast<int>(
           trade.aggressor_side
       )
    << ','
    << trade.maker_participant_id
    << ','
    << trade.taker_participant_id
    << ','
    << trade.symbol_id
    << ';';
    }

    return output.str();
}

// ============================================================
// STRATEGY AND RISK TYPES
// ============================================================

using Position = std::int64_t;

Side oppositeSide(Side side) {
    return side == Side::Buy
        ? Side::Sell
        : Side::Buy;
}

std::string sideToString(Side side) {
    return side == Side::Buy
        ? "BUY"
        : "SELL";
}

enum class RejectionReason {
    None,
    KillSwitchActive,
    MarketUnavailable,
    StaleMarket,
    InvalidTimestamp,
    CrossedMarket,
    InvalidPrice,
    InvalidQuantity,
    MaximumOrderSizeExceeded,
    MaximumPositionExceeded,
    MaximumNotionalExceeded,
    QuoteWouldCross,
    DuplicateOrderId,
    OrderBookRejected
};

std::string rejectionReasonToString(
    RejectionReason reason
) {
    switch (reason) {
        case RejectionReason::None:
            return "none";

        case RejectionReason::KillSwitchActive:
            return "kill switch active";

        case RejectionReason::MarketUnavailable:
            return "market unavailable";

        case RejectionReason::StaleMarket:
            return "stale market";

        case RejectionReason::InvalidTimestamp:
            return "invalid timestamp";

        case RejectionReason::CrossedMarket:
            return "crossed or locked market";

        case RejectionReason::InvalidPrice:
            return "invalid price";

        case RejectionReason::InvalidQuantity:
            return "invalid quantity";

        case RejectionReason::MaximumOrderSizeExceeded:
            return "maximum order size exceeded";

        case RejectionReason::MaximumPositionExceeded:
            return "maximum position exceeded";

        case RejectionReason::MaximumNotionalExceeded:
            return "maximum notional exposure exceeded";

        case RejectionReason::QuoteWouldCross:
            return "generated quotes would cross";

        case RejectionReason::DuplicateOrderId:
            return "duplicate order ID";

        case RejectionReason::OrderBookRejected:
            return "order book rejected order";
    }

    return "unknown";
}

// ============================================================
// REJECTION REPORTING
// ============================================================

struct RejectionRecord {
    TimestampNs timestamp_ns;
    RejectionReason reason;
    std::optional<Order> order;
    std::string details;
};

class RejectionReporter {
public:
    void recordOrderRejection(
        TimestampNs timestampNs,
        const Order& order,
        RejectionReason reason,
        const std::string& details
    ) {
        records_.push_back(
            RejectionRecord{
                timestampNs,
                reason,
                order,
                details
            }
        );
    }

    void recordMarketRejection(
        TimestampNs timestampNs,
        RejectionReason reason,
        const std::string& details
    ) {
        records_.push_back(
            RejectionRecord{
                timestampNs,
                reason,
                std::nullopt,
                details
            }
        );
    }

    const std::vector<RejectionRecord>& records() const {
        return records_;
    }

    std::size_t count() const {
        return records_.size();
    }

    bool empty() const {
        return records_.empty();
    }

    void clear() {
        records_.clear();
    }

    void print() const {
        std::cout << "\nRejected-order report\n";
        std::cout << "---------------------\n";

        if (records_.empty()) {
            std::cout << "No rejected actions.\n";
            return;
        }

        for (const RejectionRecord& record : records_) {
            std::cout
                << "Timestamp: "
                << record.timestamp_ns
                << " | Reason: "
                << rejectionReasonToString(record.reason);

            if (record.order.has_value()) {
                const Order& order = record.order.value();

                std::cout
                    << " | Order ID: "
                    << order.id
                    << " | Side: "
                    << sideToString(order.side)
                    << " | Price: "
                    << order.price
                    << " | Quantity: "
                    << order.quantity;
            }

            if (!record.details.empty()) {
                std::cout
                    << " | Details: "
                    << record.details;
            }

            std::cout << '\n';
        }
    }

private:
    std::vector<RejectionRecord> records_;
};


// ============================================================
// INVENTORY TRACKING
// ============================================================

struct InventoryFill {
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;
    bool was_maker;
};

class InventoryTracker {
public:
    void processNewTrades(
        const std::vector<Trade>& trades,
        const std::unordered_map<OrderId, Side>& ownedOrders
    ) {
        /*
            tradeHistory may have been cleared by resetOrderBook().
            Reset the cursor if the new history is shorter.
        */
        if (processed_trade_count_ > trades.size()) {
            processed_trade_count_ = 0;
        }

        while (processed_trade_count_ < trades.size()) {
            const Trade& trade =
                trades[processed_trade_count_];

            auto makerIterator =
                ownedOrders.find(trade.maker_order_id);

            if (makerIterator != ownedOrders.end()) {
                Side makerSide =
                    oppositeSide(trade.aggressor_side);

                applyFill(
                    trade.maker_order_id,
                    makerSide,
                    trade.price,
                    trade.quantity,
                    true
                );
            }

            auto takerIterator =
                ownedOrders.find(trade.taker_order_id);

            if (takerIterator != ownedOrders.end()) {
                Side takerSide =
                    trade.aggressor_side;

                applyFill(
                    trade.taker_order_id,
                    takerSide,
                    trade.price,
                    trade.quantity,
                    false
                );
            }

            ++processed_trade_count_;
        }
    }

    Position position() const {
        return position_;
    }

    std::uint64_t totalBought() const {
        return total_bought_;
    }

    std::uint64_t totalSold() const {
        return total_sold_;
    }

    const std::vector<InventoryFill>& fills() const {
        return fills_;
    }

    void reset() {
        position_ = 0;
        total_bought_ = 0;
        total_sold_ = 0;
        processed_trade_count_ = 0;
        fills_.clear();
    }

private:
    void applyFill(
        OrderId orderId,
        Side side,
        Price price,
        Quantity quantity,
        bool wasMaker
    ) {
        Position signedQuantity =
            static_cast<Position>(quantity);

        if (side == Side::Buy) {
            position_ += signedQuantity;
            total_bought_ += quantity;
        } else {
            position_ -= signedQuantity;
            total_sold_ += quantity;
        }

        fills_.push_back(
            InventoryFill{
                orderId,
                side,
                price,
                quantity,
                wasMaker
            }
        );
    }

    Position position_{0};

    std::uint64_t total_bought_{0};
    std::uint64_t total_sold_{0};

    std::size_t processed_trade_count_{0};

    std::vector<InventoryFill> fills_;
};


// ============================================================
// RISK MANAGEMENT
// ============================================================

struct RiskLimits {
    Quantity maximum_order_size{100};

    Position maximum_absolute_position{1000};

    /*
        Uses the same units as Price × Quantity.

        For example, if price is stored in pence:
        10000 × 100 = 1,000,000 pence of exposure.
    */
    long double maximum_notional_exposure{
        10'000'000.0L
    };

    /*
        One second by default.
    */
    TimestampNs maximum_market_age_ns{
        1'000'000'000ULL
    };
};

struct RiskCheckResult {
    bool accepted;
    RejectionReason reason;
    std::string message;
};

RiskCheckResult acceptRiskCheck() {
    return RiskCheckResult{
        true,
        RejectionReason::None,
        ""
    };
}

RiskCheckResult rejectRiskCheck(
    RejectionReason reason,
    const std::string& message
) {
    return RiskCheckResult{
        false,
        reason,
        message
    };
}

class RiskManager {
public:
    explicit RiskManager(
        RiskLimits limits = {}
    )
        : limits_(limits) {
    }

    RiskCheckResult validateMarket(
        TimestampNs currentTimestamp,
        TimestampNs lastMarketUpdateTimestamp,
        std::optional<Price> currentBestBid,
        std::optional<Price> currentBestAsk
    ) const {
        if (kill_switch_active_) {
            return rejectRiskCheck(
                RejectionReason::KillSwitchActive,
                kill_switch_reason_
            );
        }

        if (
            !currentBestBid.has_value() ||
            !currentBestAsk.has_value()
        ) {
            return rejectRiskCheck(
                RejectionReason::MarketUnavailable,
                "Both a best bid and best ask are required"
            );
        }

        if (
            currentBestBid.value() >=
            currentBestAsk.value()
        ) {
            return rejectRiskCheck(
                RejectionReason::CrossedMarket,
                "Best bid must be below best ask"
            );
        }

        if (lastMarketUpdateTimestamp == 0) {
            return rejectRiskCheck(
                RejectionReason::MarketUnavailable,
                "No market update timestamp is available"
            );
        }

        if (
            currentTimestamp <
            lastMarketUpdateTimestamp
        ) {
            return rejectRiskCheck(
                RejectionReason::InvalidTimestamp,
                "Current timestamp is before the last market update"
            );
        }

        TimestampNs marketAge =
            currentTimestamp -
            lastMarketUpdateTimestamp;

        if (
            marketAge >
            limits_.maximum_market_age_ns
        ) {
            return rejectRiskCheck(
                RejectionReason::StaleMarket,
                "Market data age is " +
                std::to_string(marketAge) +
                " ns"
            );
        }

        return acceptRiskCheck();
    }

    RiskCheckResult validateOrder(
        const Order& order,
        Position currentPosition,
        TimestampNs currentTimestamp,
        TimestampNs lastMarketUpdateTimestamp,
        std::optional<Price> currentBestBid,
        std::optional<Price> currentBestAsk
    ) const {
        RiskCheckResult marketCheck =
            validateMarket(
                currentTimestamp,
                lastMarketUpdateTimestamp,
                currentBestBid,
                currentBestAsk
            );

        if (!marketCheck.accepted) {
            return marketCheck;
        }

        if (order.price <= 0) {
            return rejectRiskCheck(
                RejectionReason::InvalidPrice,
                "Order price must be positive"
            );
        }

        if (order.quantity == 0) {
            return rejectRiskCheck(
                RejectionReason::InvalidQuantity,
                "Order quantity must be positive"
            );
        }

        if (
            order.quantity >
            limits_.maximum_order_size
        ) {
            return rejectRiskCheck(
                RejectionReason::MaximumOrderSizeExceeded,
                "Quantity " +
                std::to_string(order.quantity) +
                " exceeds maximum " +
                std::to_string(
                    limits_.maximum_order_size
                )
            );
        }

        long double projectedPosition =
            static_cast<long double>(currentPosition);

        if (order.side == Side::Buy) {
            projectedPosition +=
                static_cast<long double>(
                    order.quantity
                );
        } else {
            projectedPosition -=
                static_cast<long double>(
                    order.quantity
                );
        }

        if (
            std::fabs(projectedPosition) >
            static_cast<long double>(
                limits_.maximum_absolute_position
            )
        ) {
            return rejectRiskCheck(
                RejectionReason::MaximumPositionExceeded,
                "Projected position would be " +
                std::to_string(
                    static_cast<long long>(
                        projectedPosition
                    )
                )
            );
        }

        long double projectedNotional =
            std::fabs(projectedPosition) *
            static_cast<long double>(order.price);

        if (
            projectedNotional >
            limits_.maximum_notional_exposure
        ) {
            return rejectRiskCheck(
                RejectionReason::MaximumNotionalExceeded,
                "Projected notional would be " +
                std::to_string(
                    static_cast<double>(
                        projectedNotional
                    )
                )
            );
        }

        if (orderIndex.contains(order.id)) {
            return rejectRiskCheck(
                RejectionReason::DuplicateOrderId,
                "Order ID already exists in the book"
            );
        }

        return acceptRiskCheck();
    }

    void activateKillSwitch(
        const std::string& reason
    ) {
        kill_switch_active_ = true;
        kill_switch_reason_ = reason;
    }

    void deactivateKillSwitch() {
        kill_switch_active_ = false;
        kill_switch_reason_.clear();
    }

    bool killSwitchActive() const {
        return kill_switch_active_;
    }

    const std::string& killSwitchReason() const {
        return kill_switch_reason_;
    }

    const RiskLimits& limits() const {
        return limits_;
    }

private:
    RiskLimits limits_;

    bool kill_switch_active_{false};
    std::string kill_switch_reason_;
};


// ============================================================
// MARKET-MAKING CONFIGURATION
// ============================================================

struct MarketMakerConfig {
    Quantity quote_quantity{10};
    Price half_spread_ticks{20};
    Price inventory_skew_per_unit{1};
    Position target_position{0};
    Price tick_size{1};

    OrderId first_strategy_order_id{
        1'000'000'000ULL
    };

    ParticipantId participant_id{5001};
    SymbolId symbol_id{DefaultSymbol};
};

struct QuoteLegResult {
    bool accepted{false};
    std::optional<OrderId> order_id;
    Price price{0};
    Quantity quantity{0};
};

struct QuoteRefreshResult {
    bool market_accepted{false};

    Price fair_price{0};
    Price inventory_skew{0};

    QuoteLegResult bid;
    QuoteLegResult ask;
};


// ============================================================
// MARKET-MAKING STRATEGY
// ============================================================

class MarketMakerStrategy {
public:
    MarketMakerStrategy(
        MarketMakerConfig config,
        RiskLimits riskLimits
    )
        : config_(config),
          risk_manager_(riskLimits),
          next_order_id_(
              config.first_strategy_order_id
          ) {
        if (config_.quote_quantity == 0) {
            throw std::invalid_argument(
                "Quote quantity must be positive"
            );
        }

        if (config_.half_spread_ticks <= 0) {
            throw std::invalid_argument(
                "Half spread must be positive"
            );
        }

        if (config_.tick_size <= 0) {
            throw std::invalid_argument(
                "Tick size must be positive"
            );
        }

        if (
            config_.inventory_skew_per_unit < 0
        ) {
            throw std::invalid_argument(
                "Inventory skew cannot be negative"
            );
        }
    }

    void onMarketUpdate(
        TimestampNs timestampNs
    ) {
        /*
            Do not move the last market-update timestamp
            backwards when replaying bad data.
        */
        if (
            timestampNs >
            last_market_update_ns_
        ) {
            last_market_update_ns_ = timestampNs;
        }
    }

    void processTrades() {
        inventory_tracker_.processNewTrades(
            tradeHistory,
            owned_order_sides_
        );

        /*
            A fully filled quote will no longer be present
            in orderIndex.
        */
        if (
            active_bid_order_id_.has_value() &&
            !containsOrder(
                active_bid_order_id_.value()
            )
        ) {
            active_bid_order_id_.reset();
        }

        if (
            active_ask_order_id_.has_value() &&
            !containsOrder(
                active_ask_order_id_.value()
            )
        ) {
            active_ask_order_id_.reset();
        }
    }

    QuoteRefreshResult refreshQuotes(
        TimestampNs currentTimestamp
    ) {
        processTrades();

        /*
            Remove old quotes before calculating new quotes.

            This prevents the strategy from using its own
            quotes as the market midpoint.
        */
        cancelActiveQuotes();

        std::optional<Price> marketBestBid =
            bestBid();

        std::optional<Price> marketBestAsk =
            bestAsk();

        QuoteRefreshResult refreshResult;

        RiskCheckResult marketCheck =
            risk_manager_.validateMarket(
                currentTimestamp,
                last_market_update_ns_,
                marketBestBid,
                marketBestAsk
            );

        if (!marketCheck.accepted) {
            rejection_reporter_.recordMarketRejection(
                currentTimestamp,
                marketCheck.reason,
                marketCheck.message
            );

            return refreshResult;
        }

        refreshResult.market_accepted = true;

        Price fairPrice = 0;
        Price inventorySkew = 0;
        Price bidPrice = 0;
        Price askPrice = 0;

        bool generated =
            generateQuotePrices(
                marketBestBid.value(),
                marketBestAsk.value(),
                fairPrice,
                inventorySkew,
                bidPrice,
                askPrice
            );

        if (!generated) {
            rejection_reporter_.recordMarketRejection(
                currentTimestamp,
                RejectionReason::QuoteWouldCross,
                "Unable to generate valid passive quotes"
            );

            refreshResult.market_accepted = false;
            return refreshResult;
        }

        refreshResult.fair_price = fairPrice;
        refreshResult.inventory_skew =
            inventorySkew;

        Order bidOrder{
            generateOrderId(),
            Side::Buy,
            bidPrice,
            config_.quote_quantity,
            config_.participant_id,
            config_.symbol_id
        };

        Order askOrder{
            generateOrderId(),
            Side::Sell,
            askPrice,
            config_.quote_quantity,
            config_.participant_id,
            config_.symbol_id
        };

        refreshResult.bid =
            submitQuote(
                bidOrder,
                currentTimestamp
            );

        refreshResult.ask =
            submitQuote(
                askOrder,
                currentTimestamp
            );

        return refreshResult;
    }

    void cancelActiveQuotes() {
        processTrades();

        cancelQuote(active_bid_order_id_);
        cancelQuote(active_ask_order_id_);
    }

    void activateKillSwitch(
        TimestampNs timestampNs,
        const std::string& reason
    ) {
        risk_manager_.activateKillSwitch(reason);

        cancelActiveQuotes();

        rejection_reporter_.recordMarketRejection(
            timestampNs,
            RejectionReason::KillSwitchActive,
            reason
        );
    }

    void deactivateKillSwitch() {
        risk_manager_.deactivateKillSwitch();
    }

    void resetStrategy() {
        cancelActiveQuotes();

        inventory_tracker_.reset();
        rejection_reporter_.clear();

        owned_order_sides_.clear();

        active_bid_order_id_.reset();
        active_ask_order_id_.reset();

        last_market_update_ns_ = 0;

        next_order_id_ =
            config_.first_strategy_order_id;

        risk_manager_.deactivateKillSwitch();
    }

    const InventoryTracker& inventory() const {
        return inventory_tracker_;
    }

    const RejectionReporter&
    rejectionReporter() const {
        return rejection_reporter_;
    }

    const RiskManager& riskManager() const {
        return risk_manager_;
    }

    std::optional<OrderId>
    activeBidOrderId() const {
        return active_bid_order_id_;
    }

    std::optional<OrderId>
    activeAskOrderId() const {
        return active_ask_order_id_;
    }

    TimestampNs lastMarketUpdate() const {
        return last_market_update_ns_;
    }

private:
    bool generateQuotePrices(
        Price marketBestBid,
        Price marketBestAsk,
        Price& fairPrice,
        Price& inventorySkew,
        Price& bidPrice,
        Price& askPrice
    ) const {
        if (
            marketBestBid <= 0 ||
            marketBestAsk <= 0 ||
            marketBestBid >= marketBestAsk
        ) {
            return false;
        }

        long double fairValue =
            (
                static_cast<long double>(
                    marketBestBid
                ) +
                static_cast<long double>(
                    marketBestAsk
                )
            ) /
            2.0L;

        Position inventoryDeviation =
            inventory_tracker_.position() -
            config_.target_position;

        long double skewValue =
            static_cast<long double>(
                inventoryDeviation
            ) *
            static_cast<long double>(
                config_.inventory_skew_per_unit
            );

        /*
            Positive inventory means we are too long.

            Therefore:
            adjusted fair value moves down,
            bid becomes less aggressive,
            ask becomes more aggressive.
        */
        long double adjustedFairValue =
            fairValue - skewValue;

        long double rawBid =
            adjustedFairValue -
            static_cast<long double>(
                config_.half_spread_ticks
            );

        long double rawAsk =
            adjustedFairValue +
            static_cast<long double>(
                config_.half_spread_ticks
            );

        /*
            Keep quotes passive.

            Buy quote must remain below best ask.
            Sell quote must remain above best bid.
        */
        long double maximumPassiveBid =
            static_cast<long double>(
                marketBestAsk
            ) -
            static_cast<long double>(
                config_.tick_size
            );

        long double minimumPassiveAsk =
            static_cast<long double>(
                marketBestBid
            ) +
            static_cast<long double>(
                config_.tick_size
            );

        rawBid = std::min(
            rawBid,
            maximumPassiveBid
        );

        rawAsk = std::max(
            rawAsk,
            minimumPassiveAsk
        );

        std::optional<Price> roundedBid =
            roundDownToTick(rawBid);

        std::optional<Price> roundedAsk =
            roundUpToTick(rawAsk);

        if (
            !roundedBid.has_value() ||
            !roundedAsk.has_value()
        ) {
            return false;
        }

        if (
            roundedBid.value() <= 0 ||
            roundedAsk.value() <= 0 ||
            roundedBid.value() >=
                roundedAsk.value()
        ) {
            return false;
        }

        std::optional<Price> convertedFair =
            convertToPrice(
                std::round(fairValue)
            );

        std::optional<Price> convertedSkew =
            convertSignedToPrice(
                std::round(skewValue)
            );

        if (
            !convertedFair.has_value() ||
            !convertedSkew.has_value()
        ) {
            return false;
        }

        fairPrice = convertedFair.value();
        inventorySkew =
            convertedSkew.value();

        bidPrice = roundedBid.value();
        askPrice = roundedAsk.value();

        return true;
    }

    QuoteLegResult submitQuote(
        const Order& order,
        TimestampNs timestampNs
    ) {
        QuoteLegResult result;

        result.order_id = order.id;
        result.price = order.price;
        result.quantity = order.quantity;

        RiskCheckResult riskCheck =
            risk_manager_.validateOrder(
                order,
                inventory_tracker_.position(),
                timestampNs,
                last_market_update_ns_,
                bestBid(),
                bestAsk()
            );

        if (!riskCheck.accepted) {
            rejection_reporter_.recordOrderRejection(
                timestampNs,
                order,
                riskCheck.reason,
                riskCheck.message
            );

            return result;
        }

        /*
            Register ownership before execution.

            This ensures an immediately executed strategy order
            can still be identified in tradeHistory.
        */
        owned_order_sides_[order.id] =
            order.side;

        ExecutionResult execution =
            executeOrder(order);

        if (!execution.accepted) {
            owned_order_sides_.erase(order.id);

            rejection_reporter_.recordOrderRejection(
                timestampNs,
                order,
                RejectionReason::OrderBookRejected,
                "executeOrder returned accepted=false"
            );

            return result;
        }

        result.accepted = true;

        processTrades();

        /*
            If quantity remains, the order is resting.

            If it was fully executed immediately, it will not
            exist in orderIndex.
        */
        if (
            execution.remaining_quantity > 0 &&
            containsOrder(order.id)
        ) {
            if (order.side == Side::Buy) {
                active_bid_order_id_ =
                    order.id;
            } else {
                active_ask_order_id_ =
                    order.id;
            }
        }

        return result;
    }

    void cancelQuote(
        std::optional<OrderId>& activeOrderId
    ) {
        if (!activeOrderId.has_value()) {
            return;
        }

        OrderId orderId =
            activeOrderId.value();

        if (containsOrder(orderId)) {
            cancelOrder(orderId);
        }

        activeOrderId.reset();
    }

    OrderId generateOrderId() {
        while (
            orderIndex.contains(next_order_id_) ||
            owned_order_sides_.contains(
                next_order_id_
            )
        ) {
            if (
                next_order_id_ ==
                std::numeric_limits<OrderId>::max()
            ) {
                throw std::overflow_error(
                    "Strategy order IDs exhausted"
                );
            }

            ++next_order_id_;
        }

        OrderId generatedId =
            next_order_id_;

        if (
            next_order_id_ !=
            std::numeric_limits<OrderId>::max()
        ) {
            ++next_order_id_;
        }

        return generatedId;
    }

    std::optional<Price> roundDownToTick(
        long double value
    ) const {
        long double tick =
            static_cast<long double>(
                config_.tick_size
            );

        long double rounded =
            std::floor(value / tick) *
            tick;

        return convertToPrice(rounded);
    }

    std::optional<Price> roundUpToTick(
        long double value
    ) const {
        long double tick =
            static_cast<long double>(
                config_.tick_size
            );

        long double rounded =
            std::ceil(value / tick) *
            tick;

        return convertToPrice(rounded);
    }

    static std::optional<Price> convertToPrice(
        long double value
    ) {
        long double minimum =
            1.0L;

        long double maximum =
            static_cast<long double>(
                std::numeric_limits<Price>::max()
            );

        if (
            value < minimum ||
            value > maximum
        ) {
            return std::nullopt;
        }

        return static_cast<Price>(value);
    }

    static std::optional<Price>
    convertSignedToPrice(
        long double value
    ) {
        long double minimum =
            static_cast<long double>(
                std::numeric_limits<Price>::min()
            );

        long double maximum =
            static_cast<long double>(
                std::numeric_limits<Price>::max()
            );

        if (
            value < minimum ||
            value > maximum
        ) {
            return std::nullopt;
        }

        return static_cast<Price>(value);
    }

    MarketMakerConfig config_;

    RiskManager risk_manager_;
    RejectionReporter rejection_reporter_;
    InventoryTracker inventory_tracker_;

    /*
        Keep all previously submitted strategy IDs.

        Filled orders are retained here so their trades can
        still be attributed to the strategy.
    */
    std::unordered_map<OrderId, Side>
        owned_order_sides_;

    std::optional<OrderId>
        active_bid_order_id_;

    std::optional<OrderId>
        active_ask_order_id_;

    OrderId next_order_id_;

    TimestampNs last_market_update_ns_{0};
};

// ============================================================
// P&L DATA TYPES
// ============================================================

struct SymbolPnlState {
    SymbolId symbol_id{DefaultSymbol};

    Position position{0};

    /*
        Average price of the currently open position.

        If position is zero, this is zero.
    */
    long double average_entry_price{0.0L};

    long double realised_pnl{0.0L};

    /*
        Cash generated by this symbol.

        Buys reduce cash.
        Sells increase cash.
    */
    long double net_cash_flow{0.0L};

    std::uint64_t bought_quantity{0};
    std::uint64_t sold_quantity{0};

    long double gross_buy_notional{0.0L};
    long double gross_sell_notional{0.0L};

    std::uint64_t fill_count{0};
};

struct AccountPnlState {
    ParticipantId participant_id{
        UnknownParticipant
    };

    long double initial_cash{0.0L};
    long double cash_balance{0.0L};

    std::map<SymbolId, SymbolPnlState>
        symbols;
};

struct PnlProcessingStatistics {
    std::uint64_t trades_processed{0};
    std::uint64_t trades_rejected{0};
    std::uint64_t participant_fills_applied{0};
};

struct PnlReportConfig {
    /*
        Use 100 when Price is stored in pence and you
        want the report displayed in pounds.

        Use 1 when Price represents generic ticks.
    */
    long double monetary_scale{1.0L};

    std::string currency_prefix{};

    int decimal_places{2};
};

class PnlEngine {
public:
    void registerAccount(
        ParticipantId participantId,
        long double initialCash = 0.0L
    ) {
        if (participantId == UnknownParticipant) {
            throw std::invalid_argument(
                "Participant ID zero is reserved for unknown participants"
            );
        }

        auto [iterator, inserted] =
            accounts_.try_emplace(
                participantId,
                AccountPnlState{
                    participantId,
                    initialCash,
                    initialCash,
                    {}
                }
            );

        if (!inserted) {
            throw std::invalid_argument(
                "Participant account is already registered"
            );
        }
    }

    bool containsAccount(
        ParticipantId participantId
    ) const {
        return accounts_.contains(participantId);
    }

    /*
        Processes only trades that have not previously
        been processed from this trade-history vector.
    */
    bool processNewTrades(
        const std::vector<Trade>& trades
    ) {
        if (
            processed_trade_count_ >
            trades.size()
        ) {
            processing_errors_.push_back(
                "Trade history became shorter. "
                "Call PnlEngine::reset() after resetOrderBook()."
            );

            return false;
        }

        while (
            processed_trade_count_ <
            trades.size()
        ) {
            processTrade(
                trades[processed_trade_count_]
            );

            ++processed_trade_count_;
        }

        return true;
    }

    bool processTrade(const Trade& trade) {
        if (
            trade.price <= 0 ||
            trade.quantity == 0 ||
            trade.symbol_id == 0
        ) {
            ++statistics_.trades_rejected;

            processing_errors_.push_back(
                "Rejected invalid trade: maker order " +
                std::to_string(trade.maker_order_id) +
                ", taker order " +
                std::to_string(trade.taker_order_id)
            );

            return false;
        }

        /*
            The aggressor is the incoming/taker order.

            Therefore the maker is always on the
            opposite side.
        */
        Side makerSide =
            trade.aggressor_side == Side::Buy
                ? Side::Sell
                : Side::Buy;

        Side takerSide =
            trade.aggressor_side;

        if (
            trade.maker_participant_id !=
            UnknownParticipant
        ) {
            applyFill(
                trade.maker_participant_id,
                trade.symbol_id,
                makerSide,
                trade.price,
                trade.quantity
            );

            ++statistics_
                .participant_fills_applied;
        }

        if (
            trade.taker_participant_id !=
            UnknownParticipant
        ) {
            applyFill(
                trade.taker_participant_id,
                trade.symbol_id,
                takerSide,
                trade.price,
                trade.quantity
            );

            ++statistics_
                .participant_fills_applied;
        }

        ++statistics_.trades_processed;

        return true;
    }

    bool setMarkPrice(
        SymbolId symbolId,
        long double markPrice
    ) {
        if (
            symbolId == 0 ||
            markPrice <= 0.0L
        ) {
            return false;
        }

        mark_prices_[symbolId] = markPrice;
        return true;
    }

    /*
        Uses the current order book midpoint as the mark.

        If only one side exists, it uses that side's price.
    */
    bool markFromCurrentOrderBook(
        SymbolId symbolId = DefaultSymbol
    ) {
        std::optional<Price> currentBid =
            bestBid();

        std::optional<Price> currentAsk =
            bestAsk();

        if (
            currentBid.has_value() &&
            currentAsk.has_value()
        ) {
            long double midpoint =
                (
                    static_cast<long double>(
                        currentBid.value()
                    ) +
                    static_cast<long double>(
                        currentAsk.value()
                    )
                ) /
                2.0L;

            return setMarkPrice(
                symbolId,
                midpoint
            );
        }

        if (currentBid.has_value()) {
            return setMarkPrice(
                symbolId,
                static_cast<long double>(
                    currentBid.value()
                )
            );
        }

        if (currentAsk.has_value()) {
            return setMarkPrice(
                symbolId,
                static_cast<long double>(
                    currentAsk.value()
                )
            );
        }

        return false;
    }

    std::optional<long double> markPrice(
        SymbolId symbolId
    ) const {
        auto iterator =
            mark_prices_.find(symbolId);

        if (iterator == mark_prices_.end()) {
            return std::nullopt;
        }

        return iterator->second;
    }

    const AccountPnlState* findAccount(
        ParticipantId participantId
    ) const {
        auto iterator =
            accounts_.find(participantId);

        if (iterator == accounts_.end()) {
            return nullptr;
        }

        return &iterator->second;
    }

    const SymbolPnlState* findSymbol(
        ParticipantId participantId,
        SymbolId symbolId
    ) const {
        const AccountPnlState* account =
            findAccount(participantId);

        if (account == nullptr) {
            return nullptr;
        }

        auto iterator =
            account->symbols.find(symbolId);

        if (iterator == account->symbols.end()) {
            return nullptr;
        }

        return &iterator->second;
    }

    long double unrealisedPnl(
        ParticipantId participantId,
        SymbolId symbolId
    ) const {
        const SymbolPnlState* state =
            findSymbol(
                participantId,
                symbolId
            );

        if (state == nullptr) {
            return 0.0L;
        }

        auto markIterator =
            mark_prices_.find(symbolId);

        if (markIterator == mark_prices_.end()) {
            return 0.0L;
        }

        return calculateUnrealisedPnl(
            *state,
            markIterator->second
        );
    }

    long double realisedPnl(
        ParticipantId participantId
    ) const {
        const AccountPnlState* account =
            findAccount(participantId);

        if (account == nullptr) {
            return 0.0L;
        }

        long double total = 0.0L;

        for (
            const auto& [symbolId, state] :
            account->symbols
        ) {
            static_cast<void>(symbolId);
            total += state.realised_pnl;
        }

        return total;
    }

    long double unrealisedPnl(
        ParticipantId participantId
    ) const {
        const AccountPnlState* account =
            findAccount(participantId);

        if (account == nullptr) {
            return 0.0L;
        }

        long double total = 0.0L;

        for (
            const auto& [symbolId, state] :
            account->symbols
        ) {
            auto markIterator =
                mark_prices_.find(symbolId);

            if (
                markIterator ==
                mark_prices_.end()
            ) {
                continue;
            }

            total += calculateUnrealisedPnl(
                state,
                markIterator->second
            );
        }

        return total;
    }

    long double totalPnl(
        ParticipantId participantId
    ) const {
        return realisedPnl(participantId) +
               unrealisedPnl(participantId);
    }

    long double marketValue(
        ParticipantId participantId,
        SymbolId symbolId
    ) const {
        const SymbolPnlState* state =
            findSymbol(
                participantId,
                symbolId
            );

        if (state == nullptr) {
            return 0.0L;
        }

        auto markIterator =
            mark_prices_.find(symbolId);

        if (markIterator == mark_prices_.end()) {
            return 0.0L;
        }

        return
            static_cast<long double>(
                state->position
            ) *
            markIterator->second;
    }

    long double accountEquity(
        ParticipantId participantId
    ) const {
        const AccountPnlState* account =
            findAccount(participantId);

        if (account == nullptr) {
            return 0.0L;
        }

        long double equity =
            account->cash_balance;

        for (
            const auto& [symbolId, state] :
            account->symbols
        ) {
            auto markIterator =
                mark_prices_.find(symbolId);

            /*
                When a mark is missing, value the position at
                its average entry price. This assigns zero
                unrealised P&L rather than treating the
                position as worthless.
            */
            long double valuationPrice =
                state.average_entry_price;

            if (
                markIterator !=
                mark_prices_.end()
            ) {
                valuationPrice =
                    markIterator->second;
            }

            equity +=
                static_cast<long double>(
                    state.position
                ) *
                valuationPrice;
        }

        return equity;
    }

    long double cashBalance(
        ParticipantId participantId
    ) const {
        const AccountPnlState* account =
            findAccount(participantId);

        if (account == nullptr) {
            return 0.0L;
        }

        return account->cash_balance;
    }

    Position position(
        ParticipantId participantId,
        SymbolId symbolId
    ) const {
        const SymbolPnlState* state =
            findSymbol(
                participantId,
                symbolId
            );

        if (state == nullptr) {
            return 0;
        }

        return state->position;
    }

    long double averageEntryPrice(
        ParticipantId participantId,
        SymbolId symbolId
    ) const {
        const SymbolPnlState* state =
            findSymbol(
                participantId,
                symbolId
            );

        if (state == nullptr) {
            return 0.0L;
        }

        return state->average_entry_price;
    }

    void setSymbolName(
        SymbolId symbolId,
        const std::string& symbolName
    ) {
        symbol_names_[symbolId] =
            symbolName;
    }

    const PnlProcessingStatistics&
    statistics() const {
        return statistics_;
    }

    const std::vector<std::string>&
    processingErrors() const {
        return processing_errors_;
    }

    void reset() {
        accounts_.clear();
        mark_prices_.clear();
        symbol_names_.clear();
        processing_errors_.clear();

        statistics_ = {};
        processed_trade_count_ = 0;
    }

    std::string createSummaryReport(
        const PnlReportConfig& config = {}
    ) const {
        if (config.monetary_scale <= 0.0L) {
            throw std::invalid_argument(
                "Report monetary scale must be positive"
            );
        }

        std::ostringstream output;

        output
            << std::fixed
            << std::setprecision(
                config.decimal_places
            );

        output
            << "P&L SUMMARY REPORT\n"
            << "==================\n";

        output
            << "Trades processed: "
            << statistics_.trades_processed
            << '\n';

        output
            << "Rejected trades: "
            << statistics_.trades_rejected
            << '\n';

        output
            << "Participant fills applied: "
            << statistics_
                .participant_fills_applied
            << "\n\n";

        if (accounts_.empty()) {
            output << "No participant accounts.\n";
            return output.str();
        }

        for (
            const auto& [participantId, account] :
            accounts_
        ) {
            long double accountRealised =
                realisedPnl(participantId);

            long double accountUnrealised =
                unrealisedPnl(participantId);

            long double accountTotal =
                accountRealised +
                accountUnrealised;

            long double equity =
                accountEquity(participantId);

            output
                << "Participant "
                << participantId
                << '\n';

            output
                << "-----------"
                << std::string(
                    std::to_string(
                        participantId
                    ).size(),
                    '-'
                )
                << '\n';

            output
                << "Initial cash: "
                << formatMoney(
                    account.initial_cash,
                    config
                )
                << '\n';

            output
                << "Cash balance: "
                << formatMoney(
                    account.cash_balance,
                    config
                )
                << '\n';

            output
                << "Account equity: "
                << formatMoney(
                    equity,
                    config
                )
                << '\n';

            output
                << "Realised P&L: "
                << formatMoney(
                    accountRealised,
                    config
                )
                << '\n';

            output
                << "Unrealised P&L: "
                << formatMoney(
                    accountUnrealised,
                    config
                )
                << '\n';

            output
                << "Total P&L: "
                << formatMoney(
                    accountTotal,
                    config
                )
                << "\n\n";

            for (
                const auto& [symbolId, state] :
                account.symbols
            ) {
                std::string displayName =
                    symbolDisplayName(symbolId);

                auto markIterator =
                    mark_prices_.find(symbolId);

                bool hasMark =
                    markIterator !=
                    mark_prices_.end();

                long double mark =
                    hasMark
                        ? markIterator->second
                        : 0.0L;

                long double unrealised =
                    hasMark
                        ? calculateUnrealisedPnl(
                            state,
                            mark
                        )
                        : 0.0L;

                long double total =
                    state.realised_pnl +
                    unrealised;

                long double symbolMarketValue =
                    hasMark
                        ? static_cast<long double>(
                              state.position
                          ) *
                          mark
                        : 0.0L;

                output
                    << "  Symbol: "
                    << displayName
                    << " ("
                    << symbolId
                    << ")\n";

                output
                    << "    Position: "
                    << state.position
                    << '\n';

                output
                    << "    Average entry: "
                    << formatMoney(
                        state.average_entry_price,
                        config
                    )
                    << '\n';

                output
                    << "    Mark price: ";

                if (hasMark) {
                    output
                        << formatMoney(
                            mark,
                            config
                        );
                } else {
                    output << "N/A";
                }

                output << '\n';

                output
                    << "    Market value: "
                    << formatMoney(
                        symbolMarketValue,
                        config
                    )
                    << '\n';

                output
                    << "    Bought quantity: "
                    << state.bought_quantity
                    << '\n';

                output
                    << "    Sold quantity: "
                    << state.sold_quantity
                    << '\n';

                output
                    << "    Buy notional: "
                    << formatMoney(
                        state.gross_buy_notional,
                        config
                    )
                    << '\n';

                output
                    << "    Sell notional: "
                    << formatMoney(
                        state.gross_sell_notional,
                        config
                    )
                    << '\n';

                output
                    << "    Realised P&L: "
                    << formatMoney(
                        state.realised_pnl,
                        config
                    )
                    << '\n';

                output
                    << "    Unrealised P&L: "
                    << formatMoney(
                        unrealised,
                        config
                    )
                    << '\n';

                output
                    << "    Total P&L: "
                    << formatMoney(
                        total,
                        config
                    )
                    << '\n';

                output
                    << "    Fills: "
                    << state.fill_count
                    << "\n\n";
            }
        }

        return output.str();
    }

private:
    AccountPnlState& ensureAccount(
        ParticipantId participantId
    ) {
        auto [iterator, inserted] =
            accounts_.try_emplace(
                participantId,
                AccountPnlState{
                    participantId,
                    0.0L,
                    0.0L,
                    {}
                }
            );

        static_cast<void>(inserted);

        return iterator->second;
    }

    void applyFill(
        ParticipantId participantId,
        SymbolId symbolId,
        Side side,
        Price price,
        Quantity quantity
    ) {
        AccountPnlState& account =
            ensureAccount(participantId);

        SymbolPnlState& symbol =
            account.symbols[symbolId];

        symbol.symbol_id = symbolId;

        long double tradePrice =
            static_cast<long double>(price);

        long double tradeQuantity =
            static_cast<long double>(quantity);

        long double tradeNotional =
            tradePrice *
            tradeQuantity;

        Position signedQuantity =
            static_cast<Position>(quantity);

        if (side == Side::Buy) {
            account.cash_balance -=
                tradeNotional;

            symbol.net_cash_flow -=
                tradeNotional;

            symbol.bought_quantity +=
                quantity;

            symbol.gross_buy_notional +=
                tradeNotional;
        } else {
            signedQuantity =
                -signedQuantity;

            account.cash_balance +=
                tradeNotional;

            symbol.net_cash_flow +=
                tradeNotional;

            symbol.sold_quantity +=
                quantity;

            symbol.gross_sell_notional +=
                tradeNotional;
        }

        updateAverageCostPosition(
            symbol,
            signedQuantity,
            tradePrice
        );

        ++symbol.fill_count;
    }

    static void updateAverageCostPosition(
        SymbolPnlState& state,
        Position signedTradeQuantity,
        long double tradePrice
    ) {
        Position previousPosition =
            state.position;

        if (previousPosition == 0) {
            state.position =
                signedTradeQuantity;

            state.average_entry_price =
                tradePrice;

            return;
        }

        bool sameDirection =
            (
                previousPosition > 0 &&
                signedTradeQuantity > 0
            ) ||
            (
                previousPosition < 0 &&
                signedTradeQuantity < 0
            );

        if (sameDirection) {
            long double previousQuantity =
                absolutePosition(
                    previousPosition
                );

            long double additionalQuantity =
                absolutePosition(
                    signedTradeQuantity
                );

            Position newPosition =
                previousPosition +
                signedTradeQuantity;

            long double newAbsoluteQuantity =
                absolutePosition(newPosition);

            state.average_entry_price =
                (
                    previousQuantity *
                        state.average_entry_price +
                    additionalQuantity *
                        tradePrice
                ) /
                newAbsoluteQuantity;

            state.position = newPosition;
            return;
        }

        /*
            The trade is closing some or all of the
            existing position.
        */
        long double closingQuantity =
            std::min(
                absolutePosition(previousPosition),
                absolutePosition(
                    signedTradeQuantity
                )
            );

        if (previousPosition > 0) {
            /*
                Closing a long position by selling.
            */
            state.realised_pnl +=
                (
                    tradePrice -
                    state.average_entry_price
                ) *
                closingQuantity;
        } else {
            /*
                Closing a short position by buying.
            */
            state.realised_pnl +=
                (
                    state.average_entry_price -
                    tradePrice
                ) *
                closingQuantity;
        }

        Position newPosition =
            previousPosition +
            signedTradeQuantity;

        if (newPosition == 0) {
            state.position = 0;
            state.average_entry_price = 0.0L;
            return;
        }

        bool retainedOriginalDirection =
            (
                previousPosition > 0 &&
                newPosition > 0
            ) ||
            (
                previousPosition < 0 &&
                newPosition < 0
            );

        if (retainedOriginalDirection) {
            /*
                Partial close. The remaining position keeps
                its original average entry price.
            */
            state.position = newPosition;
            return;
        }

        /*
            The trade was larger than the existing position,
            so the account flipped from long to short or
            short to long.

            The leftover position was opened at this trade's
            price.
        */
        state.position = newPosition;
        state.average_entry_price =
            tradePrice;
    }

    static long double calculateUnrealisedPnl(
        const SymbolPnlState& state,
        long double markPrice
    ) {
        if (state.position > 0) {
            return
                (
                    markPrice -
                    state.average_entry_price
                ) *
                static_cast<long double>(
                    state.position
                );
        }

        if (state.position < 0) {
            return
                (
                    state.average_entry_price -
                    markPrice
                ) *
                absolutePosition(
                    state.position
                );
        }

        return 0.0L;
    }

    static long double absolutePosition(
        Position position
    ) {
        if (position >= 0) {
            return static_cast<long double>(
                position
            );
        }

        return -static_cast<long double>(
            position
        );
    }

    static std::string formatMoney(
        long double rawValue,
        const PnlReportConfig& config
    ) {
        std::ostringstream output;

        output
            << std::fixed
            << std::setprecision(
                config.decimal_places
            );

        output
            << config.currency_prefix
            << (
                rawValue /
                config.monetary_scale
            );

        return output.str();
    }

    std::string symbolDisplayName(
        SymbolId symbolId
    ) const {
        auto iterator =
            symbol_names_.find(symbolId);

        if (
            iterator ==
            symbol_names_.end()
        ) {
            return "SYMBOL_" +
                   std::to_string(symbolId);
        }

        return iterator->second;
    }

    std::map<
        ParticipantId,
        AccountPnlState
    > accounts_;

    std::map<
        SymbolId,
        long double
    > mark_prices_;

    std::map<
        SymbolId,
        std::string
    > symbol_names_;

    std::size_t processed_trade_count_{0};

    PnlProcessingStatistics statistics_;

    std::vector<std::string>
        processing_errors_;
};


struct EventRecordDecodeResult {
    bool success{false};
    MarketEvent event{};
    std::string error;
};

EventRecordDecodeResult decodeEventRecord(
    const std::uint8_t* data,
    std::size_t size
) {
    EventRecordDecodeResult result;

    if (data == nullptr) {
        result.error = "null record data";
        return result;
    }

    if (size != BinaryFormat::RecordSize) {
        result.error =
            "unexpected UDP record size";

        return result;
    }

    std::vector<std::uint8_t> record(
        data,
        data + size
    );

    std::uint32_t storedChecksum = 0;

    std::size_t checksumOffset =
        BinaryFormat::RecordPayloadSize;

    if (!readLittleEndian(
        record,
        checksumOffset,
        storedChecksum
    )) {
        result.error =
            "unable to decode checksum";

        return result;
    }

    std::uint32_t calculatedChecksum =
        calculateChecksum(
            record,
            BinaryFormat::RecordPayloadSize
        );

    if (storedChecksum != calculatedChecksum) {
        result.error = "checksum mismatch";
        return result;
    }

    std::size_t offset = 0;

    std::uint32_t recordMagic = 0;
    std::uint16_t recordSize = 0;
    std::uint8_t recordVersion = 0;
    std::uint8_t eventTypeValue = 0;

    std::uint64_t sequenceNumber = 0;
    std::uint64_t timestampNs = 0;

    OrderId orderId = 0;
    Price price = 0;
    Quantity quantity = 0;

    std::uint8_t sideValue = 0;

    ParticipantId participantId =
        UnknownParticipant;

    SymbolId symbolId =
        DefaultSymbol;

    std::uint8_t reserved0 = 0;
    std::uint8_t reserved1 = 0;
    std::uint8_t reserved2 = 0;

    bool decoded =
        readLittleEndian(
            record,
            offset,
            recordMagic
        ) &&
        readLittleEndian(
            record,
            offset,
            recordSize
        ) &&
        readLittleEndian(
            record,
            offset,
            recordVersion
        ) &&
        readLittleEndian(
            record,
            offset,
            eventTypeValue
        ) &&
        readLittleEndian(
            record,
            offset,
            sequenceNumber
        ) &&
        readLittleEndian(
            record,
            offset,
            timestampNs
        ) &&
        readLittleEndian(
            record,
            offset,
            orderId
        ) &&
        readLittleEndian(
            record,
            offset,
            price
        ) &&
            readLittleEndian(
        record,
        offset,
        quantity
    ) &&
    readLittleEndian(
        record,
        offset,
        sideValue
    ) &&
    readLittleEndian(
        record,
        offset,
        participantId
    ) &&
    readLittleEndian(
        record,
        offset,
        symbolId
    ) &&
    readLittleEndian(
        record,
        offset,
        reserved0
    ) &&
    readLittleEndian(
        record,
        offset,
        reserved1
    ) &&
    readLittleEndian(
        record,
        offset,
        reserved2

        );

    if (!decoded) {
        result.error =
            "unable to decode UDP record";

        return result;
    }

    if (
        recordMagic !=
        BinaryFormat::RecordMagic
    ) {
        result.error = "invalid record magic";
        return result;
    }

    if (
        recordSize !=
        BinaryFormat::RecordSize
    ) {
        result.error = "invalid record size";
        return result;
    }

    if (
        recordVersion !=
        BinaryFormat::RecordVersion
    ) {
        result.error =
            "unsupported record version";

        return result;
    }

    if (
        eventTypeValue <
            static_cast<std::uint8_t>(
                EventType::Add
            ) ||
        eventTypeValue >
            static_cast<std::uint8_t>(
                EventType::Execute
            )
    ) {
        result.error = "invalid event type";
        return result;
    }

    if (sideValue > 1) {
        result.error = "invalid side";
        return result;
    }

    MarketEvent event{
        sequenceNumber,
        timestampNs,
        static_cast<EventType>(
            eventTypeValue
        ),
        orderId,
        static_cast<Side>(sideValue),
        price,
        quantity,
        participantId,
        symbolId
    };

    std::string validationReason;

    if (!validateMarketEvent(
        event,
        validationReason
    )) {
        result.error = validationReason;
        return result;
    }

    result.success = true;
    result.event = event;

    return result;
}

class RecoverySource {
public:
    virtual ~RecoverySource() = default;

    virtual std::optional<MarketEvent>
    fetch(
        std::uint64_t sequenceNumber
    ) = 0;
};

class InMemoryRecoverySource final
    : public RecoverySource {
public:
    explicit InMemoryRecoverySource(
        const std::vector<MarketEvent>& events
    ) {
        for (const MarketEvent& event : events) {
            events_[event.sequence_number] =
                event;
        }
    }

    std::optional<MarketEvent> fetch(
        std::uint64_t sequenceNumber
    ) override {
        auto iterator =
            events_.find(sequenceNumber);

        if (iterator == events_.end()) {
            return std::nullopt;
        }

        return iterator->second;
    }

private:
    std::map<
        std::uint64_t,
        MarketEvent
    > events_;
};

class TcpRecoverySource final
    : public RecoverySource {
public:
    TcpRecoverySource(
        std::string serverAddress,
        std::uint16_t serverPort
    )
        : server_address_(
              std::move(serverAddress)
          ),
          server_port_(serverPort) {
    }

    std::optional<MarketEvent> fetch(
        std::uint64_t sequenceNumber
    ) override {
        last_error_.clear();

        SOCKET recoverySocket =
            socket(
                AF_INET,
                SOCK_STREAM,
                IPPROTO_TCP
            );

        if (
            recoverySocket ==
            INVALID_SOCKET
        ) {
            last_error_ =
                "Unable to create TCP socket";

            return std::nullopt;
        }

        sockaddr_in server{};

        server.sin_family = AF_INET;
        server.sin_port =
            htons(server_port_);

        int addressResult = inet_pton(
            AF_INET,
            server_address_.c_str(),
            &server.sin_addr
        );

        if (addressResult != 1) {
            closesocket(recoverySocket);

            last_error_ =
                "Invalid recovery-server address";

            return std::nullopt;
        }

        int connectionResult = connect(
            recoverySocket,
            reinterpret_cast<
                const sockaddr*
            >(&server),
            sizeof(server)
        );

        if (
            connectionResult ==
            SOCKET_ERROR
        ) {
            closesocket(recoverySocket);

            last_error_ =
                "Unable to connect to recovery server";

            return std::nullopt;
        }

        /*
            Request protocol:

            8 bytes: missing sequence number,
            little-endian.
        */
        std::vector<std::uint8_t> request;

        appendLittleEndian(
            request,
            sequenceNumber
        );

        if (!sendAll(
            recoverySocket,
            request.data(),
            request.size()
        )) {
            closesocket(recoverySocket);

            last_error_ =
                "Unable to send recovery request";

            return std::nullopt;
        }

        /*
            Response protocol:

            status = 0: sequence was not found.
            status = 1: a 64-byte event follows.
        */
        std::uint8_t status = 0;

        if (!receiveAll(
            recoverySocket,
            &status,
            sizeof(status)
        )) {
            closesocket(recoverySocket);

            last_error_ =
                "Unable to receive recovery status";

            return std::nullopt;
        }

        if (status != 1) {
            closesocket(recoverySocket);

            last_error_ =
                "Recovery sequence was not found";

            return std::nullopt;
        }

        std::array<
            std::uint8_t,
            BinaryFormat::RecordSize
        > record{};

        if (!receiveAll(
            recoverySocket,
            record.data(),
            record.size()
        )) {
            closesocket(recoverySocket);

            last_error_ =
                "Unable to receive recovery event";

            return std::nullopt;
        }

        closesocket(recoverySocket);

        EventRecordDecodeResult decoded =
            decodeEventRecord(
                record.data(),
                record.size()
            );

        if (!decoded.success) {
            last_error_ =
                decoded.error;

            return std::nullopt;
        }

        if (
            decoded.event.sequence_number !=
            sequenceNumber
        ) {
            last_error_ =
                "Recovery server returned "
                "the wrong sequence";

            return std::nullopt;
        }

        return decoded.event;
    }

    const std::string& lastError() const {
        return last_error_;
    }

private:
    static bool sendAll(
        SOCKET socketHandle,
        const std::uint8_t* data,
        std::size_t size
    ) {
        std::size_t sentTotal = 0;

        while (sentTotal < size) {
            int sent = send(
                socketHandle,
                reinterpret_cast<
                    const char*
                >(data + sentTotal),
                static_cast<int>(
                    size - sentTotal
                ),
                0
            );

            if (
                sent == SOCKET_ERROR ||
                sent == 0
            ) {
                return false;
            }

            sentTotal +=
                static_cast<std::size_t>(
                    sent
                );
        }

        return true;
    }

    static bool receiveAll(
        SOCKET socketHandle,
        std::uint8_t* data,
        std::size_t size
    ) {
        std::size_t receivedTotal = 0;

        while (receivedTotal < size) {
            int received = recv(
                socketHandle,
                reinterpret_cast<char*>(
                    data + receivedTotal
                ),
                static_cast<int>(
                    size - receivedTotal
                ),
                0
            );

            if (
                received == SOCKET_ERROR ||
                received == 0
            ) {
                return false;
            }

            receivedTotal +=
                static_cast<std::size_t>(
                    received
                );
        }

        return true;
    }

    std::string server_address_;
    std::uint16_t server_port_;

    std::string last_error_;
};




enum class FeedState {
    Healthy,
    Recovering,
    Failed
};

struct RecoveryStatistics {
    std::uint64_t events_received{0};
    std::uint64_t events_applied{0};

    std::uint64_t gaps_detected{0};
    std::uint64_t out_of_order_events{0};
    std::uint64_t duplicates_ignored{0};

    std::uint64_t events_recovered{0};
    std::uint64_t recovery_misses{0};

    std::uint64_t application_rejections{0};
    std::uint64_t reorder_buffer_overflows{0};

    std::size_t maximum_buffer_depth{0};
};


class RecoverySequencer {
public:
    RecoverySequencer(
        std::uint64_t expectedFirstSequence,
        std::size_t maximumReorderDepth,
        RecoverySource& recoverySource
    )
        : expected_sequence_(
              expectedFirstSequence
          ),
          maximum_reorder_depth_(
              maximumReorderDepth
          ),
          recovery_source_(
              recoverySource
          ) {
        if (expectedFirstSequence == 0) {
            throw std::invalid_argument(
                "Expected sequence cannot be zero"
            );
        }

        if (maximumReorderDepth == 0) {
            throw std::invalid_argument(
                "Maximum reorder depth cannot be zero"
            );
        }
    }

    bool onEvent(const MarketEvent& event) {
        if (state_ == FeedState::Failed) {
            return false;
        }

        ++statistics_.events_received;

        if (
            event.sequence_number <
            expected_sequence_
        ) {
            ++statistics_.duplicates_ignored;
            return true;
        }

        if (
            event.sequence_number ==
            expected_sequence_
        ) {
            if (!applyExpectedEvent(event)) {
                return false;
            }

            return drainContiguousEvents();
        }

        // Event is ahead of the expected sequence.
        ++statistics_.out_of_order_events;
        ++statistics_.gaps_detected;

        if (
            event.sequence_number -
                expected_sequence_ >
            maximum_reorder_depth_
        ) {
            ++statistics_
                .reorder_buffer_overflows;

            state_ = FeedState::Failed;
            return false;
        }

        auto [iterator, inserted] =
            reorder_buffer_.try_emplace(
                event.sequence_number,
                event
            );

        static_cast<void>(iterator);

        if (!inserted) {
            ++statistics_.duplicates_ignored;
            return true;
        }

        updateMaximumBufferDepth();

        state_ = FeedState::Recovering;

        /*
            Do not recover inside onEvent().

            Return control to the processing loop first so the
            strategy can cancel its quotes before recovery begins.
        */
        return true;
    }

    bool retryRecovery() {
        if (
            state_ == FeedState::Failed ||
            reorder_buffer_.empty()
        ) {
            return state_ != FeedState::Failed;
        }

        std::uint64_t highestBufferedSequence =
            reorder_buffer_.rbegin()->first;

        attemptRecoveryUntil(
            highestBufferedSequence
        );

        return drainContiguousEvents();
    }

    FeedState state() const {
        return state_;
    }

    bool healthy() const {
        return state_ == FeedState::Healthy;
    }

    std::uint64_t expectedSequence() const {
        return expected_sequence_;
    }

    std::size_t bufferedEventCount() const {
        return reorder_buffer_.size();
    }

    const RecoveryStatistics&
    statistics() const {
        return statistics_;
    }

private:
    void attemptRecoveryUntil(
        std::uint64_t sequenceExclusive
    ) {
        for (
            std::uint64_t sequence =
                expected_sequence_;
            sequence < sequenceExclusive;
            ++sequence
        ) {
            if (
                reorder_buffer_.contains(
                    sequence
                )
            ) {
                continue;
            }

            std::optional<MarketEvent>
                recovered =
                    recovery_source_.fetch(
                        sequence
                    );

            if (!recovered.has_value()) {
                ++statistics_.recovery_misses;
                return;
            }

            reorder_buffer_.emplace(
                sequence,
                recovered.value()
            );

            ++statistics_.events_recovered;

            updateMaximumBufferDepth();
        }
    }

    bool drainContiguousEvents() {
        while (true) {
            auto iterator =
                reorder_buffer_.find(
                    expected_sequence_
                );

            if (
                iterator ==
                reorder_buffer_.end()
            ) {
                break;
            }

            MarketEvent event =
                iterator->second;

            reorder_buffer_.erase(iterator);

            if (!applyExpectedEvent(event)) {
                return false;
            }
        }

        if (reorder_buffer_.empty()) {
            state_ = FeedState::Healthy;
        } else {
            state_ = FeedState::Recovering;
        }

        return true;
    }

    bool applyExpectedEvent(
        const MarketEvent& event
    ) {
        if (
            event.sequence_number !=
            expected_sequence_
        ) {
            state_ = FeedState::Failed;
            return false;
        }

        ApplyEventResult application =
            applyMarketEvent(event);

        if (!application.accepted) {
            ++statistics_
                .application_rejections;

            state_ = FeedState::Failed;
            return false;
        }

        ++statistics_.events_applied;

        if (
            expected_sequence_ ==
            std::numeric_limits<
                std::uint64_t
            >::max()
        ) {
            state_ = FeedState::Failed;
            return false;
        }

        ++expected_sequence_;
        return true;
    }

    void updateMaximumBufferDepth() {
        statistics_.maximum_buffer_depth =
            std::max(
                statistics_
                    .maximum_buffer_depth,
                reorder_buffer_.size()
            );
    }

    std::uint64_t expected_sequence_;
    std::size_t maximum_reorder_depth_;

    RecoverySource& recovery_source_;

    std::map<
        std::uint64_t,
        MarketEvent
    > reorder_buffer_;

    FeedState state_{FeedState::Healthy};

    RecoveryStatistics statistics_;
};

#include "analytics/simulation_telemetry_bridge.hpp"
#include <cstdlib>

struct UdpReceiverStatistics {
    std::uint64_t packets_received{0};
    std::uint64_t packets_queued{0};
    std::uint64_t invalid_size_packets{0};
    std::uint64_t queue_full_drops{0};
    std::uint64_t socket_errors{0};
};

class UdpReceiver {
public:
    UdpReceiver(
        NetworkQueue& queue,
        std::uint16_t port
    )
        : queue_(queue),
          port_(port) {
        socket_ = socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP
        );

        if (socket_ == INVALID_SOCKET) {
            throw std::runtime_error(
                "Unable to create UDP socket: " +
                std::to_string(
                    WSAGetLastError()
                )
            );
        }

        sockaddr_in address{};

        address.sin_family = AF_INET;
        address.sin_addr.s_addr =
            htonl(INADDR_ANY);

        address.sin_port =
            htons(port_);

        int bindResult = bind(
            socket_,
            reinterpret_cast<
                const sockaddr*
            >(&address),
            sizeof(address)
        );

        if (bindResult == SOCKET_ERROR) {
            int error = WSAGetLastError();

            closesocket(socket_);
            socket_ = INVALID_SOCKET;

            throw std::runtime_error(
                "Unable to bind UDP socket: " +
                std::to_string(error)
            );
        }
    }

    ~UdpReceiver() {
        stop();

        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }

    UdpReceiver(
        const UdpReceiver&
    ) = delete;

    UdpReceiver& operator=(
        const UdpReceiver&
    ) = delete;

    void start() {
        bool expected = false;

        if (!running_.compare_exchange_strong(
            expected,
            true
        )) {
            return;
        }

        receiver_thread_ =
            std::thread(
                &UdpReceiver::receiveLoop,
                this
            );
    }

    void stop() {
        running_.store(false);

        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

    UdpReceiverStatistics statistics() const {
        return UdpReceiverStatistics{
            packets_received_.load(),
            packets_queued_.load(),
            invalid_size_packets_.load(),
            queue_full_drops_.load(),
            socket_errors_.load()
        };
    }

private:
    void receiveLoop() {
        /*
            Use a larger temporary buffer so oversized UDP
            packets can be identified and rejected cleanly.
        */
        std::array<char, 2048>
            receiveBuffer{};

        while (running_.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(socket_, &readSet);

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 100'000;

            int ready = select(
                0,
                &readSet,
                nullptr,
                nullptr,
                &timeout
            );

            if (ready == SOCKET_ERROR) {
                ++socket_errors_;
                continue;
            }

            if (ready == 0) {
                continue;
            }

            sockaddr_storage senderAddress{};
            int senderAddressSize =
                sizeof(senderAddress);

            int receivedBytes = recvfrom(
                socket_,
                receiveBuffer.data(),
                static_cast<int>(
                    receiveBuffer.size()
                ),
                0,
                reinterpret_cast<sockaddr*>(
                    &senderAddress
                ),
                &senderAddressSize
            );

            if (receivedBytes == SOCKET_ERROR) {
                ++socket_errors_;
                continue;
            }

            ++packets_received_;

            if (
                receivedBytes !=
                static_cast<int>(
                    BinaryFormat::RecordSize
                )
            ) {
                ++invalid_size_packets_;
                continue;
            }

            UdpDatagram datagram;

            std::memcpy(
                datagram.bytes.data(),
                receiveBuffer.data(),
                BinaryFormat::RecordSize
            );

            datagram.size =
                BinaryFormat::RecordSize;

            datagram.received_timestamp_ns =
                steadyTimestampNs();

            if (!queue_.tryPush(datagram)) {
                ++queue_full_drops_;
                continue;
            }

            ++packets_queued_;
        }
    }

    NetworkQueue& queue_;
    std::uint16_t port_;

    SOCKET socket_{INVALID_SOCKET};

    std::atomic<bool> running_{false};
    std::thread receiver_thread_;

    std::atomic<std::uint64_t>
        packets_received_{0};

    std::atomic<std::uint64_t>
        packets_queued_{0};

    std::atomic<std::uint64_t>
        invalid_size_packets_{0};

    std::atomic<std::uint64_t>
        queue_full_drops_{0};

    std::atomic<std::uint64_t>
        socket_errors_{0};
};


struct UdpProcessingStatistics {
    std::uint64_t datagrams_processed{0};
    std::uint64_t decode_failures{0};
    std::uint64_t sequencer_failures{0};

    std::uint64_t recovery_pauses{0};
    std::uint64_t recovery_resumes{0};

    std::uint64_t quote_refreshes{0};
};

class UdpFeedProcessor {
public:
    UdpFeedProcessor(
    NetworkQueue& queue,
    RecoverySequencer& sequencer,
    MarketMakerStrategy& strategy,
    PnlEngine& pnl,
    analytics::SimulationTelemetryBridge*
        telemetryBridge = nullptr,
    ParticipantId participantId = 5001,
    SymbolId symbolId = DefaultSymbol,
    bool updateStrategy = true,
    std::uint64_t telemetrySampleInterval = 1,
    std::size_t telemetryDepthLevels = 20
)
    : queue_(queue),
      sequencer_(sequencer),
      strategy_(strategy),
      pnl_(pnl),
      telemetry_bridge_(telemetryBridge),
      participant_id_(participantId),
      symbol_id_(symbolId),
      update_strategy_(updateStrategy),
      telemetry_sample_interval_(
          telemetrySampleInterval == 0
              ? 1
              : telemetrySampleInterval
      ),
      telemetry_depth_levels_(
          telemetryDepthLevels
      ) {
}

    std::size_t processAvailable() {
        std::size_t processedThisCall = 0;

        UdpDatagram datagram;

        while (queue_.tryPop(datagram)) {
            ++processedThisCall;
            ++statistics_
                .datagrams_processed;

            EventRecordDecodeResult decoded =
                decodeEventRecord(
                    datagram.bytes.data(),
                    datagram.size
                );

            if (!decoded.success) {
                ++statistics_.decode_failures;
                continue;
            }


            TimestampNs eventTimestamp = decoded.event.timestamp_ns;
            last_event_timestamp_ = eventTimestamp;

            ++telemetry_events_seen_;

            bool accepted =
                sequencer_.onEvent(
                    decoded.event
                );

            if (
                    !accepted ||
                    sequencer_.state() == FeedState::Failed
                ) {
                ++statistics_
                    .sequencer_failures;

                pauseStrategy(
                    eventTimestamp,
                    "Market-data feed failed"
                );

                failRecoveryTelemetry(
                    eventTimestamp
                );

                captureTelemetry(
    eventTimestamp,
    true
);

                continue;
}

            if (
                    sequencer_.state() == FeedState::Recovering
                ) {
                beginRecoveryTelemetry(
                    eventTimestamp,
                    decoded.event.sequence_number
                );

                pauseStrategy(
                    eventTimestamp,
                    "Market-data recovery in progress"
                );

                /*
                    Capture the Recovering state before attempting
                    to repair the gap.
                */
                captureTelemetry(
                    eventTimestamp,
                    true
                );

                bool recoverySucceeded =
                    sequencer_.retryRecovery();

                if (
                        !recoverySucceeded ||
                        sequencer_.state() ==
                            FeedState::Failed
                    ) {
                    ++statistics_
                        .sequencer_failures;

                    failRecoveryTelemetry(
                        eventTimestamp
                    );

                    captureTelemetry(
    eventTimestamp,
    true
);

                    continue;
                }
            }

            if (sequencer_.healthy()) {
                const bool recoveryJustCompleted =
                    telemetry_recovery_active_;

                finishRecoveryTelemetry(
                    eventTimestamp
                );

                resumeStrategyIfNecessary(
                    eventTimestamp
                );

                if (update_strategy_) {
                    updateStrategyAndAccounting(
                        eventTimestamp
                    );
                } else {
                    /*
                        In a market-data-only replay, do not submit
                        strategy quotes or alter the reconstructed book.
                    */
                    captureTelemetry(
                        eventTimestamp,
                        recoveryJustCompleted
                    );
                }
            }
        }

        /*
    Retry an incomplete recovery when the processing
    loop is called without receiving another datagram.
*/
        if (
    processedThisCall == 0 &&
    sequencer_.state() ==
        FeedState::Recovering &&
    last_event_timestamp_ != 0
) {
            pauseStrategy(
                last_event_timestamp_,
                "Market-data recovery in progress"
            );

            bool recoverySucceeded =
                sequencer_.retryRecovery();

            if (
                !recoverySucceeded ||
                sequencer_.state() ==
                    FeedState::Failed
            ) {
                ++statistics_.sequencer_failures;

                failRecoveryTelemetry(
                    last_event_timestamp_
                );

                captureTelemetry(
                    last_event_timestamp_,
                    true
                );
            } else if (sequencer_.healthy()) {
                const bool recoveryJustCompleted =
                    telemetry_recovery_active_;

                finishRecoveryTelemetry(
                    last_event_timestamp_
                );

                resumeStrategyIfNecessary(
                    last_event_timestamp_
                );

                if (update_strategy_) {
                    updateStrategyAndAccounting(
                        last_event_timestamp_
                    );
                } else {
                    captureTelemetry(
                        last_event_timestamp_,
                        recoveryJustCompleted
                    );
                }
            }
}

        return processedThisCall;
    }

    const UdpProcessingStatistics&
    statistics() const {
        return statistics_;
    }


private:
    void beginRecoveryTelemetry(
        TimestampNs timestamp,
        std::uint64_t receivedSequence
    ) {
        if (
            telemetry_bridge_ == nullptr ||
            telemetry_recovery_active_
        ) {
            return;
        }

        telemetry_bridge_->recordRecoveryStart(
            timestamp,
            sequencer_.expectedSequence(),
            receivedSequence,
            sequencer_.bufferedEventCount()
        );

        telemetry_recovery_active_ = true;
    }

    void finishRecoveryTelemetry(
        TimestampNs timestamp
    ) {
        if (
            telemetry_bridge_ == nullptr ||
            !telemetry_recovery_active_
        ) {
            return;
        }

        telemetry_bridge_->recordRecoveryEnd(
            timestamp,
            sequencer_.expectedSequence(),
            sequencer_.bufferedEventCount()
        );

        telemetry_recovery_active_ = false;
    }

    void failRecoveryTelemetry(
    TimestampNs timestamp
) {
        if (
            telemetry_bridge_ == nullptr ||
            !telemetry_recovery_active_
        ) {
            return;
        }

        telemetry_bridge_->recordRecoveryFailure(
            timestamp,
            sequencer_.expectedSequence(),
            sequencer_.bufferedEventCount()
        );

        telemetry_recovery_active_ = false;

    }

    void captureTelemetry(
    TimestampNs timestamp,
    bool force = false
) {
        if (telemetry_bridge_ == nullptr) {
            return;
        }

        /*
            Record ordinary state only at the configured sample
            interval. Recovery transitions are always forced.
        */
        if (
            !force &&
            telemetry_events_seen_ %
                telemetry_sample_interval_ !=
                0
        ) {
            return;
        }

        telemetry_bridge_->capture(
            timestamp,
            participant_id_,
            symbol_id_,
            pnl_,
            strategy_,
            &queue_,
            &sequencer_,
            telemetry_depth_levels_
        );
    }
    void pauseStrategy(
        TimestampNs timestamp,
        const std::string& reason
    ) {
        if (recovery_pause_active_) {
            return;
        }

        /*
            Do not claim ownership of a kill switch that was
            already active for another reason.
        */
        if (
            strategy_.riskManager()
                .killSwitchActive()
        ) {
            return;
        }

        strategy_.activateKillSwitch(
            timestamp,
            reason
        );

        recovery_pause_active_ = true;

        ++statistics_.recovery_pauses;
    }

    void resumeStrategyIfNecessary(
        TimestampNs timestamp
    ) {
        if (!recovery_pause_active_) {
            return;
        }

        strategy_.deactivateKillSwitch();

        recovery_pause_active_ = false;

        ++statistics_.recovery_resumes;

        /*
            The recovered book now represents the latest
            complete market-data state.
        */
        strategy_.onMarketUpdate(timestamp);
    }

    void updateStrategyAndAccounting(
        TimestampNs timestamp
    ) {
        /*
            First process trades generated by incoming
            market-data events.
        */
        strategy_.processTrades();

        pnl_.processNewTrades(
            tradeHistory
        );

        pnl_.markFromCurrentOrderBook(
            DefaultSymbol
        );

        strategy_.onMarketUpdate(timestamp);

        QuoteRefreshResult quotes =
            strategy_.refreshQuotes(
                timestamp
            );

        if (telemetry_bridge_ != nullptr) {
            telemetry_bridge_->recordQuoteRefresh(
                timestamp,
                quotes
            );
        }

        if (quotes.market_accepted) {
            ++statistics_.quote_refreshes;
        }

        /*
            Process any trades potentially generated while
            submitting the refreshed quotes.
        */
        strategy_.processTrades();

        pnl_.processNewTrades(
            tradeHistory
        );

        pnl_.markFromCurrentOrderBook(
            DefaultSymbol
        );
        captureTelemetry(timestamp);
    }

    NetworkQueue& queue_;
    RecoverySequencer& sequencer_;

    MarketMakerStrategy& strategy_;
    PnlEngine& pnl_;

    bool recovery_pause_active_{false};

    TimestampNs last_event_timestamp_{0};

    analytics::SimulationTelemetryBridge*
        telemetry_bridge_{nullptr};

    ParticipantId participant_id_{5001};
    SymbolId symbol_id_{DefaultSymbol};

    bool telemetry_recovery_active_{false};

    bool update_strategy_{true};

    std::uint64_t telemetry_sample_interval_{1};

    std::size_t telemetry_depth_levels_{20};

    std::uint64_t telemetry_events_seen_{0};

    UdpProcessingStatistics statistics_;
};



#include <cstdlib>
#include <numeric>
#include <vector>

#define CHECK(condition)                                                     \
do {                                                                     \
if (!(condition)) {                                                  \
std::cerr << "CHECK failed: " #condition                         \
<< " at line " << __LINE__ << '\n';                    \
std::exit(EXIT_FAILURE);                                         \
}                                                                    \
} while (false)

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


void checkBookInvariants() {
    std::size_t numberOfOrdersInBook = 0;

    auto checkSide = [&](auto& book, Side expectedSide) {
        for (auto& [price, level] : book) {
            CHECK(!level.orders.empty());

            std::uint64_t calculatedTotal = 0;

            for (
                auto orderIterator = level.orders.begin();
                orderIterator != level.orders.end();
                ++orderIterator
            ) {
                ++numberOfOrdersInBook;
                calculatedTotal += orderIterator->quantity;

                CHECK(orderIterator->side == expectedSide);
                CHECK(orderIterator->price == price);
                CHECK(orderIterator->quantity > 0);

                auto indexIterator =
                    orderIndex.find(orderIterator->id);

                CHECK(indexIterator != orderIndex.end());
                CHECK(indexIterator->second.side == expectedSide);
                CHECK(indexIterator->second.price == price);

                // orderIndex must point to this exact list element.
                CHECK(
                    indexIterator->second.iterator ==
                    orderIterator
                );
            }

            CHECK(calculatedTotal == level.total_quantity);
        }
    };

    checkSide(bids, Side::Buy);
    checkSide(asks, Side::Sell);

    CHECK(numberOfOrdersInBook == orderIndex.size());

    for (const auto& [id, location] : orderIndex) {
        CHECK(location.iterator->id == id);
        CHECK(location.iterator->side == location.side);
        CHECK(location.iterator->price == location.price);
        CHECK(location.iterator->quantity > 0);

        if (location.side == Side::Buy) {
            CHECK(bids.contains(location.price));
        } else {
            CHECK(asks.contains(location.price));
        }
    }
}



void testExecuteRejectsZeroQuantity() {
    resetOrderBook();

    ExecutionResult result = executeOrder(
        Order{1, Side::Buy, 10000, 0}
    );

    CHECK(!result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.empty());

    CHECK(bids.empty());
    CHECK(asks.empty());
    CHECK(orderIndex.empty());
    CHECK(tradeHistory.empty());

    checkBookInvariants();
}

void testExecuteRejectsInvalidPrice() {
    resetOrderBook();

    ExecutionResult zeroPrice = executeOrder(
        Order{1, Side::Buy, 0, 10}
    );

    CHECK(!zeroPrice.accepted);

    ExecutionResult negativePrice = executeOrder(
        Order{2, Side::Sell, -100, 10}
    );

    CHECK(!negativePrice.accepted);

    CHECK(bids.empty());
    CHECK(asks.empty());
    CHECK(orderIndex.empty());
    CHECK(tradeHistory.empty());

    checkBookInvariants();
}

void testExecuteRejectsDuplicateId() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10000, 10}));

    ExecutionResult result = executeOrder(
        Order{1, Side::Sell, 9000, 5}
    );

    CHECK(!result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 5);
    CHECK(result.trades.empty());

    // Original order must remain unchanged.
    CHECK(orderIndex.size() == 1);
    CHECK(orderIndex.contains(1));
    CHECK(bids.at(10000).total_quantity == 10);
    CHECK(bids.at(10000).orders.front().quantity == 10);

    CHECK(tradeHistory.empty());

    checkBookInvariants();
}




void testBuyWithEmptyAskBookRests() {
    resetOrderBook();

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 8);
    CHECK(result.trades.empty());

    CHECK(bids.contains(10000));
    CHECK(bids.at(10000).total_quantity == 8);
    CHECK(bids.at(10000).orders.front().id == 10);
    CHECK(orderIndex.contains(10));

    CHECK(tradeHistory.empty());

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}

void testSellWithEmptyBidBookRests() {
    resetOrderBook();

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10100, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 8);
    CHECK(result.trades.empty());

    CHECK(asks.contains(10100));
    CHECK(asks.at(10100).total_quantity == 8);
    CHECK(asks.at(10100).orders.front().id == 10);
    CHECK(orderIndex.contains(10));

    CHECK(tradeHistory.empty());

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}

void testNonCrossingBuyRests() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10100, 5}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 8);
    CHECK(result.trades.empty());

    // Existing ask remains unchanged.
    CHECK(asks.at(10100).total_quantity == 5);

    // Incoming buy rests.
    CHECK(bids.at(10000).total_quantity == 8);
    CHECK(orderIndex.contains(10));

    CHECK(bestBid().value() == 10000);
    CHECK(bestAsk().value() == 10100);

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}

void testNonCrossingSellRests() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10000, 5}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10100, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 8);
    CHECK(result.trades.empty());

    CHECK(bids.at(10000).total_quantity == 5);
    CHECK(asks.at(10100).total_quantity == 8);

    CHECK(bestBid().value() == 10000);
    CHECK(bestAsk().value() == 10100);

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}



void testExactFullFillIncomingBuy() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 5}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 5}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 5);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 1);

    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        5,
        Side::Buy
    );

    CHECK(asks.empty());
    CHECK(bids.empty());

    // Maker was removed.
    CHECK(!orderIndex.contains(1));

    // Fully executed taker was never added.
    CHECK(!orderIndex.contains(10));

    CHECK(tradeHistory.size() == 1);

    checkExecutionArithmetic(result, 5);
    checkBookInvariants();
}

void testExactFullFillIncomingSell() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10100, 7}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10100, 7}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 7);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 1);

    checkTrade(
        result.trades[0],
        1,
        10,
        10100,
        7,
        Side::Sell
    );

    CHECK(bids.empty());
    CHECK(asks.empty());
    CHECK(orderIndex.empty());

    CHECK(tradeHistory.size() == 1);

    checkExecutionArithmetic(result, 7);
    checkBookInvariants();
}





void testIncomingBuyPartiallyFillsRestingSell() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 10}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10100, 4}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 4);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 1);

    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        4,
        Side::Buy
    );

    // Resting sell remains with quantity 6.
    CHECK(asks.contains(10000));
    CHECK(asks.at(10000).total_quantity == 6);
    CHECK(asks.at(10000).orders.size() == 1);
    CHECK(asks.at(10000).orders.front().id == 1);
    CHECK(asks.at(10000).orders.front().quantity == 6);

    CHECK(orderIndex.contains(1));
    CHECK(orderIndex.at(1).iterator->quantity == 6);

    CHECK(!orderIndex.contains(10));

    checkExecutionArithmetic(result, 4);
    checkBookInvariants();
}

void testIncomingSellPartiallyFillsRestingBuy() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10100, 10}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10000, 4}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 4);
    CHECK(result.remaining_quantity == 0);

    CHECK(bids.at(10100).total_quantity == 6);
    CHECK(bids.at(10100).orders.front().quantity == 6);
    CHECK(orderIndex.at(1).iterator->quantity == 6);

    checkTrade(
        result.trades[0],
        1,
        10,
        10100,
        4,
        Side::Sell
    );

    checkExecutionArithmetic(result, 4);
    checkBookInvariants();
}

void testIncomingBuyLeavesRestingRemainder() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10100, 10}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 4);
    CHECK(result.remaining_quantity == 6);

    // Sell was completely removed.
    CHECK(asks.empty());
    CHECK(!orderIndex.contains(1));

    // Remaining incoming buy rests at its own limit price.
    CHECK(bids.contains(10100));
    CHECK(bids.at(10100).total_quantity == 6);
    CHECK(bids.at(10100).orders.front().id == 10);
    CHECK(bids.at(10100).orders.front().quantity == 6);
    CHECK(orderIndex.contains(10));

    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        4,
        Side::Buy
    );

    checkExecutionArithmetic(result, 10);
    checkBookInvariants();
}

void testIncomingSellLeavesRestingRemainder() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10100, 3}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10000, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 3);
    CHECK(result.remaining_quantity == 5);

    CHECK(bids.empty());

    CHECK(asks.contains(10000));
    CHECK(asks.at(10000).total_quantity == 5);
    CHECK(asks.at(10000).orders.front().id == 10);
    CHECK(asks.at(10000).orders.front().quantity == 5);

    checkTrade(
        result.trades[0],
        1,
        10,
        10100,
        3,
        Side::Sell
    );

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}




void testBuyUsesFifoAtSameAskPrice() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 3}));
    CHECK(addOrder(Order{2, Side::Sell, 10000, 4}));
    CHECK(addOrder(Order{3, Side::Sell, 10000, 5}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 5}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 5);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 2);

    // Oldest order executes first.
    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        3,
        Side::Buy
    );

    // Then the second-oldest order.
    checkTrade(
        result.trades[1],
        2,
        10,
        10000,
        2,
        Side::Buy
    );

    std::vector<OrderId> expectedQueue{2, 3};

    CHECK(
        getQueueIds(asks.at(10000).orders) ==
        expectedQueue
    );

    CHECK(asks.at(10000).orders.front().quantity == 2);
    CHECK(asks.at(10000).total_quantity == 7);

    CHECK(!orderIndex.contains(1));
    CHECK(orderIndex.contains(2));
    CHECK(orderIndex.contains(3));

    checkExecutionArithmetic(result, 5);
    checkBookInvariants();
}

void testSellUsesFifoAtSameBidPrice() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10000, 2}));
    CHECK(addOrder(Order{2, Side::Buy, 10000, 4}));
    CHECK(addOrder(Order{3, Side::Buy, 10000, 6}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10000, 5}
    );

    CHECK(result.trades.size() == 2);

    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        2,
        Side::Sell
    );

    checkTrade(
        result.trades[1],
        2,
        10,
        10000,
        3,
        Side::Sell
    );

    std::vector<OrderId> expectedQueue{2, 3};

    CHECK(
        getQueueIds(bids.at(10000).orders) ==
        expectedQueue
    );

    CHECK(bids.at(10000).orders.front().quantity == 1);
    CHECK(bids.at(10000).total_quantity == 7);

    checkExecutionArithmetic(result, 5);
    checkBookInvariants();
}



void testBuyConsumesMultipleAskLevels() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 9900, 2}));
    CHECK(addOrder(Order{2, Side::Sell, 10000, 3}));
    CHECK(addOrder(Order{3, Side::Sell, 10100, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 6}
    );

    /*
        Matches:
        2 at 9900
        3 at 10000

        Stops before 10100 because buy limit is 10000.

        Remaining quantity 1 rests as a bid at 10000.
    */

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 5);
    CHECK(result.remaining_quantity == 1);
    CHECK(result.trades.size() == 2);

    checkTrade(
        result.trades[0],
        1,
        10,
        9900,
        2,
        Side::Buy
    );

    checkTrade(
        result.trades[1],
        2,
        10,
        10000,
        3,
        Side::Buy
    );

    CHECK(!asks.contains(9900));
    CHECK(!asks.contains(10000));

    CHECK(asks.contains(10100));
    CHECK(asks.at(10100).total_quantity == 4);

    CHECK(bids.contains(10000));
    CHECK(bids.at(10000).total_quantity == 1);
    CHECK(bids.at(10000).orders.front().id == 10);

    CHECK(bestBid().value() == 10000);
    CHECK(bestAsk().value() == 10100);

    checkExecutionArithmetic(result, 6);
    checkBookInvariants();
}

void testSellConsumesMultipleBidLevels() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10300, 2}));
    CHECK(addOrder(Order{2, Side::Buy, 10200, 3}));
    CHECK(addOrder(Order{3, Side::Buy, 10100, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10200, 6}
    );

    /*
        Matches:
        2 at 10300
        3 at 10200

        Stops before 10100 because sell limit is 10200.

        Remaining quantity 1 rests as an ask at 10200.
    */

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 5);
    CHECK(result.remaining_quantity == 1);
    CHECK(result.trades.size() == 2);

    checkTrade(
        result.trades[0],
        1,
        10,
        10300,
        2,
        Side::Sell
    );

    checkTrade(
        result.trades[1],
        2,
        10,
        10200,
        3,
        Side::Sell
    );

    CHECK(!bids.contains(10300));
    CHECK(!bids.contains(10200));

    CHECK(bids.contains(10100));
    CHECK(bids.at(10100).total_quantity == 4);

    CHECK(asks.contains(10200));
    CHECK(asks.at(10200).total_quantity == 1);

    CHECK(bestBid().value() == 10100);
    CHECK(bestAsk().value() == 10200);

    checkExecutionArithmetic(result, 6);
    checkBookInvariants();
}




void testBuyCompletelySweepsSeveralAskLevels() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 2}));
    CHECK(addOrder(Order{2, Side::Sell, 10100, 3}));
    CHECK(addOrder(Order{3, Side::Sell, 10200, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10200, 9}
    );

    CHECK(result.executed_quantity == 9);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 3);

    CHECK(asks.empty());
    CHECK(bids.empty());
    CHECK(orderIndex.empty());

    CHECK(tradeHistory.size() == 3);

    checkExecutionArithmetic(result, 9);
    checkBookInvariants();
}

void testSellCompletelySweepsSeveralBidLevels() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10200, 2}));
    CHECK(addOrder(Order{2, Side::Buy, 10100, 3}));
    CHECK(addOrder(Order{3, Side::Buy, 10000, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10000, 9}
    );

    CHECK(result.executed_quantity == 9);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 3);

    CHECK(bids.empty());
    CHECK(asks.empty());
    CHECK(orderIndex.empty());

    CHECK(tradeHistory.size() == 3);

    checkExecutionArithmetic(result, 9);
    checkBookInvariants();
}




void testExecutionTradesAreSeparateFromGlobalHistory() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 2}));

    ExecutionResult first = executeOrder(
        Order{10, Side::Buy, 10000, 1}
    );

    CHECK(first.trades.size() == 1);
    CHECK(tradeHistory.size() == 1);

    ExecutionResult second = executeOrder(
        Order{11, Side::Buy, 10000, 1}
    );

    // This call created only one trade.
    CHECK(second.trades.size() == 1);

    // Global history contains both calls.
    CHECK(tradeHistory.size() == 2);

    checkTrade(
        first.trades[0],
        1,
        10,
        10000,
        1,
        Side::Buy
    );

    checkTrade(
        second.trades[0],
        1,
        11,
        10000,
        1,
        Side::Buy
    );

    CHECK(asks.empty());
    CHECK(orderIndex.empty());

    checkBookInvariants();
}



void runExecuteOrderTests() {
    testExecuteRejectsZeroQuantity();
    testExecuteRejectsInvalidPrice();
    testExecuteRejectsDuplicateId();

    testBuyWithEmptyAskBookRests();
    testSellWithEmptyBidBookRests();

    testNonCrossingBuyRests();
    testNonCrossingSellRests();

    testExactFullFillIncomingBuy();
    testExactFullFillIncomingSell();

    testIncomingBuyPartiallyFillsRestingSell();
    testIncomingSellPartiallyFillsRestingBuy();

    testIncomingBuyLeavesRestingRemainder();
    testIncomingSellLeavesRestingRemainder();

    testBuyUsesFifoAtSameAskPrice();
    testSellUsesFifoAtSameBidPrice();

    testBuyConsumesMultipleAskLevels();
    testSellConsumesMultipleBidLevels();

    testBuyCompletelySweepsSeveralAskLevels();
    testSellCompletelySweepsSeveralBidLevels();

    testExecutionTradesAreSeparateFromGlobalHistory();

    std::cout
        << "All executeOrder edge-case tests passed!\n";
}



std::filesystem::path getTestFilePath(
    const std::string& fileName
) {
    return std::filesystem::temp_directory_path() /
           fileName;
}

void testBinaryWriterAndParserRoundTrip() {
    std::filesystem::path filePath =
        getTestFilePath(
            "order_book_round_trip.bin"
        );

    std::vector<MarketEvent> originalEvents{
        makeAddEvent(
            1,
            1000,
            Order{
     1,
     Side::Sell,
     10000,
     10,
     6001,
     DefaultSymbol
 }
        ),
        makeExecuteEvent(
            2,
            1100,
            Order{
    10,
    Side::Buy,
    10000,
    4,
    6002,
    DefaultSymbol
}
        ),
        makeModifyEvent(
            3,
            1200,
            1,
            10100,
            8
        ),
        makeCancelEvent(
            4,
            1300,
            1
        )
    };

    std::string writeError;

    CHECK(
        BinaryEventWriter::write(
            filePath,
            originalEvents,
            writeError
        )
    );

    ParseResult parseResult =
        BinaryEventParser::parse(filePath);

    CHECK(parseResult.success);
    CHECK(parseResult.errors.empty());

    CHECK(
        parseResult.statistics.records_declared ==
        originalEvents.size()
    );

    CHECK(
        parseResult.statistics.valid_records ==
        originalEvents.size()
    );

    CHECK(
        parseResult.events ==
        originalEvents
    );

    std::filesystem::remove(filePath);
}

void testDeterministicReplay() {
    std::vector<MarketEvent> events{
        makeAddEvent(
            1,
            1000,
            Order{1, Side::Sell, 10000, 5}
        ),

        makeExecuteEvent(
            2,
            1100,
            Order{10, Side::Buy, 10000, 3}
        ),

        makeCancelEvent(
            3,
            1200,
            1
        ),

        makeAddEvent(
            4,
            1300,
            Order{2, Side::Buy, 9900, 4}
        ),

        makeModifyEvent(
            5,
            1400,
            2,
            9950,
            6
        )
    };

    ReplayEngine replayEngine;

    ReplayResult firstReplay =
        replayEngine.replay(events);

    CHECK(firstReplay.completed);
    CHECK(firstReplay.statistics.total_events == 5);
    CHECK(firstReplay.statistics.applied_events == 5);
    CHECK(firstReplay.statistics.rejected_events == 0);

    CHECK(firstReplay.statistics.add_events == 2);
    CHECK(firstReplay.statistics.execute_events == 1);
    CHECK(firstReplay.statistics.cancel_events == 1);
    CHECK(firstReplay.statistics.modify_events == 1);

    CHECK(firstReplay.statistics.trades_generated == 1);
    CHECK(firstReplay.statistics.executed_quantity == 3);

    CHECK(firstReplay.statistics.sequence_gap_events == 0);
    CHECK(firstReplay.statistics.missing_sequences == 0);

    CHECK(bids.contains(9950));
    CHECK(bids.at(9950).total_quantity == 6);
    CHECK(bids.at(9950).orders.front().id == 2);

    CHECK(asks.empty());
    CHECK(tradeHistory.size() == 1);

    std::string firstSnapshot =
        createOrderBookSnapshot();

    ReplayResult secondReplay =
        replayEngine.replay(events);

    CHECK(secondReplay.completed);
    CHECK(secondReplay.statistics.applied_events == 5);

    std::string secondSnapshot =
        createOrderBookSnapshot();

    // Same input must produce exactly the same book state.
    CHECK(firstSnapshot == secondSnapshot);

    checkBookInvariants();
}

void testReplayDetectsSequenceGap() {
    std::vector<MarketEvent> events{
        makeAddEvent(
            1,
            1000,
            Order{1, Side::Buy, 9900, 4}
        ),

        // Sequence 2 is missing.
        makeAddEvent(
            3,
            1100,
            Order{2, Side::Sell, 10100, 5}
        )
    };

    ReplayEngine replayEngine;

    ReplayResult result =
        replayEngine.replay(events);

    CHECK(!result.completed);

    CHECK(
        result.statistics.sequence_gap_events ==
        1
    );

    CHECK(
        result.statistics.missing_sequences ==
        1
    );

    CHECK(
        result.statistics.applied_events ==
        1
    );

    CHECK(!result.errors.empty());

    CHECK(bids.contains(9900));
    CHECK(!asks.contains(10100));

    checkBookInvariants();
}

void testReplayStopsOnSequenceGap() {
    std::vector<MarketEvent> events{
        makeAddEvent(
            1,
            1000,
            Order{1, Side::Buy, 9900, 4}
        ),

        makeAddEvent(
            3,
            1100,
            Order{2, Side::Sell, 10100, 5}
        ),

        makeCancelEvent(
            4,
            1200,
            1
        )
    };

    ReplayOptions options;

    ReplayEngine replayEngine;

    ReplayResult result =
        replayEngine.replay(events, options);

    CHECK(!result.completed);

    // First event applied; replay stops before event 3.
    CHECK(result.statistics.applied_events == 1);

    CHECK(orderIndex.contains(1));
    CHECK(!orderIndex.contains(2));

    checkBookInvariants();
}

void testReplayDetectsDuplicateSequence() {
    std::vector<MarketEvent> events{
        makeAddEvent(
            1,
            1000,
            Order{1, Side::Buy, 9900, 4}
        ),

        makeAddEvent(
            2,
            1100,
            Order{2, Side::Sell, 10100, 5}
        ),

        // Duplicate sequence number.
        makeCancelEvent(
            2,
            1200,
            1
        )
    };

    ReplayEngine replayEngine;

    ReplayResult result =
        replayEngine.replay(events);

    CHECK(
        result.statistics
            .duplicate_or_out_of_order_sequences ==
        1
    );

    CHECK(!result.errors.empty());

    checkBookInvariants();
}

void testParserDetectsCorruptChecksum() {
    std::filesystem::path filePath =
        getTestFilePath(
            "order_book_corrupt_checksum.bin"
        );

    std::vector<MarketEvent> events{
        makeAddEvent(
            1,
            1000,
            Order{1, Side::Buy, 10000, 5}
        )
    };

    std::string writeError;

    CHECK(
        BinaryEventWriter::write(
            filePath,
            events,
            writeError
        )
    );

    {
        std::fstream file(
            filePath,
            std::ios::binary |
            std::ios::in |
            std::ios::out
        );

        CHECK(file.is_open());

        std::streamoff corruptionPosition =
            BinaryFormat::FileHeaderSize + 20;

        file.seekg(corruptionPosition);

        char byte = 0;
        file.read(&byte, 1);

        CHECK(file.good());

        byte ^= static_cast<char>(0xFF);

        file.seekp(corruptionPosition);
        file.write(&byte, 1);
    }

    ParseResult result =
        BinaryEventParser::parse(filePath);

    CHECK(!result.success);
    CHECK(result.events.empty());

    CHECK(
        result.statistics.corrupt_records ==
        1
    );

    CHECK(!result.errors.empty());

    std::filesystem::remove(filePath);
}

void testParserDetectsTruncatedRecord() {
    std::filesystem::path filePath =
        getTestFilePath(
            "order_book_truncated_record.bin"
        );

    std::vector<MarketEvent> events{
        makeAddEvent(
            1,
            1000,
            Order{1, Side::Buy, 10000, 5}
        )
    };

    std::string writeError;

    CHECK(
        BinaryEventWriter::write(
            filePath,
            events,
            writeError
        )
    );

    std::uintmax_t originalFileSize =
        std::filesystem::file_size(filePath);

    CHECK(originalFileSize > 5);

    std::filesystem::resize_file(
        filePath,
        originalFileSize - 5
    );

    ParseResult result =
        BinaryEventParser::parse(filePath);

    CHECK(!result.success);

    CHECK(
        result.statistics.truncated_records ==
        1
    );

    CHECK(!result.errors.empty());

    std::filesystem::remove(filePath);
}

void testReplayRejectsUnknownOrderCancellation() {
    std::vector<MarketEvent> events{
        makeCancelEvent(
            1,
            1000,
            999
        )
    };

    ReplayEngine replayEngine;

    ReplayResult result =
        replayEngine.replay(events);

    CHECK(result.completed);
    CHECK(result.statistics.applied_events == 0);
    CHECK(result.statistics.rejected_events == 1);
    CHECK(!result.errors.empty());

    CHECK(bids.empty());
    CHECK(asks.empty());
    CHECK(orderIndex.empty());
}

void testCompleteBinaryReplayPipeline() {
    std::filesystem::path filePath =
        getTestFilePath(
            "complete_replay_pipeline.bin"
        );

    std::vector<MarketEvent> events{
        makeAddEvent(
            1,
            1000,
            Order{1, Side::Sell, 10000, 3}
        ),

        makeAddEvent(
            2,
            1100,
            Order{2, Side::Sell, 10100, 4}
        ),

        makeExecuteEvent(
            3,
            1200,
            Order{10, Side::Buy, 10100, 5}
        ),

        makeAddEvent(
            4,
            1300,
            Order{3, Side::Buy, 9900, 8}
        ),

        makeModifyEvent(
            5,
            1400,
            3,
            9950,
            10
        )
    };

    std::string writeError;

    CHECK(
        BinaryEventWriter::write(
            filePath,
            events,
            writeError
        )
    );

    ParseResult parseResult =
        BinaryEventParser::parse(filePath);

    CHECK(parseResult.success);
    CHECK(parseResult.events.size() == 5);

    ReplayEngine replayEngine;

    ReplayResult replayResult =
        replayEngine.replay(parseResult.events);

    CHECK(replayResult.completed);
    CHECK(replayResult.statistics.applied_events == 5);

    /*
        Incoming buy quantity 5:

        3 execute against order 1 at 10000
        2 execute against order 2 at 10100

        Order 2 remains with quantity 2.
    */

    CHECK(tradeHistory.size() == 2);

    CHECK(asks.contains(10100));
    CHECK(asks.at(10100).total_quantity == 2);
    CHECK(asks.at(10100).orders.front().id == 2);

    CHECK(bids.contains(9950));
    CHECK(bids.at(9950).total_quantity == 10);
    CHECK(bids.at(9950).orders.front().id == 3);

    CHECK(
        replayResult.statistics.executed_quantity ==
        5
    );

    CHECK(
        replayResult.statistics.trades_generated ==
        2
    );

    checkBookInvariants();

    std::filesystem::remove(filePath);
}

void testLobsterPartialCancellationKeepsPriority() {
    std::filesystem::path filePath =
        getTestFilePath(
            "lobster_fifo_test.csv"
        );

    /*
        Create a tiny controlled LOBSTER message file:

        1. Order 1 joins the bid queue.
        2. Order 2 joins behind Order 1.
        3. Order 1 is partially cancelled.

        Reducing Order 1's quantity must not move it
        behind Order 2.
    */
    {
        std::ofstream output{
            filePath
        };

        CHECK(output.is_open());

        output
            << "34200,1,1,10,10000,1\n"
            << "34200.1,1,2,10,10000,1\n"
            << "34200.2,2,1,2,10000,1\n";
    }

    std::vector<MarketEvent> events =
    loadLobsterMessageFile(
        filePath
    );

    ReplayEngine replayEngine;

    ReplayOptions options;
    options.reset_book_before_replay = true;
    options.stop_on_rejection = true;

    ReplayResult result =
        replayEngine.replay(
            events,
            options
        );

    CHECK(result.completed);

    CHECK(
        result.statistics.rejected_events ==
        0
    );

    CHECK(
        bids.contains(10000)
    );

    std::vector<OrderId> expectedIds{
        1,
        2
    };

    CHECK(
        getQueueIds(
            bids.at(10000).orders
        ) ==
        expectedIds
    );

    CHECK(
        bids.at(10000)
            .orders
            .front()
            .quantity ==
        8
    );

    CHECK(
        bids.at(10000)
            .total_quantity ==
        18
    );

    checkBookInvariants();

    std::filesystem::remove(
        filePath
    );
}

void runFeedReplayTests() {
    testBinaryWriterAndParserRoundTrip();
    testDeterministicReplay();

    testReplayDetectsSequenceGap();
    testReplayStopsOnSequenceGap();
    testReplayDetectsDuplicateSequence();

    testParserDetectsCorruptChecksum();
    testParserDetectsTruncatedRecord();
    testReplayRejectsUnknownOrderCancellation();
    testCompleteBinaryReplayPipeline();

    testLobsterPartialCancellationKeepsPriority();

    std::cout
        << "All feed and replay tests passed!\n";
}

void addBasicExternalMarket() {
    CHECK(
        addOrder(
            Order{
                1,
                Side::Buy,
                9900,
                100
            }
        )
    );

    CHECK(
        addOrder(
            Order{
                2,
                Side::Sell,
                10100,
                100
            }
        )
    );
}

MarketMakerConfig makeTestStrategyConfig() {
    MarketMakerConfig config;

    config.quote_quantity = 10;
    config.half_spread_ticks = 20;
    config.inventory_skew_per_unit = 2;
    config.target_position = 0;
    config.tick_size = 1;
    config.first_strategy_order_id =
        1'000'000;

    return config;
}

RiskLimits makeTestRiskLimits() {
    RiskLimits limits;

    limits.maximum_order_size = 20;
    limits.maximum_absolute_position = 100;
    limits.maximum_notional_exposure =
        10'000'000.0L;

    limits.maximum_market_age_ns = 1000;

    return limits;
}

void testMarketMakerGeneratesBidAndAsk() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerStrategy strategy(
        makeTestStrategyConfig(),
        makeTestRiskLimits()
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult result =
        strategy.refreshQuotes(1000);

    CHECK(result.market_accepted);

    CHECK(result.fair_price == 10000);
    CHECK(result.inventory_skew == 0);

    CHECK(result.bid.accepted);
    CHECK(result.ask.accepted);

    CHECK(result.bid.price == 9980);
    CHECK(result.ask.price == 10020);

    CHECK(
        result.bid.order_id.has_value()
    );

    CHECK(
        result.ask.order_id.has_value()
    );

    CHECK(
        orderIndex.contains(
            result.bid.order_id.value()
        )
    );

    CHECK(
        orderIndex.contains(
            result.ask.order_id.value()
        )
    );

    CHECK(strategy.inventory().position() == 0);

    checkBookInvariants();
}

void testInventoryChangesQuotePrices() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerStrategy strategy(
        makeTestStrategyConfig(),
        makeTestRiskLimits()
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult firstQuotes =
        strategy.refreshQuotes(1000);

    CHECK(firstQuotes.bid.accepted);
    CHECK(firstQuotes.bid.price == 9980);

    /*
        External seller executes against our bid.

        Our strategy bought 10 units.
    */
    ExecutionResult externalExecution =
        executeOrder(
            Order{
                100,
                Side::Sell,
                firstQuotes.bid.price,
                10
            }
        );

    CHECK(externalExecution.accepted);
    CHECK(
        externalExecution.executed_quantity ==
        10
    );

    strategy.processTrades();

    CHECK(strategy.inventory().position() == 10);
    CHECK(strategy.inventory().totalBought() == 10);

    strategy.onMarketUpdate(1100);

    QuoteRefreshResult secondQuotes =
        strategy.refreshQuotes(1100);

    /*
        Position = +10
        Skew per unit = 2

        Inventory skew = 20 ticks.

        Original fair value = 10000
        Adjusted fair value = 9980

        New bid = 9960
        New ask = 10000
    */

    CHECK(secondQuotes.market_accepted);
    CHECK(secondQuotes.inventory_skew == 20);

    CHECK(secondQuotes.bid.price == 9960);
    CHECK(secondQuotes.ask.price == 10000);

    checkBookInvariants();
}

void testMaximumOrderSizeRejectsQuotes() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerConfig config =
        makeTestStrategyConfig();

    config.quote_quantity = 10;

    RiskLimits limits =
        makeTestRiskLimits();

    limits.maximum_order_size = 5;

    MarketMakerStrategy strategy(
        config,
        limits
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult result =
        strategy.refreshQuotes(1000);

    CHECK(result.market_accepted);

    CHECK(!result.bid.accepted);
    CHECK(!result.ask.accepted);

    CHECK(
        strategy.rejectionReporter().count() ==
        2
    );

    CHECK(!strategy.activeBidOrderId().has_value());
    CHECK(!strategy.activeAskOrderId().has_value());

    checkBookInvariants();
}


void testMaximumPositionRejectsQuotes() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerConfig config =
        makeTestStrategyConfig();

    config.quote_quantity = 10;

    RiskLimits limits =
        makeTestRiskLimits();

    limits.maximum_absolute_position = 5;

    MarketMakerStrategy strategy(
        config,
        limits
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult result =
        strategy.refreshQuotes(1000);

    CHECK(result.market_accepted);

    /*
        Buying 10 would produce position +10.
        Selling 10 would produce position -10.

        Both exceed maximum absolute position 5.
    */
    CHECK(!result.bid.accepted);
    CHECK(!result.ask.accepted);

    CHECK(
        strategy.rejectionReporter().count() ==
        2
    );

    checkBookInvariants();
}


void testMaximumNotionalRejectsQuotes() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerConfig config =
        makeTestStrategyConfig();

    config.quote_quantity = 10;

    RiskLimits limits =
        makeTestRiskLimits();

    /*
        Approximate projected notional:

        10 × 10,000 = 100,000
    */
    limits.maximum_notional_exposure =
        50'000.0L;

    MarketMakerStrategy strategy(
        config,
        limits
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult result =
        strategy.refreshQuotes(1000);

    CHECK(result.market_accepted);

    CHECK(!result.bid.accepted);
    CHECK(!result.ask.accepted);

    CHECK(
        strategy.rejectionReporter().count() ==
        2
    );

    checkBookInvariants();
}


void testStaleMarketPreventsQuoting() {
    resetOrderBook();
    addBasicExternalMarket();

    RiskLimits limits =
        makeTestRiskLimits();

    limits.maximum_market_age_ns = 100;

    MarketMakerStrategy strategy(
        makeTestStrategyConfig(),
        limits
    );

    strategy.onMarketUpdate(1000);

    /*
        Current time is 1200.

        Market age = 200 ns.
        Maximum permitted age = 100 ns.
    */
    QuoteRefreshResult result =
        strategy.refreshQuotes(1200);

    CHECK(!result.market_accepted);

    CHECK(!result.bid.accepted);
    CHECK(!result.ask.accepted);

    CHECK(
        strategy.rejectionReporter().count() ==
        1
    );

    CHECK(!strategy.activeBidOrderId().has_value());
    CHECK(!strategy.activeAskOrderId().has_value());

    checkBookInvariants();
}


void testKillSwitchCancelsQuotes() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerStrategy strategy(
        makeTestStrategyConfig(),
        makeTestRiskLimits()
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult initialQuotes =
        strategy.refreshQuotes(1000);

    CHECK(initialQuotes.bid.accepted);
    CHECK(initialQuotes.ask.accepted);

    OrderId bidId =
        initialQuotes.bid.order_id.value();

    OrderId askId =
        initialQuotes.ask.order_id.value();

    CHECK(orderIndex.contains(bidId));
    CHECK(orderIndex.contains(askId));

    strategy.activateKillSwitch(
        1050,
        "Manual emergency stop"
    );

    CHECK(
        strategy.riskManager()
            .killSwitchActive()
    );

    CHECK(!orderIndex.contains(bidId));
    CHECK(!orderIndex.contains(askId));

    CHECK(!strategy.activeBidOrderId().has_value());
    CHECK(!strategy.activeAskOrderId().has_value());

    strategy.onMarketUpdate(1100);

    QuoteRefreshResult blockedQuotes =
        strategy.refreshQuotes(1100);

    CHECK(!blockedQuotes.market_accepted);
    CHECK(!blockedQuotes.bid.accepted);
    CHECK(!blockedQuotes.ask.accepted);

    checkBookInvariants();
}

void testRejectedOrderReporting() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerConfig config =
        makeTestStrategyConfig();

    config.quote_quantity = 50;

    RiskLimits limits =
        makeTestRiskLimits();

    limits.maximum_order_size = 10;

    MarketMakerStrategy strategy(
        config,
        limits
    );

    strategy.onMarketUpdate(1000);
    strategy.refreshQuotes(1000);

    const auto& records =
        strategy.rejectionReporter().records();

    CHECK(records.size() == 2);

    CHECK(
        records[0].reason ==
        RejectionReason::
            MaximumOrderSizeExceeded
    );

    CHECK(records[0].order.has_value());

    CHECK(
        records[0].order->quantity ==
        50
    );

    CHECK(
        records[1].reason ==
        RejectionReason::
            MaximumOrderSizeExceeded
    );
}

void runStrategyRiskTests() {
    testMarketMakerGeneratesBidAndAsk();
    testInventoryChangesQuotePrices();

    testMaximumOrderSizeRejectsQuotes();
    testMaximumPositionRejectsQuotes();
    testMaximumNotionalRejectsQuotes();

    testStaleMarketPreventsQuoting();
    testKillSwitchCancelsQuotes();

    testRejectedOrderReporting();

    std::cout
        << "All strategy and risk tests passed!\n";
}
#define CHECK_NEAR(actual, expected, tolerance)                         \
do {                                                                    \
long double actualValue =                                           \
static_cast<long double>(actual);                               \
\
long double expectedValue =                                         \
static_cast<long double>(expected);                             \
\
if (std::fabs(actualValue - expectedValue) > tolerance) {           \
std::cerr                                                       \
<< "CHECK_NEAR failed at line "                             \
<< __LINE__                                                 \
<< ": actual="                                              \
<< static_cast<double>(actualValue)                          \
<< ", expected="                                            \
<< static_cast<double>(expectedValue)                        \
<< '\n';                                                    \
\
std::exit(EXIT_FAILURE);                                         \
}                                                                   \
} while (false)

void testLongPositionPnl() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;
    constexpr SymbolId Symbol = 1;

    pnl.registerAccount(Trader);
    pnl.setSymbolName(Symbol, "TEST");

    /*
        Trader buys 10 at 100.

        Participant 0 represents an external/unknown
        counterparty.
    */
    Trade buyTrade{
        1,
        2,
        100,
        10,
        Side::Buy,
        UnknownParticipant,
        Trader,
        Symbol
    };

    CHECK(pnl.processTrade(buyTrade));

    CHECK(pnl.position(Trader, Symbol) == 10);

    CHECK_NEAR(
        pnl.averageEntryPrice(
            Trader,
            Symbol
        ),
        100.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Trader),
        -1000.0L,
        0.0001L
    );

    CHECK(
        pnl.setMarkPrice(
            Symbol,
            110.0L
        )
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            Symbol
        ),
        100.0L,
        0.0001L
    );

    /*
        Trader then sells 4 at 120.

        Profit on closed quantity:

        (120 - 100) × 4 = 80
    */
    Trade sellTrade{
        3,
        4,
        120,
        4,
        Side::Sell,
        UnknownParticipant,
        Trader,
        Symbol
    };

    CHECK(pnl.processTrade(sellTrade));

    CHECK(pnl.position(Trader, Symbol) == 6);

    CHECK_NEAR(
        pnl.realisedPnl(Trader),
        80.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            Symbol
        ),
        60.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Trader),
        140.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Trader),
        -520.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.accountEquity(Trader),
        140.0L,
        0.0001L
    );
}

void testWeightedAverageEntryPrice() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    pnl.processTrade(
        Trade{
            1,
            10,
            100,
            10,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    pnl.processTrade(
        Trade{
            2,
            11,
            120,
            10,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    CHECK(
        pnl.position(Trader, 1) ==
        20
    );

    CHECK_NEAR(
        pnl.averageEntryPrice(
            Trader,
            1
        ),
        110.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Trader),
        -2200.0L,
        0.0001L
    );
}


void testShortPositionPnl() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    /*
        Trader sells short 8 at 200.
    */
    pnl.processTrade(
        Trade{
            1,
            10,
            200,
            8,
            Side::Sell,
            UnknownParticipant,
            Trader,
            1
        }
    );

    CHECK(
        pnl.position(Trader, 1) ==
        -8
    );

    CHECK_NEAR(
        pnl.averageEntryPrice(
            Trader,
            1
        ),
        200.0L,
        0.0001L
    );

    /*
        Trader buys back 3 at 180.

        Profit:

        (200 - 180) × 3 = 60
    */
    pnl.processTrade(
        Trade{
            2,
            11,
            180,
            3,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    CHECK(
        pnl.position(Trader, 1) ==
        -5
    );

    CHECK_NEAR(
        pnl.realisedPnl(Trader),
        60.0L,
        0.0001L
    );

    pnl.setMarkPrice(1, 190.0L);

    /*
        Remaining short profit:

        (200 - 190) × 5 = 50
    */
    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            1
        ),
        50.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Trader),
        110.0L,
        0.0001L
    );
}




void testPositionFlip() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    pnl.processTrade(
        Trade{
            1,
            10,
            100,
            5,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    /*
        Sell 8:

        5 closes the existing long.
        3 opens a new short at 120.
    */
    pnl.processTrade(
        Trade{
            2,
            11,
            120,
            8,
            Side::Sell,
            UnknownParticipant,
            Trader,
            1
        }
    );

    CHECK(
        pnl.position(Trader, 1) ==
        -3
    );

    CHECK_NEAR(
        pnl.averageEntryPrice(
            Trader,
            1
        ),
        120.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.realisedPnl(Trader),
        100.0L,
        0.0001L
    );
}



void testMakerAndTakerAccounts() {
    PnlEngine pnl;

    constexpr ParticipantId Seller = 5001;
    constexpr ParticipantId Buyer = 5002;

    Trade trade{
        1,
        2,
        100,
        5,
        Side::Buy,
        Seller,
        Buyer,
        1
    };

    pnl.processTrade(trade);

    /*
        Taker aggressor was Buy.

        Therefore:
        maker sold;
        taker bought.
    */
    CHECK(
        pnl.position(Seller, 1) ==
        -5
    );

    CHECK(
        pnl.position(Buyer, 1) ==
        5
    );

    CHECK_NEAR(
        pnl.cashBalance(Seller),
        500.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Buyer),
        -500.0L,
        0.0001L
    );

    pnl.setMarkPrice(1, 100.0L);

    CHECK_NEAR(
        pnl.totalPnl(Seller),
        0.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Buyer),
        0.0L,
        0.0001L
    );
}

void testPerSymbolPnl() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    pnl.setSymbolName(1, "ALPHA");
    pnl.setSymbolName(2, "BETA");

    pnl.processTrade(
        Trade{
            1,
            10,
            100,
            10,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    pnl.processTrade(
        Trade{
            2,
            11,
            200,
            4,
            Side::Sell,
            UnknownParticipant,
            Trader,
            2
        }
    );

    pnl.setMarkPrice(1, 110.0L);
    pnl.setMarkPrice(2, 180.0L);

    CHECK(
        pnl.position(Trader, 1) ==
        10
    );

    CHECK(
        pnl.position(Trader, 2) ==
        -4
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            1
        ),
        100.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            2
        ),
        80.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Trader),
        180.0L,
        0.0001L
    );
}

void testPnlFromOrderBookTrades() {
    resetOrderBook();

    constexpr ParticipantId Buyer = 5001;
    constexpr ParticipantId Seller = 5002;

    /*
        Seller places a resting ask.
    */
    CHECK(
        addOrder(
            Order{
                1,
                Side::Sell,
                10000,
                10,
                Seller,
                1
            }
        )
    );

    /*
        Buyer submits an aggressive buy.
    */
    ExecutionResult execution =
        executeOrder(
            Order{
                2,
                Side::Buy,
                10000,
                4,
                Buyer,
                1
            }
        );

    CHECK(execution.accepted);
    CHECK(execution.executed_quantity == 4);
    CHECK(tradeHistory.size() == 1);

    CHECK(
        tradeHistory[0]
            .maker_participant_id ==
        Seller
    );

    CHECK(
        tradeHistory[0]
            .taker_participant_id ==
        Buyer
    );

    PnlEngine pnl;

    CHECK(
        pnl.processNewTrades(
            tradeHistory
        )
    );

    CHECK(
        pnl.position(Buyer, 1) ==
        4
    );

    CHECK(
        pnl.position(Seller, 1) ==
        -4
    );

    CHECK_NEAR(
        pnl.cashBalance(Buyer),
        -40000.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Seller),
        40000.0L,
        0.0001L
    );

    pnl.setMarkPrice(1, 10100.0L);

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Buyer,
            1
        ),
        400.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Seller,
            1
        ),
        -400.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Buyer) +
        pnl.totalPnl(Seller),
        0.0L,
        0.0001L
    );
}

void testPnlSummaryReport() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    pnl.registerAccount(
        Trader,
        1'000'000.0L
    );

    pnl.setSymbolName(
        1,
        "TEST"
    );

    pnl.processTrade(
        Trade{
            1,
            10,
            10000,
            10,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    pnl.setMarkPrice(
        1,
        10100.0L
    );

    PnlReportConfig config;

    /*
        Prices and cash are stored in pence.
        Divide by 100 to display pounds.
    */
    config.monetary_scale = 100.0L;
    config.currency_prefix = "£";
    config.decimal_places = 2;

    std::string report =
        pnl.createSummaryReport(config);

    CHECK(
        report.find(
            "Participant 5001"
        ) != std::string::npos
    );

    CHECK(
        report.find(
            "Symbol: TEST"
        ) != std::string::npos
    );

    CHECK(
        report.find(
            "Position: 10"
        ) != std::string::npos
    );

    CHECK(
        report.find(
            "Total P&L"
        ) != std::string::npos
    );
}

void runPnlTests() {
    testLongPositionPnl();
    testWeightedAverageEntryPrice();
    testShortPositionPnl();
    testPositionFlip();

    testMakerAndTakerAccounts();
    testPerSymbolPnl();

    testPnlFromOrderBookTrades();
    testPnlSummaryReport();

    std::cout
        << "All P&L tests passed!\n";
}

void runStrategyPnlDemo() {
    // Start a completely new simulated trading session.
    resetOrderBook();

    analytics::TelemetryRecorder telemetry{
        "results/telemetry"
    };

    /*
        Enables the ScopedLatency timers only for this
        simulation, not for the unit tests.
    */


    analytics::SimulationTelemetryBridge
        analyticsBridge{
            telemetry
        };

    // --------------------------------------------------------
    // 1. Configure the market-making strategy
    // --------------------------------------------------------

    MarketMakerConfig strategyConfig;

    strategyConfig.quote_quantity = 10;
    strategyConfig.half_spread_ticks = 20;
    strategyConfig.inventory_skew_per_unit = 2;
    strategyConfig.target_position = 0;
    strategyConfig.tick_size = 1;

    strategyConfig.first_strategy_order_id =
        1'000'000;

    strategyConfig.participant_id = 5001;
    strategyConfig.symbol_id = DefaultSymbol;

    RiskLimits riskLimits;

    riskLimits.maximum_order_size = 20;
    riskLimits.maximum_absolute_position = 100;

    riskLimits.maximum_notional_exposure =
        10'000'000.0L;

    riskLimits.maximum_market_age_ns =
        1'000'000'000ULL;

    MarketMakerStrategy strategy{
        strategyConfig,
        riskLimits
    };

    // --------------------------------------------------------
    // 2. Create one persistent P&L engine
    // --------------------------------------------------------

    PnlEngine pnl;

    pnl.registerAccount(
        5001,
        1'000'000.0L
    );

    pnl.setSymbolName(
        DefaultSymbol,
        "SIMULATED_MARKET"
    );

    // --------------------------------------------------------
    // 3. Add an external market
    // --------------------------------------------------------

    constexpr ParticipantId ExternalBuyer = 6001;
    constexpr ParticipantId ExternalSeller = 6002;

    CHECK(
        addOrder(
            Order{
                1,
                Side::Buy,
                9900,
                100,
                ExternalBuyer,
                DefaultSymbol
            }
        )
    );

    CHECK(
        addOrder(
            Order{
                2,
                Side::Sell,
                10100,
                100,
                ExternalSeller,
                DefaultSymbol
            }
        )
    );

    // Inform the strategy that market data arrived.
    TimestampNs currentTimestamp = 1000;

    strategy.onMarketUpdate(
        currentTimestamp
    );

    // --------------------------------------------------------
    // 4. Generate the strategy's bid and ask
    // --------------------------------------------------------

    QuoteRefreshResult quotes =
        strategy.refreshQuotes(
            currentTimestamp
        );

    analyticsBridge.recordQuoteRefresh(
    currentTimestamp,
    quotes
);
    analyticsBridge.capture(
    currentTimestamp,
    strategyConfig.participant_id,
    strategyConfig.symbol_id,
    pnl,
    strategy,
    nullptr,
    nullptr,
    20
);

    std::cout
        << "\nStrategy bid: "
        << quotes.bid.price
        << '\n';

    std::cout
        << "Strategy ask: "
        << quotes.ask.price
        << '\n';

    // --------------------------------------------------------
    // 5. Simulate an external seller hitting our bid
    // --------------------------------------------------------
    currentTimestamp = 1100;
    if (
        quotes.bid.accepted &&
        quotes.bid.order_id.has_value()
    ) {
        ExecutionResult execution =
            executeOrder(
                Order{
                    100,
                    Side::Sell,
                    quotes.bid.price,
                    10,
                    ExternalSeller,
                    DefaultSymbol
                }
            );

        CHECK(execution.accepted);
    }

    // --------------------------------------------------------
    // 6. Send newly generated trades to inventory and P&L
    // --------------------------------------------------------

    strategy.processTrades();

    CHECK(
        pnl.processNewTrades(
            tradeHistory
        )
    );

    // --------------------------------------------------------
    // 7. Mark the position using the current order book
    // --------------------------------------------------------

    CHECK(
        pnl.markFromCurrentOrderBook(
            DefaultSymbol
        )
    );

    analyticsBridge.capture(
    currentTimestamp,
    strategyConfig.participant_id,
    strategyConfig.symbol_id,
    pnl,
    strategy,
    nullptr,
    nullptr,
    20
);

    // --------------------------------------------------------
    // 8. Produce the final report
    // --------------------------------------------------------

    PnlReportConfig reportConfig;

    /*
        Prices are stored in pence, so divide monetary
        values by 100 when displaying pounds.
    */
    reportConfig.monetary_scale = 100.0L;
    reportConfig.currency_prefix = "GBP ";
    reportConfig.decimal_places = 2;

    std::cout
    << '\n'
    << pnl.createSummaryReport(
        reportConfig
    );

    telemetry.flush();



    std::cout
        << "\nTelemetry written to: "
        << std::filesystem::absolute(
               "results/telemetry"
           )
        << '\n';
}

void testRecoveryRestoresMissingEvents() {
    std::vector<MarketEvent> completeFeed{
        makeAddEvent(
            1,
            1000,
            Order{
                1,
                Side::Buy,
                9900,
                10,
                6001,
                DefaultSymbol
            }
        ),

        makeAddEvent(
            2,
            1100,
            Order{
                2,
                Side::Sell,
                10100,
                10,
                6002,
                DefaultSymbol
            }
        ),

        makeModifyEvent(
            3,
            1200,
            1,
            9950,
            12,
            6001,
            DefaultSymbol
        )
    };

    // Produce the expected clean final state.
    resetOrderBook();

    for (const MarketEvent& event : completeFeed) {
        CHECK(
            applyMarketEvent(event).accepted
        );
    }

    std::string expectedSnapshot =
        createOrderBookSnapshot();

    // Simulate UDP loss: event 2 is missing.
    resetOrderBook();

    InMemoryRecoverySource recoverySource{
        completeFeed
    };

    RecoverySequencer sequencer{
        1,
        64,
        recoverySource
    };

    CHECK(sequencer.onEvent(completeFeed[0]));

    // Event 3 arrives while event 2 is missing.
    CHECK(sequencer.onEvent(completeFeed[2]));

    CHECK(
        sequencer.state() ==
        FeedState::Recovering
    );

    CHECK(sequencer.retryRecovery());

    CHECK(sequencer.healthy());
    CHECK(sequencer.expectedSequence() == 4);

    CHECK(
        sequencer.statistics()
            .events_recovered ==
        1
    );

    CHECK(
        createOrderBookSnapshot() ==
        expectedSnapshot
    );

    checkBookInvariants();
}

void testOutOfOrderArrivalWithoutRecovery() {
    std::vector<MarketEvent> events{
        makeAddEvent(
            1,
            1000,
            Order{1, Side::Buy, 9900, 10}
        ),

        makeAddEvent(
            2,
            1100,
            Order{2, Side::Sell, 10100, 10}
        ),

        makeModifyEvent(
            3,
            1200,
            1,
            9950,
            12
        )
    };

    resetOrderBook();

    InMemoryRecoverySource emptySource{
            {}
    };

    RecoverySequencer sequencer{
        1,
        64,
        emptySource
    };

    CHECK(sequencer.onEvent(events[0]));

    // Sequence 3 is buffered.
    CHECK(sequencer.onEvent(events[2]));

    CHECK(
        sequencer.state() ==
        FeedState::Recovering
    );

    CHECK(sequencer.bufferedEventCount() == 1);

    // Sequence 2 later arrives over UDP.
    CHECK(sequencer.onEvent(events[1]));

    CHECK(sequencer.healthy());
    CHECK(sequencer.expectedSequence() == 4);

    CHECK(bids.contains(9950));
    CHECK(asks.contains(10100));

    checkBookInvariants();
}
void testUdpProcessingLoopRecoversGap() {
    resetOrderBook();

    std::vector<MarketEvent> completeFeed{
        makeAddEvent(
            1,
            1000,
            Order{
                1,
                Side::Buy,
                9900,
                100,
                6001,
                DefaultSymbol
            }
        ),

        makeAddEvent(
            2,
            1100,
            Order{
                2,
                Side::Sell,
                10100,
                100,
                6002,
                DefaultSymbol
            }
        ),

        makeModifyEvent(
            3,
            1200,
            1,
            9950,
            100,
            6001,
            DefaultSymbol
        )
    };

    NetworkQueue queue;

    InMemoryRecoverySource recoverySource{
        completeFeed
    };

    RecoverySequencer sequencer{
        1,
        64,
        recoverySource
    };

    MarketMakerStrategy strategy{
        makeTestStrategyConfig(),
        makeTestRiskLimits()
    };

    PnlEngine pnl;

    pnl.registerAccount(
        5001,
        1'000'000.0L
    );

    pnl.setSymbolName(
        DefaultSymbol,
        "SIMULATED_MARKET"
    );

    UdpFeedProcessor processor{
        queue,
        sequencer,
        strategy,
        pnl
    };

    /*
        Sequence 1 arrives normally.
    */
    CHECK(
        queue.tryPush(
            makeUdpDatagram(
                completeFeed[0]
            )
        )
    );

    CHECK(processor.processAvailable() == 1);

    /*
        Sequence 2 is missing.

        Sequence 3 arrives, forcing the processor to:
        pause the strategy,
        recover sequence 2,
        apply sequence 2,
        apply sequence 3,
        resume the strategy.
    */
    CHECK(
        queue.tryPush(
            makeUdpDatagram(
                completeFeed[2]
            )
        )
    );

    CHECK(processor.processAvailable() == 1);

    CHECK(sequencer.healthy());
    CHECK(sequencer.expectedSequence() == 4);

    CHECK(
        sequencer.statistics()
            .events_recovered ==
        1
    );

    CHECK(
        processor.statistics()
            .recovery_pauses ==
        1
    );

    CHECK(
        processor.statistics()
            .recovery_resumes ==
        1
    );

    CHECK(bids.contains(9950));
    CHECK(asks.contains(10100));

    CHECK(
        !strategy.riskManager()
            .killSwitchActive()
    );

    checkBookInvariants();
}

void runUdpRecoveryTests() {
    testRecoveryRestoresMissingEvents();
    testOutOfOrderArrivalWithoutRecovery();
    testUdpProcessingLoopRecoversGap();

    std::cout
        << "All UDP recovery tests passed!\n";
}


void runUdpRecoveryTelemetryDemo() {
    resetOrderBook();

    constexpr ParticipantId DemoParticipant =
        5001;

    constexpr SymbolId DemoSymbol =
        DefaultSymbol;

    /*
        This directory is intentionally separate from:

        results/lobster_telemetry
        results/matching_engine_telemetry
    */
    analytics::TelemetryRecorder telemetry{
        "results/udp_recovery_telemetry"
    };

    analytics::SimulationTelemetryBridge
        telemetryBridge{
            telemetry
        };

    /*
        Complete source feed:

        sequence 1 = arrives normally
        sequence 2 = deliberately omitted from the queue
        sequence 3 = arrives early and exposes the gap

        The recovery source contains all three events, so
        sequence 2 can be recovered when sequence 3 arrives.
    */
    std::vector<MarketEvent> completeFeed{
        makeAddEvent(
            1,
            1'000,
            Order{
                1,
                Side::Buy,
                9'900,
                100,
                6'001,
                DemoSymbol
            }
        ),

        makeAddEvent(
            2,
            1'100,
            Order{
                2,
                Side::Sell,
                10'100,
                100,
                6'002,
                DemoSymbol
            }
        ),

        makeModifyEvent(
            3,
            1'200,
            1,
            9'950,
            100,
            6'001,
            DemoSymbol
        )
    };

    NetworkQueue queue;

    /*
        The recovery source has the complete feed, including
        the sequence-2 event that will be missing from the
        simulated UDP arrival path.
    */
    InMemoryRecoverySource recoverySource{
        completeFeed
    };

    RecoverySequencer sequencer{
        1,
        64,
        recoverySource
    };

    MarketMakerConfig strategyConfig =
        makeTestStrategyConfig();

    strategyConfig.participant_id =
        DemoParticipant;

    strategyConfig.symbol_id =
        DemoSymbol;

    MarketMakerStrategy strategy{
        strategyConfig,
        makeTestRiskLimits()
    };

    PnlEngine pnl;

    pnl.registerAccount(
        DemoParticipant,
        1'000'000.0L
    );

    pnl.setSymbolName(
        DemoSymbol,
        "UDP_RECOVERY_DEMO"
    );

    /*
        Passing &telemetryBridge activates the recovery
        telemetry already built into UdpFeedProcessor.
    */
    UdpFeedProcessor processor{
        queue,
        sequencer,
        strategy,
        pnl,
        &telemetryBridge,
        DemoParticipant,
        DemoSymbol
    };

    /*
        First, sequence 1 arrives normally.
    */
    if (!queue.tryPush(
        makeUdpDatagram(
            completeFeed[0]
        )
    )) {
        throw std::runtime_error(
            "Unable to queue UDP sequence 1"
        );
    }

    if (processor.processAvailable() != 1) {
        throw std::runtime_error(
            "UDP processor did not process sequence 1"
        );
    }

    if (!sequencer.healthy()) {
        throw std::runtime_error(
            "UDP sequencer was not healthy after sequence 1"
        );
    }

    /*
        Deliberately skip completeFeed[1], which is sequence 2.

        Sequence 3 now arrives. The processor should:

        1. detect that sequence 2 is missing;
        2. enter Recovering;
        3. record gap_start;
        4. pause the strategy;
        5. fetch sequence 2 from recoverySource;
        6. apply sequence 2;
        7. apply buffered sequence 3;
        8. record gap_end;
        9. return to Healthy;
        10. resume the strategy.
    */
    if (!queue.tryPush(
        makeUdpDatagram(
            completeFeed[2]
        )
    )) {
        throw std::runtime_error(
            "Unable to queue UDP sequence 3"
        );
    }

    if (processor.processAvailable() != 1) {
        throw std::runtime_error(
            "UDP processor did not process sequence 3"
        );
    }



    const RecoveryStatistics&
        recoveryStatistics =
            sequencer.statistics();

    const UdpProcessingStatistics&
        processingStatistics =
            processor.statistics();

    /*
        Validate the expected recovery outcome before
        presenting the telemetry as successful.
    */
    if (!sequencer.healthy()) {
        throw std::runtime_error(
            "UDP sequencer did not return to Healthy"
        );
    }

    if (sequencer.expectedSequence() != 4) {
        throw std::runtime_error(
            "UDP sequencer did not advance to sequence 4"
        );
    }

    if (
        recoveryStatistics.gaps_detected !=
        1
    ) {
        throw std::runtime_error(
            "UDP demo did not detect exactly one gap"
        );
    }

    if (
        recoveryStatistics.events_recovered !=
        1
    ) {
        throw std::runtime_error(
            "UDP demo did not recover exactly one event"
        );
    }

    if (
        recoveryStatistics.recovery_misses !=
        0
    ) {
        throw std::runtime_error(
            "UDP demo unexpectedly recorded a recovery miss"
        );
    }

    if (
        processingStatistics.recovery_pauses !=
        1
    ) {
        throw std::runtime_error(
            "Strategy was not paused exactly once"
        );
    }

    if (
        processingStatistics.recovery_resumes !=
        1
    ) {
        throw std::runtime_error(
            "Strategy was not resumed exactly once"
        );
    }

    if (queue.approximateSize() != 0) {
        throw std::runtime_error(
            "UDP queue was not empty after processing"
        );
    }

    checkBookInvariants();

    telemetry.flush();

    std::cout
        << "\nUDP recovery telemetry demo completed\n"
        << "-------------------------------------\n"
        << "Gaps detected: "
        << recoveryStatistics.gaps_detected
        << '\n'
        << "Events recovered: "
        << recoveryStatistics.events_recovered
        << '\n'
        << "Recovery misses: "
        << recoveryStatistics.recovery_misses
        << '\n'
        << "Strategy pauses: "
        << processingStatistics.recovery_pauses
        << '\n'
        << "Strategy resumes: "
        << processingStatistics.recovery_resumes
        << '\n'
        << "Final feed state: Healthy\n"
        << "Telemetry directory: "
        << std::filesystem::absolute(
               "results/udp_recovery_telemetry"
           )
        << '\n';
}

void runFullLobsterUdpRecoveryDemo() {
    resetOrderBook();

    constexpr ParticipantId DemoParticipant =
        5001;

    constexpr SymbolId DemoSymbol =
        DefaultSymbol;

    /*
        Deliberately omit one event every 25,000 converted
        events. The following event exposes the sequence gap.
    */
    constexpr std::uint64_t DropEvery =
        25'000;

    /*
        Record an ordinary state snapshot every 5,000 events.

        Recovery starts and ends are always recorded,
        regardless of this sampling interval.
    */
    constexpr std::uint64_t StateSampleInterval =
        5'000;

    const std::filesystem::path messagePath =
        "data/lobster/AAPL_message.csv";

    std::vector<MarketEvent> events =
        loadLobsterMessageFile(
            messagePath
        );

    if (events.size() < 2) {
        throw std::runtime_error(
            "LOBSTER UDP demo requires at least two events"
        );
    }

    /*
        The converter assigns contiguous sequence numbers to
        the converted event stream.
    */
    for (
    std::size_t index = 0;
    index < events.size();
    ++index
) {
        const std::uint64_t expectedSequence =
            static_cast<std::uint64_t>(
                index + 1
            );

        if (
            events[index].sequence_number !=
            expectedSequence
        ) {
            throw std::runtime_error(
                "Converted LOBSTER sequence numbers are "
                "not contiguous at event index " +
                std::to_string(index)
            );
        }
}

    /*
        Establish the expected final state by applying every
        converted event without injected packet loss.
    */
    resetOrderBook();

    for (const MarketEvent& event : events) {
        ApplyEventResult applied =
            applyMarketEvent(event);

        if (!applied.accepted) {
            throw std::runtime_error(
                "Clean LOBSTER baseline rejected sequence " +
                std::to_string(
                    event.sequence_number
                )
            );
        }
    }

    const std::string expectedCleanSnapshot =
        createOrderBookSnapshot();

    /*
        Clear the baseline before the fault-injected UDP replay.
    */
    resetOrderBook();

    analytics::TelemetryRecorder telemetry{
        "results/udp_recovery_telemetry"
    };

    analytics::SimulationTelemetryBridge
        telemetryBridge{
            telemetry
        };

    NetworkQueue queue;

    /*
        The recovery source contains every converted event.

        Events deliberately omitted from the UDP arrival
        stream can therefore be recovered by sequence number.
    */
    InMemoryRecoverySource recoverySource{
        events
    };

    RecoverySequencer sequencer{
        events.front().sequence_number,
        64,
        recoverySource
    };

    MarketMakerConfig strategyConfig =
        makeTestStrategyConfig();

    strategyConfig.participant_id =
        DemoParticipant;

    strategyConfig.symbol_id =
        DemoSymbol;

    /*
        Use an ID range far away from the LOBSTER IDs.
        Strategy updates are disabled below, but this also
        prevents accidental collisions.
    */
    strategyConfig.first_strategy_order_id =
        9'000'000'000'000'000'000ULL;

    MarketMakerStrategy strategy{
        strategyConfig,
        makeTestRiskLimits()
    };

    PnlEngine pnl;

    pnl.registerAccount(
        DemoParticipant,
        1'000'000.0L
    );

    pnl.setSymbolName(
        DemoSymbol,
        "AAPL_UDP_RECOVERY"
    );

    /*
        Constructor settings after DemoSymbol:

        false  = do not run the strategy after each event
        5000   = sample ordinary state every 5,000 events
        0      = do not record depth rows for this test
    */
    UdpFeedProcessor processor{
        queue,
        sequencer,
        strategy,
        pnl,
        &telemetryBridge,
        DemoParticipant,
        DemoSymbol,
        false,
        StateSampleInterval,
        0
    };

    /*
        Record the initial Healthy state before processing.
    */
    telemetryBridge.capture(
        events.front().timestamp_ns,
        DemoParticipant,
        DemoSymbol,
        pnl,
        strategy,
        &queue,
        &sequencer,
        0
    );

    std::uint64_t deliberatelyDropped = 0;
    std::uint64_t datagramsSubmitted = 0;

    for (
        std::size_t index = 0;
        index < events.size();
        ++index
    ) {
        const MarketEvent& event =
            events[index];

        /*
            Do not drop the final event because another event
            is required to reveal the missing sequence.
        */
        const bool hasFollowingEvent =
            index + 1 < events.size();

        const bool shouldDrop =
            hasFollowingEvent &&
            event.sequence_number %
                DropEvery ==
                0;

        if (shouldDrop) {
            ++deliberatelyDropped;

            /*
                Simulate packet loss: do not place this event
                into the UDP queue.

                It remains available in recoverySource.
            */
            continue;
        }

        if (!queue.tryPush(
            makeUdpDatagram(event)
        )) {
            throw std::runtime_error(
                "UDP queue rejected LOBSTER event " +
                std::to_string(
                    event.sequence_number
                )
            );
        }

        ++datagramsSubmitted;

        const std::size_t processed =
            processor.processAvailable();

        if (processed != 1) {
            throw std::runtime_error(
                "UDP processor did not process exactly one "
                "submitted LOBSTER datagram"
            );
        }

        if (
            sequencer.state() ==
            FeedState::Failed
        ) {
            throw std::runtime_error(
                "UDP sequencer failed at LOBSTER event " +
                std::to_string(
                    event.sequence_number
                )
            );
        }

        if (
    event.sequence_number > 1 &&
    (
        event.sequence_number - 1
    ) % 50'000 == 0
) {
            std::cout
                << "LOBSTER UDP events processed through: "
                << event.sequence_number
                << " / "
                << events.size()
                << '\n';

            telemetry.flush();
}
    }

    /*
        Force the completed final state into state.csv even
        when the event count does not land on a sampling
        boundary.
    */
    telemetryBridge.capture(
        events.back().timestamp_ns,
        DemoParticipant,
        DemoSymbol,
        pnl,
        strategy,
        &queue,
        &sequencer,
        0
    );

    const RecoveryStatistics&
        recoveryStatistics =
            sequencer.statistics();

    const UdpProcessingStatistics&
        processingStatistics =
            processor.statistics();

    const std::uint64_t expectedFinalSequence =
        events.back().sequence_number + 1;

    if (!sequencer.healthy()) {
        throw std::runtime_error(
            "Full LOBSTER UDP replay did not finish Healthy"
        );
    }

    if (
        sequencer.expectedSequence() !=
        expectedFinalSequence
    ) {
        throw std::runtime_error(
            "Full LOBSTER UDP replay ended at the wrong "
            "expected sequence"
        );
    }

    if (
        recoveryStatistics.events_applied !=
        events.size()
    ) {
        throw std::runtime_error(
            "Not every converted LOBSTER event was applied"
        );
    }

    if (
        recoveryStatistics.gaps_detected !=
        deliberatelyDropped
    ) {
        throw std::runtime_error(
            "Detected gap count does not match injected "
            "packet-loss count"
        );
    }

    if (
        recoveryStatistics.events_recovered !=
        deliberatelyDropped
    ) {
        throw std::runtime_error(
            "Recovered event count does not match injected "
            "packet-loss count"
        );
    }

    if (
        recoveryStatistics.recovery_misses !=
        0
    ) {
        throw std::runtime_error(
            "Full LOBSTER UDP replay recorded recovery misses"
        );
    }

    if (
        recoveryStatistics.application_rejections !=
        0
    ) {
        throw std::runtime_error(
            "Full LOBSTER UDP replay had order-book "
            "application rejections"
        );
    }
    if (
    processingStatistics.datagrams_processed !=
    datagramsSubmitted
) {
        throw std::runtime_error(
            "Processed datagram count does not match "
            "submitted datagram count"
        );
}

    if (
        processingStatistics.decode_failures !=
        0
    ) {
        throw std::runtime_error(
            "Full LOBSTER UDP replay had decode failures"
        );
    }

    if (
        processingStatistics.sequencer_failures !=
        0
    ) {
        throw std::runtime_error(
            "Full LOBSTER UDP replay had sequencer failures"
        );
    }

    if (
        processingStatistics.recovery_pauses !=
        deliberatelyDropped
    ) {
        throw std::runtime_error(
            "Recovery pause count does not match "
            "injected packet-loss count"
        );
    }

    if (
        processingStatistics.recovery_resumes !=
        deliberatelyDropped
    ) {
        throw std::runtime_error(
            "Recovery resume count does not match "
            "injected packet-loss count"
        );
    }

    if (queue.approximateSize() != 0) {
        throw std::runtime_error(
            "UDP queue was not empty after full replay"
        );
    }

    checkBookInvariants();

    /*
        Compare the final fault-recovered book against the book
        produced by applying the complete event stream without
        any injected packet loss.
    */
    const std::string recoveredSnapshot =
        createOrderBookSnapshot();

    if (
        recoveredSnapshot !=
        expectedCleanSnapshot
    ) {
        throw std::runtime_error(
            "Recovered LOBSTER order book differs from "
            "the clean baseline replay"
        );
    }

    telemetry.flush();

    std::cout
        << "\nFull LOBSTER UDP recovery demo completed\n"
        << "----------------------------------------\n"
        << "Converted LOBSTER events: "
        << events.size()
        << '\n'
        << "UDP datagrams submitted: "
        << datagramsSubmitted
        << '\n'
        << "Packets deliberately omitted: "
        << deliberatelyDropped
        << '\n'
        << "Gaps detected: "
        << recoveryStatistics.gaps_detected
        << '\n'
        << "Events recovered: "
        << recoveryStatistics.events_recovered
        << '\n'
        << "Recovery misses: "
        << recoveryStatistics.recovery_misses
        << '\n'
        << "Application rejections: "
        << recoveryStatistics
               .application_rejections
        << '\n'
        << "Datagrams processed: "
        << processingStatistics
               .datagrams_processed
        << '\n'
        << "Final feed state: Healthy\n"
        << "Telemetry directory: "
        << std::filesystem::absolute(
               "results/udp_recovery_telemetry"
           )
        << '\n';
}


double currentWorkingSetMb() {
    PROCESS_MEMORY_COUNTERS counters{};

    if (
        GetProcessMemoryInfo(
            GetCurrentProcess(),
            &counters,
            static_cast<DWORD>(
                sizeof(counters)
            )
        ) == 0
    ) {
        return -1.0;
    }

    constexpr double BytesPerMegabyte =
        1024.0 * 1024.0;

    return static_cast<double>(
        counters.WorkingSetSize
    ) / BytesPerMegabyte;
}


std::uint64_t percentileFromSorted(
    const std::vector<std::uint64_t>& sortedSamples,
    long double percentile
) {
    if (sortedSamples.empty()) {
        return 0;
    }

    if (sortedSamples.size() == 1) {
        return sortedSamples.front();
    }

    const long double position =
        percentile *
        static_cast<long double>(
            sortedSamples.size() - 1
        );

    const std::size_t lowerIndex =
        static_cast<std::size_t>(
            std::floor(position)
        );

    const std::size_t upperIndex =
        static_cast<std::size_t>(
            std::ceil(position)
        );

    if (lowerIndex == upperIndex) {
        return sortedSamples[lowerIndex];
    }

    const long double fraction =
        position -
        static_cast<long double>(
            lowerIndex
        );

    const long double interpolated =
        static_cast<long double>(
            sortedSamples[lowerIndex]
        ) *
            (1.0L - fraction) +
        static_cast<long double>(
            sortedSamples[upperIndex]
        ) *
            fraction;

    return static_cast<std::uint64_t>(
        std::llround(interpolated)
    );
}


void runLobsterScalabilityBenchmark() {
    /*
        This benchmark measures MarketEvent application after
        the source CSV has already been loaded and converted.

        It therefore measures replay/matching-engine scaling,
        not CSV parsing speed.
    */
    const std::filesystem::path messagePath =
        "data/lobster/AAPL_message.csv";

    std::vector<MarketEvent> sourceEvents =
        loadLobsterMessageFile(
            messagePath
        );

    if (sourceEvents.empty()) {
        throw std::runtime_error(
            "Scalability benchmark has no source events"
        );
    }

    constexpr std::array<
        std::uint64_t,
        5
    > WorkloadSizes{
        100'000,
        500'000,
        1'000'000,
        5'000'000,
        10'000'000
    };

    constexpr std::size_t Repetitions = 5;

    /*
        Record one latency measurement per 100 events.

        This provides:
            1,000 samples at 100,000 events;
            100,000 samples at 10,000,000 events.

        Sampling avoids adding two clock calls to every event.
    */
    constexpr std::uint64_t
        LatencySampleInterval = 100;

    /*
        Poll memory every 100,000 events. The reported memory is
        the maximum observed process working set during the run.
    */
    constexpr std::uint64_t
        MemorySampleInterval = 100'000;

    std::filesystem::create_directories(
        "results/scalability"
    );

    const std::filesystem::path outputPath =
        "results/scalability/"
        "lobster_scalability.csv";

    std::ofstream output{
        outputPath,
        std::ios::trunc
    };

    if (!output.is_open()) {
        throw std::runtime_error(
            "Unable to create scalability results CSV"
        );
    }

    output
        << "events,"
        << "repetition,"
        << "source_events,"
        << "full_source_passes,"
        << "final_prefix_events,"
        << "runtime_seconds,"
        << "throughput_eps,"
        << "p50_ns,"
        << "p95_ns,"
        << "p99_ns,"
        << "max_sampled_ns,"
        << "peak_working_set_mb,"
        << "final_resting_orders,"
        << "rejected_events,"
        << "invariants_ok\n";

    /*
        Disable ordinary analytics telemetry. Recording every
        benchmark operation to CSV would measure telemetry
        overhead rather than engine scalability.
    */
    analytics::active_recorder = nullptr;

    std::cout
        << "\nLOBSTER scalability benchmark\n"
        << "-----------------------------\n"
        << "Source converted events: "
        << sourceEvents.size()
        << '\n'
        << "Repetitions per workload: "
        << Repetitions
        << "\n\n";

    for (
        std::uint64_t workloadSize :
        WorkloadSizes
    ) {
        for (
            std::size_t repetition = 1;
            repetition <= Repetitions;
            ++repetition
        ) {
            std::uint64_t processedEvents = 0;
            std::uint64_t rejectedEvents = 0;
            std::uint64_t measuredRuntimeNs = 0;

            double peakWorkingSetMb =
                currentWorkingSetMb();

            std::vector<std::uint64_t>
                latencySamples;

            latencySamples.reserve(
                static_cast<std::size_t>(
                    workloadSize /
                        LatencySampleInterval +
                    1
                )
            );

            /*
                Each pass starts from a clean book because the
                original LOBSTER order IDs repeat when the source
                stream is reused.
            */
            while (
                processedEvents <
                workloadSize
            ) {
                resetOrderBook();

                /*
                    Reserve outside the measured interval.
                */
                orderIndex.reserve(
                    sourceEvents.size()
                );

                const std::uint64_t remaining =
                    workloadSize -
                    processedEvents;

                const std::size_t batchSize =
                    static_cast<std::size_t>(
                        std::min<std::uint64_t>(
                            remaining,
                            sourceEvents.size()
                        )
                    );

                const std::uint64_t batchStartNs =
                    steadyTimestampNs();

                for (
                    std::size_t sourceIndex = 0;
                    sourceIndex < batchSize;
                    ++sourceIndex
                ) {
                    const bool measureLatency =
                        processedEvents %
                            LatencySampleInterval ==
                        0;

                    std::uint64_t operationStartNs = 0;

                    if (measureLatency) {
                        operationStartNs =
                            steadyTimestampNs();
                    }

                    ApplyEventResult applied =
                        applyMarketEvent(
                            sourceEvents[sourceIndex]
                        );

                    if (measureLatency) {
                        const std::uint64_t
                            operationEndNs =
                                steadyTimestampNs();

                        latencySamples.push_back(
                            operationEndNs -
                            operationStartNs
                        );
                    }

                    if (!applied.accepted) {
                        ++rejectedEvents;

                        throw std::runtime_error(
                            "Scalability replay rejected "
                            "source sequence " +
                            std::to_string(
                                sourceEvents[
                                    sourceIndex
                                ].sequence_number
                            ) +
                            " at measured event " +
                            std::to_string(
                                processedEvents
                            )
                        );
                    }

                    ++processedEvents;

                    if (
                        processedEvents %
                            MemorySampleInterval ==
                        0
                    ) {
                        peakWorkingSetMb =
                            std::max(
                                peakWorkingSetMb,
                                currentWorkingSetMb()
                            );
                    }
                }

                const std::uint64_t batchEndNs =
                    steadyTimestampNs();

                measuredRuntimeNs +=
                    batchEndNs -
                    batchStartNs;

                /*
                    This validation is outside the measured
                    replay interval.
                */
                checkBookInvariants();
            }

            peakWorkingSetMb =
                std::max(
                    peakWorkingSetMb,
                    currentWorkingSetMb()
                );

            std::sort(
                latencySamples.begin(),
                latencySamples.end()
            );

            const std::uint64_t p50Ns =
                percentileFromSorted(
                    latencySamples,
                    0.50L
                );

            const std::uint64_t p95Ns =
                percentileFromSorted(
                    latencySamples,
                    0.95L
                );

            const std::uint64_t p99Ns =
                percentileFromSorted(
                    latencySamples,
                    0.99L
                );

            const std::uint64_t maxSampledNs =
                latencySamples.empty()
                    ? 0
                    : latencySamples.back();

            const long double runtimeSeconds =
                static_cast<long double>(
                    measuredRuntimeNs
                ) /
                1'000'000'000.0L;

            const long double throughput =
                runtimeSeconds > 0.0L
                    ? static_cast<long double>(
                          workloadSize
                      ) /
                          runtimeSeconds
                    : 0.0L;

            const std::uint64_t fullPasses =
                workloadSize /
                sourceEvents.size();

            const std::uint64_t finalPrefix =
                workloadSize %
                sourceEvents.size();

            output
                << workloadSize
                << ','
                << repetition
                << ','
                << sourceEvents.size()
                << ','
                << fullPasses
                << ','
                << finalPrefix
                << ','
                << std::fixed
                << std::setprecision(9)
                << static_cast<double>(
                       runtimeSeconds
                   )
                << ','
                << std::setprecision(3)
                << static_cast<double>(
                       throughput
                   )
                << ','
                << p50Ns
                << ','
                << p95Ns
                << ','
                << p99Ns
                << ','
                << maxSampledNs
                << ','
                << std::setprecision(3)
                << peakWorkingSetMb
                << ','
                << orderIndex.size()
                << ','
                << rejectedEvents
                << ','
                << 1
                << '\n';

            output.flush();

            std::cout
                << "Events: "
                << workloadSize
                << ", repetition: "
                << repetition
                << ", runtime: "
                << std::fixed
                << std::setprecision(3)
                << static_cast<double>(
                       runtimeSeconds
                   )
                << " s, throughput: "
                << static_cast<double>(
                       throughput
                   )
                << " events/s, p99: "
                << p99Ns
                << " ns, memory: "
                << peakWorkingSetMb
                << " MB\n";
        }
    }

    output.flush();

    std::cout
        << "\nScalability benchmark completed\n"
        << "Results: "
        << std::filesystem::absolute(
               outputPath
           )
        << '\n';
}

void writeLatencySeries(
    analytics::TelemetryRecorder& telemetry,
    const std::string& operation,
    const std::vector<std::uint64_t>& samples
) {
    const std::uint64_t baseTimestamp =
        steadyTimestampNs();

    for (
        std::size_t index = 0;
        index < samples.size();
        ++index
    ) {
        telemetry.recordLatency(
            analytics::LatencySample{
                baseTimestamp + index,
                operation,
                samples[index]
            }
        );
    }
}


void runMatchingEngineLatencyBenchmark() {
    /*
        Important:

        - All input orders are prepared before timing.
        - CSV parsing is not involved.
        - Telemetry writing occurs after measurement.
        - State/depth/P&L capture is not involved.
        - The benchmark uses an already-loaded book.
    */
    constexpr std::size_t WarmupOperations =
        10'000;

    constexpr std::size_t MeasuredOperations =
        100'000;

    constexpr std::size_t InitialBookOrders =
        100'000;

    std::vector<std::uint64_t> addLatencies;
    std::vector<std::uint64_t> cancelLatencies;
    std::vector<std::uint64_t> modifyLatencies;
    std::vector<std::uint64_t> executeLatencies;

    addLatencies.reserve(
        MeasuredOperations
    );

    cancelLatencies.reserve(
        MeasuredOperations
    );

    modifyLatencies.reserve(
        MeasuredOperations
    );

    executeLatencies.reserve(
        MeasuredOperations
    );

    /*
        Do not let the normal telemetry system run inside
        the measured engine calls.
    */
    analytics::active_recorder = nullptr;

    resetOrderBook();

    /*
        Reduce unordered_map rehashing during the benchmark.
    */
    orderIndex.reserve(
        InitialBookOrders * 2
    );

    tradeHistory.reserve(16);

    OrderId nextOrderId = 1;

    /*
        Create a realistic loaded book before measuring.

        Bids are below the market.
        Asks are above the market.
    */
    for (
        std::size_t index = 0;
        index < InitialBookOrders;
        ++index
    ) {
        Side side =
            index % 2 == 0
                ? Side::Buy
                : Side::Sell;

        Price price =
            side == Side::Buy
                ? 9900 -
                    static_cast<Price>(
                        index % 100
                    )
                : 10100 +
                    static_cast<Price>(
                        index % 100
                    );

        bool accepted =
            addOrder(
                Order{
                    nextOrderId++,
                    side,
                    price,
                    10,
                    UnknownParticipant,
                    DefaultSymbol
                }
            );

        if (!accepted) {
            throw std::runtime_error(
                "Unable to prepare benchmark order book"
            );
        }
    }

    /*
        --------------------------------------------------
        ADD-ORDER LATENCY
        --------------------------------------------------

        Cleanup is performed after the timer stops.
    */
    for (
        std::size_t index = 0;
        index <
            WarmupOperations +
            MeasuredOperations;
        ++index
    ) {
        Order order{
            nextOrderId++,
            Side::Buy,
            9850,
            10,
            UnknownParticipant,
            DefaultSymbol
        };

        const std::uint64_t startNs =
            steadyTimestampNs();

        bool accepted =
            addOrder(order);

        const std::uint64_t endNs =
            steadyTimestampNs();

        if (!accepted) {
            throw std::runtime_error(
                "Add benchmark operation was rejected"
            );
        }

        if (index >= WarmupOperations) {
            addLatencies.push_back(
                endNs - startNs
            );
        }

        /*
            Cleanup is deliberately outside the measured
            interval.
        */
        if (!cancelOrder(order.id)) {
            throw std::runtime_error(
                "Add benchmark cleanup failed"
            );
        }
    }

    /*
        --------------------------------------------------
        CANCEL-ORDER LATENCY
        --------------------------------------------------
    */
    OrderId cancellationTarget =
        nextOrderId++;

    if (!addOrder(
        Order{
            cancellationTarget,
            Side::Buy,
            9849,
            10,
            UnknownParticipant,
            DefaultSymbol
        }
    )) {
        throw std::runtime_error(
            "Unable to prepare cancellation benchmark"
        );
    }

    for (
        std::size_t index = 0;
        index <
            WarmupOperations +
            MeasuredOperations;
        ++index
    ) {
        const std::uint64_t startNs =
            steadyTimestampNs();

        bool cancelled =
            cancelOrder(
                cancellationTarget
            );

        const std::uint64_t endNs =
            steadyTimestampNs();

        if (!cancelled) {
            throw std::runtime_error(
                "Cancel benchmark operation was rejected"
            );
        }

        if (index >= WarmupOperations) {
            cancelLatencies.push_back(
                endNs - startNs
            );
        }

        /*
            Add the next cancellation target outside the
            timed interval.
        */
        cancellationTarget =
            nextOrderId++;

        if (!addOrder(
            Order{
                cancellationTarget,
                Side::Buy,
                9849,
                10,
                UnknownParticipant,
                DefaultSymbol
            }
        )) {
            throw std::runtime_error(
                "Unable to reset cancellation benchmark"
            );
        }
    }

    /*
        --------------------------------------------------
        MODIFY-ORDER LATENCY
        --------------------------------------------------

        This measures a same-price quantity reduction,
        which preserves FIFO priority.
    */
    OrderId modificationTarget =
        nextOrderId++;

    if (!addOrder(
        Order{
            modificationTarget,
            Side::Buy,
            9848,
            20,
            UnknownParticipant,
            DefaultSymbol
        }
    )) {
        throw std::runtime_error(
            "Unable to prepare modification benchmark"
        );
    }

    for (
        std::size_t index = 0;
        index <
            WarmupOperations +
            MeasuredOperations;
        ++index
    ) {
        const std::uint64_t startNs =
            steadyTimestampNs();

        bool modified =
            modifyOrder(
                modificationTarget,
                9848,
                10
            );

        const std::uint64_t endNs =
            steadyTimestampNs();

        if (!modified) {
            throw std::runtime_error(
                "Modify benchmark operation was rejected"
            );
        }

        if (index >= WarmupOperations) {
            modifyLatencies.push_back(
                endNs - startNs
            );
        }

        /*
            Restore quantity outside the measured interval.

            Increasing quantity loses queue priority, which
            matches your engine's intended behaviour.
        */
        if (!modifyOrder(
            modificationTarget,
            9848,
            20
        )) {
            throw std::runtime_error(
                "Unable to reset modification benchmark"
            );
        }
    }

    /*
        --------------------------------------------------
        EXECUTE-ORDER LATENCY
        --------------------------------------------------

        Each incoming buy executes exactly one resting sell.
    */
    for (
        std::size_t index = 0;
        index <
            WarmupOperations +
            MeasuredOperations;
        ++index
    ) {
        OrderId restingOrderId =
            nextOrderId++;

        OrderId incomingOrderId =
            nextOrderId++;

        /*
            Setup occurs outside the timed interval.
        */
        if (!addOrder(
            Order{
                restingOrderId,
                Side::Sell,
                10000,
                10,
                UnknownParticipant,
                DefaultSymbol
            }
        )) {
            throw std::runtime_error(
                "Unable to prepare execution benchmark"
            );
        }

        Order incomingOrder{
            incomingOrderId,
            Side::Buy,
            10000,
            10,
            UnknownParticipant,
            DefaultSymbol
        };

        const std::uint64_t startNs =
            steadyTimestampNs();

        ExecutionResult execution =
            executeOrder(
                incomingOrder
            );

        const std::uint64_t endNs =
            steadyTimestampNs();

        if (
            !execution.accepted ||
            execution.executed_quantity != 10 ||
            execution.remaining_quantity != 0
        ) {
            throw std::runtime_error(
                "Execute benchmark operation failed"
            );
        }

        if (index >= WarmupOperations) {
            executeLatencies.push_back(
                endNs - startNs
            );
        }

        /*
            Prevent trade history from growing throughout
            the benchmark. This happens after timing.
        */
        tradeHistory.clear();
    }

    checkBookInvariants();

    /*
        Only now write the samples to disk.
    */
    analytics::TelemetryRecorder telemetry{
        "results/matching_engine_telemetry"
    };

    writeLatencySeries(
        telemetry,
        "engine_add_order",
        addLatencies
    );

    writeLatencySeries(
        telemetry,
        "engine_cancel_order",
        cancelLatencies
    );

    writeLatencySeries(
        telemetry,
        "engine_modify_reduce",
        modifyLatencies
    );

    writeLatencySeries(
        telemetry,
        "engine_execute_one_fill",
        executeLatencies
    );

    telemetry.flush();

    std::cout
        << "\nMatching-engine latency benchmark completed\n"
        << "-------------------------------------------\n"
        << "Warm-up operations per test: "
        << WarmupOperations
        << '\n'
        << "Measured operations per test: "
        << MeasuredOperations
        << '\n'
        << "Initial resting orders: "
        << InitialBookOrders
        << '\n'
        << "Telemetry directory: "
        << std::filesystem::absolute(
               "results/matching_engine_telemetry"
           )
        << '\n';
}
std::vector<std::string> splitFixedCsvRow(
    const std::string& line,
    std::size_t expectedColumns,
    std::size_t rowNumber,
    const std::string& fileLabel
) {
    std::stringstream stream{line};
    std::vector<std::string> columns;

    std::string value;

    while (std::getline(stream, value, ',')) {
        columns.push_back(value);
    }

    if (columns.size() != expectedColumns) {
        throw std::runtime_error(
            fileLabel +
            " row " +
            std::to_string(rowNumber) +
            " contains " +
            std::to_string(columns.size()) +
            " columns; expected " +
            std::to_string(expectedColumns)
        );
    }

    return columns;
}


void writeLobsterReferenceTelemetry(
    const std::filesystem::path& messagePath,
    const std::filesystem::path& orderBookPath
) {
    std::ifstream messageInput{messagePath};
    std::ifstream orderBookInput{orderBookPath};

    if (!messageInput.is_open()) {
        throw std::runtime_error(
            "Unable to open LOBSTER message file: " +
            messagePath.string()
        );
    }

    if (!orderBookInput.is_open()) {
        throw std::runtime_error(
            "Unable to open LOBSTER order-book file: " +
            orderBookPath.string()
        );
    }

    /*
        TelemetryRecorder creates:

        state.csv
        depth.csv
        quotes.csv
        fills.csv
        latency.csv
        recovery.csv

        Quotes, fills and recovery will contain headers only,
        because this is an offline market-data reconstruction,
        not a strategy or UDP-recovery simulation.
    */
    analytics::TelemetryRecorder telemetry{
        "results/lobster_telemetry"
    };

    std::string messageLine;
    std::string orderBookLine;

    std::uint64_t rowNumber = 0;

    /*
        State is recorded for every LOBSTER row.

        Depth is sampled every 100 rows so that the Python
        heatmap does not attempt to construct an enormous
        dense matrix from roughly 7.5 million depth entries.
    */
    constexpr std::uint64_t DepthSamplingInterval = 100;

    while (true) {
        bool hasMessageRow =
            static_cast<bool>(
                std::getline(
                    messageInput,
                    messageLine
                )
            );

        bool hasOrderBookRow =
            static_cast<bool>(
                std::getline(
                    orderBookInput,
                    orderBookLine
                )
            );

        if (hasMessageRow != hasOrderBookRow) {
            throw std::runtime_error(
                "LOBSTER message and order-book files "
                "contain different numbers of rows"
            );
        }

        if (!hasMessageRow) {
            break;
        }

        ++rowNumber;

        if (
            messageLine.empty() ||
            orderBookLine.empty()
        ) {
            throw std::runtime_error(
                "Unexpected empty LOBSTER row at row " +
                std::to_string(rowNumber)
            );
        }

        std::uint64_t processingStartNs =
            steadyTimestampNs();

        /*
            Message file:

            time,event_type,order_id,size,price,direction
        */
        std::vector<std::string> messageColumns =
            splitFixedCsvRow(
                messageLine,
                6,
                rowNumber,
                "LOBSTER message file"
            );

        /*
            Level-10 order-book file:

            ask_price_1,ask_size_1,bid_price_1,bid_size_1,
            ...
            ask_price_10,ask_size_10,bid_price_10,bid_size_10
        */
        std::vector<std::string> bookColumns =
            splitFixedCsvRow(
                orderBookLine,
                40,
                rowNumber,
                "LOBSTER order-book file"
            );

        long double timeSeconds =
            std::stold(
                messageColumns[0]
            );

        TimestampNs timestamp =
            static_cast<TimestampNs>(
                std::llround(
                    timeSeconds *
                    1'000'000'000.0L
                )
            );

        std::array<Price, 10> askPrices{};
        std::array<Price, 10> bidPrices{};

        std::array<std::uint64_t, 10>
            askQuantities{};

        std::array<std::uint64_t, 10>
            bidQuantities{};

        for (
            std::size_t level = 0;
            level < 10;
            ++level
        ) {
            std::size_t baseColumn =
                level * 4;

            askPrices[level] =
                static_cast<Price>(
                    std::stoll(
                        bookColumns[
                            baseColumn
                        ]
                    )
                );

            askQuantities[level] =
                std::stoull(
                    bookColumns[
                        baseColumn + 1
                    ]
                );

            bidPrices[level] =
                static_cast<Price>(
                    std::stoll(
                        bookColumns[
                            baseColumn + 2
                        ]
                    )
                );

            bidQuantities[level] =
                std::stoull(
                    bookColumns[
                        baseColumn + 3
                    ]
                );
        }

        Price bestAsk =
            askPrices[0];

        Price bestBid =
            bidPrices[0];

        if (
            bestAsk <= 0 ||
            bestBid <= 0 ||
            askQuantities[0] == 0 ||
            bidQuantities[0] == 0
        ) {
            throw std::runtime_error(
                "LOBSTER top of book is invalid at row " +
                std::to_string(rowNumber)
            );
        }

        /*
            A strictly crossed authoritative snapshot would
            indicate a malformed or incorrectly parsed file.

            A zero spread is allowed here for defensive
            compatibility with transient locked snapshots.
        */
        if (bestBid > bestAsk) {
            throw std::runtime_error(
                "Authoritative LOBSTER order book is crossed "
                "at row " +
                std::to_string(rowNumber) +
                ": bid=" +
                std::to_string(bestBid) +
                ", ask=" +
                std::to_string(bestAsk)
            );
        }

        long double midPrice =
            (
                static_cast<long double>(
                    bestBid
                ) +
                static_cast<long double>(
                    bestAsk
                )
            ) /
            2.0L;

        std::uint64_t wallTimeNs =
            steadyTimestampNs();

        analytics::StateSample state;

        state.timestamp_ns =
            timestamp;

        state.wall_time_ns =
            wallTimeNs;

        state.event_index =
            rowNumber;

        state.participant_id =
            UnknownParticipant;

        state.symbol_id =
            DefaultSymbol;

        state.best_bid =
            bestBid;

        state.best_ask =
            bestAsk;

        state.mid_price =
            midPrice;

        /*
            No strategy is running in this reference-data pass.
        */
        state.strategy_bid = 0;
        state.strategy_ask = 0;
        state.strategy_spread = 0;

        /*
            No account trades are attributed to a strategy.
        */
        state.realised_pnl = 0.0L;
        state.unrealised_pnl = 0.0L;
        state.total_pnl = 0.0L;
        state.cash_balance = 0.0L;
        state.account_equity = 0.0L;
        state.inventory = 0;
        state.maximum_absolute_position = 0;

        state.market_spread =
            bestAsk -
            bestBid;

        state.queue_depth = 0;

        state.events_processed =
            rowNumber;

        state.gaps_detected = 0;
        state.events_recovered = 0;
        state.recovery_misses = 0;

        state.feed_state =
            "Healthy";

        telemetry.recordState(
            state
        );

        bool shouldRecordDepth =
            rowNumber == 1 ||
            rowNumber %
                DepthSamplingInterval ==
                0;

        if (shouldRecordDepth) {
            for (
                std::size_t level = 0;
                level < 10;
                ++level
            ) {
                /*
                    LOBSTER can use sentinel values for empty
                    levels. Record only real positive entries.
                */
                bool validAsk =
                    askPrices[level] > 0 &&
                    askPrices[level] <
                        9'000'000'000LL &&
                    askQuantities[level] > 0;

                bool validBid =
                    bidPrices[level] > 0 &&
                    bidPrices[level] <
                        9'000'000'000LL &&
                    bidQuantities[level] > 0;

                if (validAsk) {
                    telemetry.recordDepth(
                        analytics::DepthSample{
                            timestamp,
                            rowNumber,
                            "ASK",
                            askPrices[level],
                            askQuantities[level],
                            level + 1
                        }
                    );
                }

                if (validBid) {
                    telemetry.recordDepth(
                        analytics::DepthSample{
                            timestamp,
                            rowNumber,
                            "BID",
                            bidPrices[level],
                            bidQuantities[level],
                            level + 1
                        }
                    );
                }
            }
        }

        std::uint64_t processingEndNs =
            steadyTimestampNs();

        telemetry.recordLatency(
            analytics::LatencySample{
                processingEndNs,
                "lobster_reference_row",
                processingEndNs -
                    processingStartNs
            }
        );

        if (
            rowNumber %
                100'000 ==
                0
        ) {
            telemetry.flush();

            std::cout
                << "LOBSTER reference rows processed: "
                << rowNumber
                << '\n';
        }
    }

    if (rowNumber == 0) {
        throw std::runtime_error(
            "LOBSTER reference files contain no rows"
        );
    }

    telemetry.flush();

    std::cout
        << "\nAuthoritative LOBSTER telemetry written to: "
        << std::filesystem::absolute(
               "results/lobster_telemetry"
           )
        << '\n'
        << "State rows: "
        << rowNumber
        << '\n'
        << "Depth sampling interval: "
        << DepthSamplingInterval
        << " events\n";
}

void runLobsterReplayDemo() {
    std::filesystem::path messagePath =
        "data/lobster/AAPL_message.csv";

    std::filesystem::path orderBookPath =
        "data/lobster/AAPL_orderbook_10.csv";

    /*
        Convert the message rows into the project's normalised
        MarketEvent representation.

        This is still useful for:

        - CSV parsing
        - event normalisation
        - binary serialisation
        - checksum verification
        - binary parser round-trip testing

        It is not used as the authoritative source for
        top-of-book charts.
    */
    std::vector<MarketEvent> events =
        loadLobsterMessageFile(
            messagePath
        );

    if (events.empty()) {
        throw std::runtime_error(
            "LOBSTER conversion produced no replayable events"
        );
    }

    std::cout
        << "Loaded "
        << events.size()
        << " converted LOBSTER events.\n";

    std::filesystem::create_directories(
        "data/processed"
    );

    std::filesystem::path binaryPath =
        "data/processed/AAPL_lobster.bin";

    std::string writeError;

    if (!BinaryEventWriter::write(
        binaryPath,
        events,
        writeError
    )) {
        throw std::runtime_error(
            "LOBSTER binary write failed: " +
            writeError
        );
    }

    ParseResult parseResult =
        BinaryEventParser::parse(
            binaryPath
        );

    printParseStatistics(
        parseResult
    );

    if (!parseResult.success) {
        throw std::runtime_error(
            "Unable to parse generated LOBSTER binary"
        );
    }

    if (parseResult.events != events) {
        throw std::runtime_error(
            "LOBSTER binary round-trip changed event data"
        );
    }

    std::cout
        << "\nLOBSTER message conversion and "
           "binary round-trip passed.\n"
        << "Binary file: "
        << std::filesystem::absolute(
               binaryPath
           )
        << '\n';

    /*
        Generate market charts from LOBSTER's authoritative
        level-10 snapshots rather than attempting to maintain
        a complete unlimited book from a level-limited message
        stream.
    */
    writeLobsterReferenceTelemetry(
        messagePath,
        orderBookPath
    );
}

void runLobsterUdpScalabilityBenchmark() {
    constexpr std::array<
        std::uint64_t,
        5
    > WorkloadSizes{
        100'000,
        500'000,
        1'000'000,
        5'000'000,
        10'000'000
    };

    constexpr std::size_t Repetitions = 5;

    constexpr std::uint64_t DropEvery =
        25'000;

    constexpr ParticipantId Participant =
        5001;

    constexpr SymbolId Symbol =
        DefaultSymbol;

    const std::filesystem::path messagePath =
        "data/lobster/AAPL_message.csv";

    const std::vector<MarketEvent> sourceEvents =
        loadLobsterMessageFile(
            messagePath
        );

    if (sourceEvents.size() < 2) {
        throw std::runtime_error(
            "UDP scalability benchmark requires "
            "at least two converted events"
        );
    }

    std::filesystem::create_directories(
        "results/scalability"
    );

    const std::filesystem::path outputPath =
        "results/scalability/"
        "udp_scalability.csv";

    std::ofstream output{
        outputPath,
        std::ios::trunc
    };

    if (!output.is_open()) {
        throw std::runtime_error(
            "Unable to create UDP scalability CSV"
        );
    }

    output
        << "events,"
        << "repetition,"
        << "runtime_seconds,"
        << "throughput_eps,"
        << "datagrams_submitted,"
        << "packets_deliberately_omitted,"
        << "gaps_detected,"
        << "events_recovered,"
        << "recovery_misses,"
        << "recovery_p50_ns,"
        << "recovery_p95_ns,"
        << "recovery_p99_ns,"
        << "recovery_max_ns,"
        << "application_rejections,"
        << "decode_failures,"
        << "sequencer_failures,"
        << "final_feed_healthy,"
        << "invariants_ok\n";

    /*
        Do not write the ordinary dashboard telemetry during
        this benchmark. It would distort the measurements.
    */
    analytics::active_recorder = nullptr;

    std::cout
        << "\nUDP scalability benchmark\n"
        << "-------------------------\n"
        << "Source events: "
        << sourceEvents.size()
        << '\n'
        << "Repetitions per size: "
        << Repetitions
        << "\n\n";

    for (
        const std::uint64_t workloadSize :
        WorkloadSizes
    ) {
        for (
            std::size_t repetition = 1;
            repetition <= Repetitions;
            ++repetition
        ) {
            std::uint64_t totalEventsApplied = 0;
            std::uint64_t totalRuntimeNs = 0;

            std::uint64_t totalDatagramsSubmitted = 0;
            std::uint64_t totalDropped = 0;
            std::uint64_t totalGapsDetected = 0;
            std::uint64_t totalEventsRecovered = 0;
            std::uint64_t totalRecoveryMisses = 0;
            std::uint64_t totalApplicationRejections = 0;
            std::uint64_t totalDecodeFailures = 0;
            std::uint64_t totalSequencerFailures = 0;

            bool everyPassHealthy = true;

            std::vector<std::uint64_t>
                recoveryDurationsNs;

            while (
                totalEventsApplied <
                workloadSize
            ) {
                const std::uint64_t remaining =
                    workloadSize -
                    totalEventsApplied;

                const std::size_t batchSize =
                    static_cast<std::size_t>(
                        std::min<std::uint64_t>(
                            remaining,
                            sourceEvents.size()
                        )
                    );

                /*
                    Each repeated pass is logically independent.
                    Resetting prevents repeated LOBSTER order IDs
                    from colliding.
                */
                resetOrderBook();

                std::vector<MarketEvent> batchEvents(
                    sourceEvents.begin(),
                    sourceEvents.begin() +
                        static_cast<
                            std::ptrdiff_t
                        >(batchSize)
                );

                NetworkQueue queue;

                InMemoryRecoverySource recoverySource{
                    batchEvents
                };

                RecoverySequencer sequencer{
                    batchEvents.front()
                        .sequence_number,
                    64,
                    recoverySource
                };

                MarketMakerConfig strategyConfig =
                    makeTestStrategyConfig();

                strategyConfig.participant_id =
                    Participant;

                strategyConfig.symbol_id =
                    Symbol;

                strategyConfig
                    .first_strategy_order_id =
                        9'000'000'000'000'000'000ULL;

                MarketMakerStrategy strategy{
                    strategyConfig,
                    makeTestRiskLimits()
                };

                PnlEngine pnl;

                pnl.registerAccount(
                    Participant,
                    1'000'000.0L
                );

                pnl.setSymbolName(
                    Symbol,
                    "AAPL_UDP_SCALABILITY"
                );

                /*
                    nullptr = no ordinary telemetry recorder
                    false   = strategy updates disabled
                    1       = irrelevant without telemetry
                    0       = no depth rows
                */
                UdpFeedProcessor processor{
                    queue,
                    sequencer,
                    strategy,
                    pnl,
                    nullptr,
                    Participant,
                    Symbol,
                    false,
                    1,
                    0
                };

                std::uint64_t passDatagramsSubmitted = 0;
                std::uint64_t passDropped = 0;

                bool waitingForRecovery = false;

                const std::uint64_t passStartNs =
                    steadyTimestampNs();

                for (
                    std::size_t index = 0;
                    index < batchEvents.size();
                    ++index
                ) {
                    const MarketEvent& event =
                        batchEvents[index];

                    const bool hasFollowingEvent =
                        index + 1 <
                        batchEvents.size();

                    const bool shouldDrop =
                        hasFollowingEvent &&
                        event.sequence_number %
                            DropEvery ==
                            0;

                    if (shouldDrop) {
                        ++passDropped;
                        waitingForRecovery = true;
                        continue;
                    }

                    if (!queue.tryPush(
                        makeUdpDatagram(event)
                    )) {
                        throw std::runtime_error(
                            "UDP queue rejected event " +
                            std::to_string(
                                event.sequence_number
                            )
                        );
                    }

                    ++passDatagramsSubmitted;

                    std::uint64_t recoveryStartNs = 0;

                    if (waitingForRecovery) {
                        recoveryStartNs =
                            steadyTimestampNs();
                    }

                    const std::size_t processed =
                        processor.processAvailable();

                    if (waitingForRecovery) {
                        const std::uint64_t
                            recoveryEndNs =
                                steadyTimestampNs();

                        recoveryDurationsNs.push_back(
                            recoveryEndNs -
                            recoveryStartNs
                        );

                        waitingForRecovery = false;
                    }

                    if (processed != 1) {
                        throw std::runtime_error(
                            "UDP scalability processor did "
                            "not process one datagram"
                        );
                    }

                    if (
                        sequencer.state() ==
                        FeedState::Failed
                    ) {
                        everyPassHealthy = false;

                        throw std::runtime_error(
                            "UDP scalability sequencer failed"
                        );
                    }
                }

                const std::uint64_t passEndNs =
                    steadyTimestampNs();

                totalRuntimeNs +=
                    passEndNs -
                    passStartNs;

                const RecoveryStatistics&
                    recoveryStatistics =
                        sequencer.statistics();

                const UdpProcessingStatistics&
                    processingStatistics =
                        processor.statistics();

                const std::uint64_t
                    expectedFinalSequence =
                        batchEvents.back()
                            .sequence_number +
                        1;

                if (
                    !sequencer.healthy() ||
                    sequencer.expectedSequence() !=
                        expectedFinalSequence
                ) {
                    everyPassHealthy = false;

                    throw std::runtime_error(
                        "UDP scalability pass did not "
                        "finish at the expected sequence"
                    );
                }

                if (
                    recoveryStatistics.events_applied !=
                    batchEvents.size()
                ) {
                    throw std::runtime_error(
                        "UDP scalability did not apply "
                        "every event"
                    );
                }

                if (
                    recoveryStatistics.gaps_detected !=
                    passDropped
                ) {
                    throw std::runtime_error(
                        "UDP gap count differs from "
                        "injected-loss count"
                    );
                }

                if (
                    recoveryStatistics
                        .events_recovered !=
                    passDropped
                ) {
                    throw std::runtime_error(
                        "UDP recovered-event count differs "
                        "from injected-loss count"
                    );
                }

                if (
                    queue.approximateSize() != 0
                ) {
                    throw std::runtime_error(
                        "UDP queue was not empty "
                        "after a pass"
                    );
                }

                checkBookInvariants();

                totalDatagramsSubmitted +=
                    passDatagramsSubmitted;

                totalDropped +=
                    passDropped;

                totalGapsDetected +=
                    recoveryStatistics
                        .gaps_detected;

                totalEventsRecovered +=
                    recoveryStatistics
                        .events_recovered;

                totalRecoveryMisses +=
                    recoveryStatistics
                        .recovery_misses;

                totalApplicationRejections +=
                    recoveryStatistics
                        .application_rejections;

                totalDecodeFailures +=
                    processingStatistics
                        .decode_failures;

                totalSequencerFailures +=
                    processingStatistics
                        .sequencer_failures;

                totalEventsApplied +=
                    batchEvents.size();
            }

            std::sort(
                recoveryDurationsNs.begin(),
                recoveryDurationsNs.end()
            );

            const std::uint64_t recoveryP50Ns =
                percentileFromSorted(
                    recoveryDurationsNs,
                    0.50L
                );

            const std::uint64_t recoveryP95Ns =
                percentileFromSorted(
                    recoveryDurationsNs,
                    0.95L
                );

            const std::uint64_t recoveryP99Ns =
                percentileFromSorted(
                    recoveryDurationsNs,
                    0.99L
                );

            const std::uint64_t recoveryMaximumNs =
                recoveryDurationsNs.empty()
                    ? 0
                    : recoveryDurationsNs.back();

            const long double runtimeSeconds =
                static_cast<long double>(
                    totalRuntimeNs
                ) /
                1'000'000'000.0L;

            const long double throughput =
                runtimeSeconds > 0.0L
                    ? static_cast<long double>(
                          workloadSize
                      ) /
                          runtimeSeconds
                    : 0.0L;

            const bool correctnessPassed =
                everyPassHealthy &&
                totalRecoveryMisses == 0 &&
                totalApplicationRejections == 0 &&
                totalDecodeFailures == 0 &&
                totalSequencerFailures == 0;

            output
                << workloadSize
                << ','
                << repetition
                << ','
                << std::fixed
                << std::setprecision(9)
                << static_cast<double>(
                       runtimeSeconds
                   )
                << ','
                << std::setprecision(3)
                << static_cast<double>(
                       throughput
                   )
                << ','
                << totalDatagramsSubmitted
                << ','
                << totalDropped
                << ','
                << totalGapsDetected
                << ','
                << totalEventsRecovered
                << ','
                << totalRecoveryMisses
                << ','
                << recoveryP50Ns
                << ','
                << recoveryP95Ns
                << ','
                << recoveryP99Ns
                << ','
                << recoveryMaximumNs
                << ','
                << totalApplicationRejections
                << ','
                << totalDecodeFailures
                << ','
                << totalSequencerFailures
                << ','
                << (
                       everyPassHealthy
                           ? 1
                           : 0
                   )
                << ','
                << (
                       correctnessPassed
                           ? 1
                           : 0
                   )
                << '\n';

            output.flush();

            std::cout
                << "UDP events: "
                << workloadSize
                << ", repetition: "
                << repetition
                << ", throughput: "
                << std::fixed
                << std::setprecision(0)
                << static_cast<double>(
                       throughput
                   )
                << " events/s, gaps: "
                << totalGapsDetected
                << ", recovered: "
                << totalEventsRecovered
                << ", misses: "
                << totalRecoveryMisses
                << ", recovery p99: "
                << recoveryP99Ns
                << " ns\n";
        }
    }

    output.flush();

    std::cout
        << "\nUDP scalability benchmark completed\n"
        << "Results: "
        << std::filesystem::absolute(
               outputPath
           )
        << '\n';
}

int main(
    int argc,
    char* argv[]
) {
    try {
        WinsockRuntime winsock;

        /*
            Isolated matching-engine latency benchmark.
        */
        if (
            argc >= 2 &&
            std::string{argv[1]} ==
                "--benchmark"

        ) {
            runMatchingEngineLatencyBenchmark();

            return EXIT_SUCCESS;
        }

        /*
            UDP gap-detection and recovery telemetry demo.
        */
        if (
            argc >= 2 &&
            std::string{argv[1]} ==
                "--udp-demo"
        ) {
            runUdpRecoveryTelemetryDemo();

            return EXIT_SUCCESS;
        }

        if (
    argc >= 2 &&
    std::string{argv[1]} ==
        "--udp-full"
) {
            runFullLobsterUdpRecoveryDemo();

            return EXIT_SUCCESS;
}
        if (
    argc >= 2 &&
    std::string{argv[1]} ==
        "--scalability"
) {
            runLobsterScalabilityBenchmark();

            return EXIT_SUCCESS;
}
        if (
    argc >= 2 &&
    std::string{argv[1]} ==
        "--udp-scalability"
) {
            runLobsterUdpScalabilityBenchmark();

            return EXIT_SUCCESS;
}

        /*
            Normal test and demonstration mode.
        */
        runExecuteOrderTests();
        runFeedReplayTests();
        runStrategyRiskTests();
        runPnlTests();
        runUdpRecoveryTests();

        runStrategyPnlDemo();
        runLobsterReplayDemo();

        return EXIT_SUCCESS;
    } catch (
        const std::exception& exception
    ) {
        std::cerr
            << "Fatal error: "
            << exception.what()
            << '\n';

        return EXIT_FAILURE;
    }
}