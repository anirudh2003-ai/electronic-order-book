#pragma once

#include "analytics/telemetry_recorder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/*
    Include this file only after the project's Order, PnlEngine,
    MarketMakerStrategy, QuoteRefreshResult, RecoverySequencer,
    NetworkQueue, bids, asks, bestBid() and bestAsk() declarations
    are visible.
*/

namespace analytics {

inline std::string feedStateName(FeedState state) {
    switch (state) {
        case FeedState::Healthy:
            return "Healthy";
        case FeedState::Recovering:
            return "Recovering";
        case FeedState::Failed:
            return "Failed";
    }

    return "Unknown";
}

inline long double currentMidPrice() {
    const std::optional<Price> bid = bestBid();
    const std::optional<Price> ask = bestAsk();

    if (bid.has_value() && ask.has_value()) {
        return (
            static_cast<long double>(bid.value()) +
            static_cast<long double>(ask.value())
        ) / 2.0L;
    }

    if (bid.has_value()) {
        return static_cast<long double>(bid.value());
    }

    if (ask.has_value()) {
        return static_cast<long double>(ask.value());
    }

    return 0.0L;
}

class SimulationTelemetryBridge {
public:
    explicit SimulationTelemetryBridge(
        TelemetryRecorder& recorder
    )
        : recorder_(recorder) {
    }

    void recordQuoteRefresh(
        TimestampNs timestampNs,
        const QuoteRefreshResult& result
    ) {
        last_quotes_ = result;
        has_last_quotes_ = true;

        recordQuoteLeg(
            timestampNs,
            "BUY",
            result.bid
        );

        recordQuoteLeg(
            timestampNs,
            "SELL",
            result.ask
        );
    }

    void capture(
        TimestampNs timestampNs,
        ParticipantId participantId,
        SymbolId symbolId,
        const PnlEngine& pnl,
        const MarketMakerStrategy& strategy,
        const NetworkQueue* queue = nullptr,
        const RecoverySequencer* sequencer = nullptr,
        std::size_t depthLevels = 20
    ) {
        ++event_index_;

        const std::optional<Price> bid = bestBid();
        const std::optional<Price> ask = bestAsk();

        const Price bestBidValue =
            bid.value_or(0);

        const Price bestAskValue =
            ask.value_or(0);

        const long double mid =
            currentMidPrice();

        Price strategyBid = 0;
        Price strategyAsk = 0;

        if (has_last_quotes_) {
            strategyBid =
                last_quotes_.bid.accepted
                    ? last_quotes_.bid.price
                    : 0;

            strategyAsk =
                last_quotes_.ask.accepted
                    ? last_quotes_.ask.price
                    : 0;
        }

        const RecoveryStatistics emptyRecovery{};
        const RecoveryStatistics& recovery =
            sequencer != nullptr
                ? sequencer->statistics()
                : emptyRecovery;

        const std::string state =
            sequencer != nullptr
                ? feedStateName(sequencer->state())
                : "Healthy";

        recorder_.recordState(
            StateSample{
                timestampNs,
                wallClockTimestampNs(),
                event_index_,
                participantId,
                symbolId,
                bestBidValue,
                bestAskValue,
                mid,
                strategyBid,
                strategyAsk,
                pnl.realisedPnl(participantId),
                pnl.unrealisedPnl(participantId),
                pnl.totalPnl(participantId),
                pnl.cashBalance(participantId),
                pnl.accountEquity(participantId),
                pnl.position(participantId, symbolId),
                strategy.riskManager()
                    .limits()
                    .maximum_absolute_position,
                (
                    bid.has_value() && ask.has_value()
                        ? ask.value() - bid.value()
                        : 0
                ),
                (
                    strategyBid > 0 && strategyAsk > 0
                        ? strategyAsk - strategyBid
                        : 0
                ),
                (
                    queue != nullptr
                        ? queue->approximateSize()
                        : 0
                ),
                recovery.events_applied,
                recovery.gaps_detected,
                recovery.events_recovered,
                recovery.recovery_misses,
                state
            }
        );

        recordDepth(timestampNs, depthLevels);
        recordNewFills(timestampNs, strategy);
    }

    void recordRecoveryStart(
        TimestampNs timestampNs,
        std::uint64_t expectedSequence,
        std::uint64_t receivedSequence,
        std::size_t bufferDepth
    ) {
        recovery_start_wall_ns_ =
            wallClockTimestampNs();

        const std::uint64_t gapSize =
            receivedSequence > expectedSequence
                ? receivedSequence - expectedSequence
                : 0;

        recorder_.recordRecovery(
            RecoverySample{
                timestampNs,
                recovery_start_wall_ns_,
                event_index_,
                "gap_start",
                "Recovering",
                expectedSequence,
                receivedSequence,
                gapSize,
                static_cast<std::uint64_t>(bufferDepth),
                0
            }
        );
    }

    void recordRecoveryEnd(
        TimestampNs timestampNs,
        std::uint64_t expectedSequence,
        std::size_t bufferDepth
    ) {
        const std::uint64_t endWall =
            wallClockTimestampNs();

        const std::uint64_t duration =
            recovery_start_wall_ns_ == 0
                ? 0
                : endWall - recovery_start_wall_ns_;

        recorder_.recordRecovery(
            RecoverySample{
                timestampNs,
                endWall,
                event_index_,
                "gap_end",
                "Healthy",
                expectedSequence,
                expectedSequence,
                0,
                static_cast<std::uint64_t>(bufferDepth),
                duration
            }
        );

        recovery_start_wall_ns_ = 0;
    }

    void recordRecoveryFailure(
        TimestampNs timestampNs,
        std::uint64_t expectedSequence,
        std::size_t bufferDepth
    ) {
        const std::uint64_t endWall =
            wallClockTimestampNs();

        const std::uint64_t duration =
            recovery_start_wall_ns_ == 0
                ? 0
                : endWall - recovery_start_wall_ns_;

        recorder_.recordRecovery(
            RecoverySample{
                timestampNs,
                endWall,
                event_index_,
                "gap_failed",
                "Failed",
                expectedSequence,
                expectedSequence,
                0,
                static_cast<std::uint64_t>(bufferDepth),
                duration
            }
        );

        recovery_start_wall_ns_ = 0;
    }

    std::uint64_t eventIndex() const {
        return event_index_;
    }

private:
    void recordQuoteLeg(
        TimestampNs timestampNs,
        const std::string& side,
        const QuoteLegResult& leg
    ) {
        recorder_.recordQuote(
            QuoteSample{
                timestampNs,
                event_index_,
                leg.order_id.value_or(0),
                side,
                leg.price,
                leg.quantity,
                leg.accepted
            }
        );
    }

    void recordDepth(
        TimestampNs timestampNs,
        std::size_t depthLevels
    ) {
        std::size_t level = 0;

        for (const auto& [price, priceLevel] : bids) {
            if (level >= depthLevels) {
                break;
            }

            recorder_.recordDepth(
                DepthSample{
                    timestampNs,
                    event_index_,
                    "BID",
                    price,
                    priceLevel.total_quantity,
                    level
                }
            );

            ++level;
        }

        level = 0;

        for (const auto& [price, priceLevel] : asks) {
            if (level >= depthLevels) {
                break;
            }

            recorder_.recordDepth(
                DepthSample{
                    timestampNs,
                    event_index_,
                    "ASK",
                    price,
                    priceLevel.total_quantity,
                    level
                }
            );

            ++level;
        }
    }

    void recordNewFills(
        TimestampNs timestampNs,
        const MarketMakerStrategy& strategy
    ) {
        const std::vector<InventoryFill>& fills =
            strategy.inventory().fills();

        if (fill_cursor_ > fills.size()) {
            fill_cursor_ = 0;
        }

        const long double mid =
            currentMidPrice();

        while (fill_cursor_ < fills.size()) {
            const InventoryFill& fill =
                fills[fill_cursor_];

            recorder_.recordFill(
                FillSample{
                    timestampNs,
                    event_index_,
                    fill.order_id,
                    fill.side == Side::Buy
                        ? "BUY"
                        : "SELL",
                    fill.price,
                    fill.quantity,
                    fill.was_maker,
                    mid
                }
            );

            ++fill_cursor_;
        }
    }

    TelemetryRecorder& recorder_;

    std::uint64_t event_index_{0};
    std::size_t fill_cursor_{0};

    bool has_last_quotes_{false};
    QuoteRefreshResult last_quotes_{};

    std::uint64_t recovery_start_wall_ns_{0};
};

} // namespace analytics