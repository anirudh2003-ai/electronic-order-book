#include "runners.hpp"

#include "binary_codec.hpp"
#include "order_book.hpp"
#include "validation.hpp"

#include "analytics/telemetry_recorder.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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