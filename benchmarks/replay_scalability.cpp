#include "runners.hpp"

#include "benchmark_utils.hpp"
#include "lobster_loader.hpp"
#include "order_book.hpp"
#include "replay_engine.hpp"
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