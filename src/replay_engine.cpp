#include "replay_engine.hpp"

#include "order_book.hpp"

#include <iostream>
#include <limits>
#include <sstream>


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