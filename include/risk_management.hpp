#pragma once

#include "order_book.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>


using Position = std::int64_t;


Side oppositeSide(Side side);

std::string sideToString(Side side);


enum class RejectionReason {
    None,
    KillSwitchActive,
    MarketUnavailable,
    StaleMarket,
    InvalidTimestamp,
    CrossedMarket,
    InvalidPrice,
    InvalidQuantity,
    MaximumOrderSizeExceeded,
    MaximumPositionExceeded,
    MaximumNotionalExceeded,
    QuoteWouldCross,
    DuplicateOrderId,
    OrderBookRejected
};


std::string rejectionReasonToString(
    RejectionReason reason
);


// ============================================================
// REJECTION REPORTING
// ============================================================

struct RejectionRecord {
    TimestampNs timestamp_ns;
    RejectionReason reason;
    std::optional<Order> order;
    std::string details;
};


class RejectionReporter {
public:
    void recordOrderRejection(
        TimestampNs timestampNs,
        const Order& order,
        RejectionReason reason,
        const std::string& details
    ) {
        records_.push_back(
            RejectionRecord{
                timestampNs,
                reason,
                order,
                details
            }
        );
    }

    void recordMarketRejection(
        TimestampNs timestampNs,
        RejectionReason reason,
        const std::string& details
    ) {
        records_.push_back(
            RejectionRecord{
                timestampNs,
                reason,
                std::nullopt,
                details
            }
        );
    }

    const std::vector<RejectionRecord>&
    records() const {
        return records_;
    }

    std::size_t count() const {
        return records_.size();
    }

    bool empty() const {
        return records_.empty();
    }

    void clear() {
        records_.clear();
    }

    void print() const {
        std::cout
            << "\nRejected-order report\n";

        std::cout
            << "---------------------\n";

        if (records_.empty()) {
            std::cout
                << "No rejected actions.\n";

            return;
        }

        for (
            const RejectionRecord& record :
            records_
        ) {
            std::cout
                << "Timestamp: "
                << record.timestamp_ns
                << " | Reason: "
                << rejectionReasonToString(
                       record.reason
                   );

            if (record.order.has_value()) {
                const Order& order =
                    record.order.value();

                std::cout
                    << " | Order ID: "
                    << order.id
                    << " | Side: "
                    << sideToString(
                           order.side
                       )
                    << " | Price: "
                    << order.price
                    << " | Quantity: "
                    << order.quantity;
            }

            if (!record.details.empty()) {
                std::cout
                    << " | Details: "
                    << record.details;
            }

            std::cout << '\n';
        }
    }

private:
    std::vector<RejectionRecord> records_;
};


// ============================================================
// INVENTORY TRACKING
// ============================================================

struct InventoryFill {
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;
    bool was_maker;
};


class InventoryTracker {
public:
    void processNewTrades(
        const std::vector<Trade>& trades,
        const std::unordered_map<
            OrderId,
            Side
        >& ownedOrders
    ) {
        /*
            tradeHistory may have been cleared by
            resetOrderBook().

            Reset the cursor if the new history is
            shorter.
        */
        if (
            processed_trade_count_ >
            trades.size()
        ) {
            processed_trade_count_ = 0;
        }

        while (
            processed_trade_count_ <
            trades.size()
        ) {
            const Trade& trade =
                trades[
                    processed_trade_count_
                ];

            auto makerIterator =
                ownedOrders.find(
                    trade.maker_order_id
                );

            if (
                makerIterator !=
                ownedOrders.end()
            ) {
                Side makerSide =
                    oppositeSide(
                        trade.aggressor_side
                    );

                applyFill(
                    trade.maker_order_id,
                    makerSide,
                    trade.price,
                    trade.quantity,
                    true
                );
            }

            auto takerIterator =
                ownedOrders.find(
                    trade.taker_order_id
                );

            if (
                takerIterator !=
                ownedOrders.end()
            ) {
                Side takerSide =
                    trade.aggressor_side;

                applyFill(
                    trade.taker_order_id,
                    takerSide,
                    trade.price,
                    trade.quantity,
                    false
                );
            }

            ++processed_trade_count_;
        }
    }

    Position position() const {
        return position_;
    }

    std::uint64_t totalBought() const {
        return total_bought_;
    }

    std::uint64_t totalSold() const {
        return total_sold_;
    }

    const std::vector<InventoryFill>&
    fills() const {
        return fills_;
    }

    void reset() {
        position_ = 0;
        total_bought_ = 0;
        total_sold_ = 0;

        processed_trade_count_ = 0;

        fills_.clear();
    }

private:
    void applyFill(
        OrderId orderId,
        Side side,
        Price price,
        Quantity quantity,
        bool wasMaker
    ) {
        Position signedQuantity =
            static_cast<Position>(
                quantity
            );

        if (side == Side::Buy) {
            position_ += signedQuantity;
            total_bought_ += quantity;
        } else {
            position_ -= signedQuantity;
            total_sold_ += quantity;
        }

        fills_.push_back(
            InventoryFill{
                orderId,
                side,
                price,
                quantity,
                wasMaker
            }
        );
    }

    Position position_{0};

    std::uint64_t total_bought_{0};
    std::uint64_t total_sold_{0};

    std::size_t processed_trade_count_{0};

    std::vector<InventoryFill> fills_;
};


// ============================================================
// RISK MANAGEMENT
// ============================================================

struct RiskLimits {
    Quantity maximum_order_size{100};

    Position maximum_absolute_position{
        1000
    };

    /*
        Uses the same units as Price × Quantity.

        For example, if price is stored in pence:

        10000 × 100 =
        1,000,000 pence of exposure.
    */
    long double maximum_notional_exposure{
        10'000'000.0L
    };

    /*
        One second by default.
    */
    TimestampNs maximum_market_age_ns{
        1'000'000'000ULL
    };
};


struct RiskCheckResult {
    bool accepted;
    RejectionReason reason;
    std::string message;
};


RiskCheckResult acceptRiskCheck();

RiskCheckResult rejectRiskCheck(
    RejectionReason reason,
    const std::string& message
);


class RiskManager {
public:
    explicit RiskManager(
        RiskLimits limits = {}
    )
        : limits_(limits) {
    }

    RiskCheckResult validateMarket(
        TimestampNs currentTimestamp,
        TimestampNs lastMarketUpdateTimestamp,
        std::optional<Price> currentBestBid,
        std::optional<Price> currentBestAsk
    ) const {
        if (kill_switch_active_) {
            return rejectRiskCheck(
                RejectionReason::
                    KillSwitchActive,
                kill_switch_reason_
            );
        }

        if (
            !currentBestBid.has_value() ||
            !currentBestAsk.has_value()
        ) {
            return rejectRiskCheck(
                RejectionReason::
                    MarketUnavailable,
                "Both a best bid and best ask "
                "are required"
            );
        }

        if (
            currentBestBid.value() >=
            currentBestAsk.value()
        ) {
            return rejectRiskCheck(
                RejectionReason::
                    CrossedMarket,
                "Best bid must be below best ask"
            );
        }

        if (
            lastMarketUpdateTimestamp == 0
        ) {
            return rejectRiskCheck(
                RejectionReason::
                    MarketUnavailable,
                "No market update timestamp "
                "is available"
            );
        }

        if (
            currentTimestamp <
            lastMarketUpdateTimestamp
        ) {
            return rejectRiskCheck(
                RejectionReason::
                    InvalidTimestamp,
                "Current timestamp is before "
                "the last market update"
            );
        }

        TimestampNs marketAge =
            currentTimestamp -
            lastMarketUpdateTimestamp;

        if (
            marketAge >
            limits_.maximum_market_age_ns
        ) {
            return rejectRiskCheck(
                RejectionReason::
                    StaleMarket,
                "Market data age is " +
                std::to_string(
                    marketAge
                ) +
                " ns"
            );
        }

        return acceptRiskCheck();
    }

    RiskCheckResult validateOrder(
        const Order& order,
        Position currentPosition,
        TimestampNs currentTimestamp,
        TimestampNs lastMarketUpdateTimestamp,
        std::optional<Price> currentBestBid,
        std::optional<Price> currentBestAsk
    ) const {
        RiskCheckResult marketCheck =
            validateMarket(
                currentTimestamp,
                lastMarketUpdateTimestamp,
                currentBestBid,
                currentBestAsk
            );

        if (!marketCheck.accepted) {
            return marketCheck;
        }

        if (order.price <= 0) {
            return rejectRiskCheck(
                RejectionReason::
                    InvalidPrice,
                "Order price must be positive"
            );
        }

        if (order.quantity == 0) {
            return rejectRiskCheck(
                RejectionReason::
                    InvalidQuantity,
                "Order quantity must be positive"
            );
        }

        if (
            order.quantity >
            limits_.maximum_order_size
        ) {
            return rejectRiskCheck(
                RejectionReason::
                    MaximumOrderSizeExceeded,
                "Quantity " +
                std::to_string(
                    order.quantity
                ) +
                " exceeds maximum " +
                std::to_string(
                    limits_
                        .maximum_order_size
                )
            );
        }

        long double projectedPosition =
            static_cast<long double>(
                currentPosition
            );

        if (order.side == Side::Buy) {
            projectedPosition +=
                static_cast<long double>(
                    order.quantity
                );
        } else {
            projectedPosition -=
                static_cast<long double>(
                    order.quantity
                );
        }

        if (
            std::fabs(projectedPosition) >
            static_cast<long double>(
                limits_
                    .maximum_absolute_position
            )
        ) {
            return rejectRiskCheck(
                RejectionReason::
                    MaximumPositionExceeded,
                "Projected position would be " +
                std::to_string(
                    static_cast<long long>(
                        projectedPosition
                    )
                )
            );
        }

        long double projectedNotional =
            std::fabs(projectedPosition) *
            static_cast<long double>(
                order.price
            );

        if (
            projectedNotional >
            limits_
                .maximum_notional_exposure
        ) {
            return rejectRiskCheck(
                RejectionReason::
                    MaximumNotionalExceeded,
                "Projected notional would be " +
                std::to_string(
                    static_cast<double>(
                        projectedNotional
                    )
                )
            );
        }

        if (
            orderIndex.contains(
                order.id
            )
        ) {
            return rejectRiskCheck(
                RejectionReason::
                    DuplicateOrderId,
                "Order ID already exists "
                "in the book"
            );
        }

        return acceptRiskCheck();
    }

    void activateKillSwitch(
        const std::string& reason
    ) {
        kill_switch_active_ = true;
        kill_switch_reason_ = reason;
    }

    void deactivateKillSwitch() {
        kill_switch_active_ = false;
        kill_switch_reason_.clear();
    }

    bool killSwitchActive() const {
        return kill_switch_active_;
    }

    const std::string&
    killSwitchReason() const {
        return kill_switch_reason_;
    }

    const RiskLimits& limits() const {
        return limits_;
    }

private:
    RiskLimits limits_;

    bool kill_switch_active_{false};
    std::string kill_switch_reason_;
};