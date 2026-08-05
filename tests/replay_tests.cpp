#include "binary_codec.hpp"
#include "lobster_loader.hpp"
#include "replay_engine.hpp"
#include "test_support.hpp"
#include "validation.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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