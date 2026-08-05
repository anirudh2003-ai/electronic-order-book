#include "binary_codec.hpp"
#include "lobster_loader.hpp"
#include "market_maker_strategy.hpp"
#include "order_book.hpp"
#include "pnl_engine.hpp"
#include "recovery.hpp"
#include "replay_engine.hpp"
#include "scenario_defaults.hpp"
#include "udp_feed.hpp"
#include "validation.hpp"

#include "analytics/telemetry_recorder.hpp"
#include "analytics/simulation_telemetry_bridge.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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