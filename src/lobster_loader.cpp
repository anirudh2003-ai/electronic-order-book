#include "lobster_loader.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

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