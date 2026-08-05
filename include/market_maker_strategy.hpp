#pragma once

#include "order_book.hpp"
#include "risk_management.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

struct MarketMakerConfig {
    Quantity quote_quantity{10};
    Price half_spread_ticks{20};
    Price inventory_skew_per_unit{1};
    Position target_position{0};
    Price tick_size{1};

    OrderId first_strategy_order_id{
        1'000'000'000ULL
    };

    ParticipantId participant_id{5001};
    SymbolId symbol_id{DefaultSymbol};
};

struct QuoteLegResult {
    bool accepted{false};
    std::optional<OrderId> order_id;
    Price price{0};
    Quantity quantity{0};
};

struct QuoteRefreshResult {
    bool market_accepted{false};

    Price fair_price{0};
    Price inventory_skew{0};

    QuoteLegResult bid;
    QuoteLegResult ask;
};


// ============================================================
// MARKET-MAKING STRATEGY
// ============================================================

class MarketMakerStrategy {
public:
    MarketMakerStrategy(
        MarketMakerConfig config,
        RiskLimits riskLimits
    )
        : config_(config),
          risk_manager_(riskLimits),
          next_order_id_(
              config.first_strategy_order_id
          ) {
        if (config_.quote_quantity == 0) {
            throw std::invalid_argument(
                "Quote quantity must be positive"
            );
        }

        if (config_.half_spread_ticks <= 0) {
            throw std::invalid_argument(
                "Half spread must be positive"
            );
        }

        if (config_.tick_size <= 0) {
            throw std::invalid_argument(
                "Tick size must be positive"
            );
        }

        if (
            config_.inventory_skew_per_unit < 0
        ) {
            throw std::invalid_argument(
                "Inventory skew cannot be negative"
            );
        }
    }

    void onMarketUpdate(
        TimestampNs timestampNs
    ) {
        /*
            Do not move the last market-update timestamp
            backwards when replaying bad data.
        */
        if (
            timestampNs >
            last_market_update_ns_
        ) {
            last_market_update_ns_ = timestampNs;
        }
    }

    void processTrades() {
        inventory_tracker_.processNewTrades(
            tradeHistory,
            owned_order_sides_
        );

        /*
            A fully filled quote will no longer be present
            in orderIndex.
        */
        if (
            active_bid_order_id_.has_value() &&
            !containsOrder(
                active_bid_order_id_.value()
            )
        ) {
            active_bid_order_id_.reset();
        }

        if (
            active_ask_order_id_.has_value() &&
            !containsOrder(
                active_ask_order_id_.value()
            )
        ) {
            active_ask_order_id_.reset();
        }
    }

    QuoteRefreshResult refreshQuotes(
        TimestampNs currentTimestamp
    ) {
        processTrades();

        /*
            Remove old quotes before calculating new quotes.

            This prevents the strategy from using its own
            quotes as the market midpoint.
        */
        cancelActiveQuotes();

        std::optional<Price> marketBestBid =
            bestBid();

        std::optional<Price> marketBestAsk =
            bestAsk();

        QuoteRefreshResult refreshResult;

        RiskCheckResult marketCheck =
            risk_manager_.validateMarket(
                currentTimestamp,
                last_market_update_ns_,
                marketBestBid,
                marketBestAsk
            );

        if (!marketCheck.accepted) {
            rejection_reporter_.recordMarketRejection(
                currentTimestamp,
                marketCheck.reason,
                marketCheck.message
            );

            return refreshResult;
        }

        refreshResult.market_accepted = true;

        Price fairPrice = 0;
        Price inventorySkew = 0;
        Price bidPrice = 0;
        Price askPrice = 0;

        bool generated =
            generateQuotePrices(
                marketBestBid.value(),
                marketBestAsk.value(),
                fairPrice,
                inventorySkew,
                bidPrice,
                askPrice
            );

        if (!generated) {
            rejection_reporter_.recordMarketRejection(
                currentTimestamp,
                RejectionReason::QuoteWouldCross,
                "Unable to generate valid passive quotes"
            );

            refreshResult.market_accepted = false;
            return refreshResult;
        }

        refreshResult.fair_price = fairPrice;
        refreshResult.inventory_skew =
            inventorySkew;

        Order bidOrder{
            generateOrderId(),
            Side::Buy,
            bidPrice,
            config_.quote_quantity,
            config_.participant_id,
            config_.symbol_id
        };

        Order askOrder{
            generateOrderId(),
            Side::Sell,
            askPrice,
            config_.quote_quantity,
            config_.participant_id,
            config_.symbol_id
        };

        refreshResult.bid =
            submitQuote(
                bidOrder,
                currentTimestamp
            );

        refreshResult.ask =
            submitQuote(
                askOrder,
                currentTimestamp
            );

        return refreshResult;
    }

    void cancelActiveQuotes() {
        processTrades();

        cancelQuote(active_bid_order_id_);
        cancelQuote(active_ask_order_id_);
    }

    void activateKillSwitch(
        TimestampNs timestampNs,
        const std::string& reason
    ) {
        risk_manager_.activateKillSwitch(reason);

        cancelActiveQuotes();

        rejection_reporter_.recordMarketRejection(
            timestampNs,
            RejectionReason::KillSwitchActive,
            reason
        );
    }

    void deactivateKillSwitch() {
        risk_manager_.deactivateKillSwitch();
    }

    void resetStrategy() {
        cancelActiveQuotes();

        inventory_tracker_.reset();
        rejection_reporter_.clear();

        owned_order_sides_.clear();

        active_bid_order_id_.reset();
        active_ask_order_id_.reset();

        last_market_update_ns_ = 0;

        next_order_id_ =
            config_.first_strategy_order_id;

        risk_manager_.deactivateKillSwitch();
    }

    const InventoryTracker& inventory() const {
        return inventory_tracker_;
    }

    const RejectionReporter&
    rejectionReporter() const {
        return rejection_reporter_;
    }

    const RiskManager& riskManager() const {
        return risk_manager_;
    }

    std::optional<OrderId>
    activeBidOrderId() const {
        return active_bid_order_id_;
    }

    std::optional<OrderId>
    activeAskOrderId() const {
        return active_ask_order_id_;
    }

    TimestampNs lastMarketUpdate() const {
        return last_market_update_ns_;
    }

private:
    bool generateQuotePrices(
        Price marketBestBid,
        Price marketBestAsk,
        Price& fairPrice,
        Price& inventorySkew,
        Price& bidPrice,
        Price& askPrice
    ) const {
        if (
            marketBestBid <= 0 ||
            marketBestAsk <= 0 ||
            marketBestBid >= marketBestAsk
        ) {
            return false;
        }

        long double fairValue =
            (
                static_cast<long double>(
                    marketBestBid
                ) +
                static_cast<long double>(
                    marketBestAsk
                )
            ) /
            2.0L;

        Position inventoryDeviation =
            inventory_tracker_.position() -
            config_.target_position;

        long double skewValue =
            static_cast<long double>(
                inventoryDeviation
            ) *
            static_cast<long double>(
                config_.inventory_skew_per_unit
            );

        /*
            Positive inventory means we are too long.

            Therefore:
            adjusted fair value moves down,
            bid becomes less aggressive,
            ask becomes more aggressive.
        */
        long double adjustedFairValue =
            fairValue - skewValue;

        long double rawBid =
            adjustedFairValue -
            static_cast<long double>(
                config_.half_spread_ticks
            );

        long double rawAsk =
            adjustedFairValue +
            static_cast<long double>(
                config_.half_spread_ticks
            );

        /*
            Keep quotes passive.

            Buy quote must remain below best ask.
            Sell quote must remain above best bid.
        */
        long double maximumPassiveBid =
            static_cast<long double>(
                marketBestAsk
            ) -
            static_cast<long double>(
                config_.tick_size
            );

        long double minimumPassiveAsk =
            static_cast<long double>(
                marketBestBid
            ) +
            static_cast<long double>(
                config_.tick_size
            );

        rawBid = std::min(
            rawBid,
            maximumPassiveBid
        );

        rawAsk = std::max(
            rawAsk,
            minimumPassiveAsk
        );

        std::optional<Price> roundedBid =
            roundDownToTick(rawBid);

        std::optional<Price> roundedAsk =
            roundUpToTick(rawAsk);

        if (
            !roundedBid.has_value() ||
            !roundedAsk.has_value()
        ) {
            return false;
        }

        if (
            roundedBid.value() <= 0 ||
            roundedAsk.value() <= 0 ||
            roundedBid.value() >=
                roundedAsk.value()
        ) {
            return false;
        }

        std::optional<Price> convertedFair =
            convertToPrice(
                std::round(fairValue)
            );

        std::optional<Price> convertedSkew =
            convertSignedToPrice(
                std::round(skewValue)
            );

        if (
            !convertedFair.has_value() ||
            !convertedSkew.has_value()
        ) {
            return false;
        }

        fairPrice = convertedFair.value();
        inventorySkew =
            convertedSkew.value();

        bidPrice = roundedBid.value();
        askPrice = roundedAsk.value();

        return true;
    }

    QuoteLegResult submitQuote(
        const Order& order,
        TimestampNs timestampNs
    ) {
        QuoteLegResult result;

        result.order_id = order.id;
        result.price = order.price;
        result.quantity = order.quantity;

        RiskCheckResult riskCheck =
            risk_manager_.validateOrder(
                order,
                inventory_tracker_.position(),
                timestampNs,
                last_market_update_ns_,
                bestBid(),
                bestAsk()
            );

        if (!riskCheck.accepted) {
            rejection_reporter_.recordOrderRejection(
                timestampNs,
                order,
                riskCheck.reason,
                riskCheck.message
            );

            return result;
        }

        /*
            Register ownership before execution.

            This ensures an immediately executed strategy order
            can still be identified in tradeHistory.
        */
        owned_order_sides_[order.id] =
            order.side;

        ExecutionResult execution =
            executeOrder(order);

        if (!execution.accepted) {
            owned_order_sides_.erase(order.id);

            rejection_reporter_.recordOrderRejection(
                timestampNs,
                order,
                RejectionReason::OrderBookRejected,
                "executeOrder returned accepted=false"
            );

            return result;
        }

        result.accepted = true;

        processTrades();

        /*
            If quantity remains, the order is resting.

            If it was fully executed immediately, it will not
            exist in orderIndex.
        */
        if (
            execution.remaining_quantity > 0 &&
            containsOrder(order.id)
        ) {
            if (order.side == Side::Buy) {
                active_bid_order_id_ =
                    order.id;
            } else {
                active_ask_order_id_ =
                    order.id;
            }
        }

        return result;
    }

    void cancelQuote(
        std::optional<OrderId>& activeOrderId
    ) {
        if (!activeOrderId.has_value()) {
            return;
        }

        OrderId orderId =
            activeOrderId.value();

        if (containsOrder(orderId)) {
            cancelOrder(orderId);
        }

        activeOrderId.reset();
    }

    OrderId generateOrderId() {
        while (
            orderIndex.contains(next_order_id_) ||
            owned_order_sides_.contains(
                next_order_id_
            )
        ) {
            if (
                next_order_id_ ==
                std::numeric_limits<OrderId>::max()
            ) {
                throw std::overflow_error(
                    "Strategy order IDs exhausted"
                );
            }

            ++next_order_id_;
        }

        OrderId generatedId =
            next_order_id_;

        if (
            next_order_id_ !=
            std::numeric_limits<OrderId>::max()
        ) {
            ++next_order_id_;
        }

        return generatedId;
    }

    std::optional<Price> roundDownToTick(
        long double value
    ) const {
        long double tick =
            static_cast<long double>(
                config_.tick_size
            );

        long double rounded =
            std::floor(value / tick) *
            tick;

        return convertToPrice(rounded);
    }

    std::optional<Price> roundUpToTick(
        long double value
    ) const {
        long double tick =
            static_cast<long double>(
                config_.tick_size
            );

        long double rounded =
            std::ceil(value / tick) *
            tick;

        return convertToPrice(rounded);
    }

    static std::optional<Price> convertToPrice(
        long double value
    ) {
        long double minimum =
            1.0L;

        long double maximum =
            static_cast<long double>(
                std::numeric_limits<Price>::max()
            );

        if (
            value < minimum ||
            value > maximum
        ) {
            return std::nullopt;
        }

        return static_cast<Price>(value);
    }

    static std::optional<Price>
    convertSignedToPrice(
        long double value
    ) {
        long double minimum =
            static_cast<long double>(
                std::numeric_limits<Price>::min()
            );

        long double maximum =
            static_cast<long double>(
                std::numeric_limits<Price>::max()
            );

        if (
            value < minimum ||
            value > maximum
        ) {
            return std::nullopt;
        }

        return static_cast<Price>(value);
    }

    MarketMakerConfig config_;

    RiskManager risk_manager_;
    RejectionReporter rejection_reporter_;
    InventoryTracker inventory_tracker_;

    /*
        Keep all previously submitted strategy IDs.

        Filled orders are retained here so their trades can
        still be attributed to the strategy.
    */
    std::unordered_map<OrderId, Side>
        owned_order_sides_;

    std::optional<OrderId>
        active_bid_order_id_;

    std::optional<OrderId>
        active_ask_order_id_;

    OrderId next_order_id_;

    TimestampNs last_market_update_ns_{0};
};