#pragma once

#include "binary_codec.hpp"
#include "market_event.hpp"
#include "order_book.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

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

void printParseStatistics(
    const ParseResult& result
);

void printReplayStatistics(
    const ReplayResult& result
);

std::string createOrderBookSnapshot();

ApplyEventResult applyMarketEvent(
    const MarketEvent& event
);

void printParseStatistics(
    const ParseResult& result
);

void printReplayStatistics(
    const ReplayResult& result
);

std::string createOrderBookSnapshot();