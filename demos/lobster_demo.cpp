#include "binary_codec.hpp"
#include "lobster_loader.hpp"
#include "replay_engine.hpp"
#include "runners.hpp"

#include "analytics/telemetry_recorder.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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