#include "runners.hpp"

#include "benchmark_utils.hpp"
#include "binary_codec.hpp"
#include "lobster_loader.hpp"
#include "market_maker_strategy.hpp"
#include "order_book.hpp"
#include "pnl_engine.hpp"
#include "recovery.hpp"
#include "scenario_defaults.hpp"
#include "udp_feed.hpp"
#include "validation.hpp"

#include "analytics/telemetry_recorder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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