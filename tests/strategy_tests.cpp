#include "market_maker_strategy.hpp"
#include "order_book.hpp"
#include "scenario_defaults.hpp"
#include "test_support.hpp"
#include "validation.hpp"

#include <iostream>

void addBasicExternalMarket() {
    CHECK(
        addOrder(
            Order{
                1,
                Side::Buy,
                9900,
                100
            }
        )
    );

    CHECK(
        addOrder(
            Order{
                2,
                Side::Sell,
                10100,
                100
            }
        )
    );
}

void testMarketMakerGeneratesBidAndAsk() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerStrategy strategy(
        makeTestStrategyConfig(),
        makeTestRiskLimits()
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult result =
        strategy.refreshQuotes(1000);

    CHECK(result.market_accepted);

    CHECK(result.fair_price == 10000);
    CHECK(result.inventory_skew == 0);

    CHECK(result.bid.accepted);
    CHECK(result.ask.accepted);

    CHECK(result.bid.price == 9980);
    CHECK(result.ask.price == 10020);

    CHECK(
        result.bid.order_id.has_value()
    );

    CHECK(
        result.ask.order_id.has_value()
    );

    CHECK(
        orderIndex.contains(
            result.bid.order_id.value()
        )
    );

    CHECK(
        orderIndex.contains(
            result.ask.order_id.value()
        )
    );

    CHECK(strategy.inventory().position() == 0);

    checkBookInvariants();
}

void testInventoryChangesQuotePrices() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerStrategy strategy(
        makeTestStrategyConfig(),
        makeTestRiskLimits()
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult firstQuotes =
        strategy.refreshQuotes(1000);

    CHECK(firstQuotes.bid.accepted);
    CHECK(firstQuotes.bid.price == 9980);

    /*
        External seller executes against our bid.

        Our strategy bought 10 units.
    */
    ExecutionResult externalExecution =
        executeOrder(
            Order{
                100,
                Side::Sell,
                firstQuotes.bid.price,
                10
            }
        );

    CHECK(externalExecution.accepted);
    CHECK(
        externalExecution.executed_quantity ==
        10
    );

    strategy.processTrades();

    CHECK(strategy.inventory().position() == 10);
    CHECK(strategy.inventory().totalBought() == 10);

    strategy.onMarketUpdate(1100);

    QuoteRefreshResult secondQuotes =
        strategy.refreshQuotes(1100);

    /*
        Position = +10
        Skew per unit = 2

        Inventory skew = 20 ticks.

        Original fair value = 10000
        Adjusted fair value = 9980

        New bid = 9960
        New ask = 10000
    */

    CHECK(secondQuotes.market_accepted);
    CHECK(secondQuotes.inventory_skew == 20);

    CHECK(secondQuotes.bid.price == 9960);
    CHECK(secondQuotes.ask.price == 10000);

    checkBookInvariants();
}

void testMaximumOrderSizeRejectsQuotes() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerConfig config =
        makeTestStrategyConfig();

    config.quote_quantity = 10;

    RiskLimits limits =
        makeTestRiskLimits();

    limits.maximum_order_size = 5;

    MarketMakerStrategy strategy(
        config,
        limits
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult result =
        strategy.refreshQuotes(1000);

    CHECK(result.market_accepted);

    CHECK(!result.bid.accepted);
    CHECK(!result.ask.accepted);

    CHECK(
        strategy.rejectionReporter().count() ==
        2
    );

    CHECK(!strategy.activeBidOrderId().has_value());
    CHECK(!strategy.activeAskOrderId().has_value());

    checkBookInvariants();
}


void testMaximumPositionRejectsQuotes() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerConfig config =
        makeTestStrategyConfig();

    config.quote_quantity = 10;

    RiskLimits limits =
        makeTestRiskLimits();

    limits.maximum_absolute_position = 5;

    MarketMakerStrategy strategy(
        config,
        limits
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult result =
        strategy.refreshQuotes(1000);

    CHECK(result.market_accepted);

    /*
        Buying 10 would produce position +10.
        Selling 10 would produce position -10.

        Both exceed maximum absolute position 5.
    */
    CHECK(!result.bid.accepted);
    CHECK(!result.ask.accepted);

    CHECK(
        strategy.rejectionReporter().count() ==
        2
    );

    checkBookInvariants();
}


void testMaximumNotionalRejectsQuotes() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerConfig config =
        makeTestStrategyConfig();

    config.quote_quantity = 10;

    RiskLimits limits =
        makeTestRiskLimits();

    /*
        Approximate projected notional:

        10 × 10,000 = 100,000
    */
    limits.maximum_notional_exposure =
        50'000.0L;

    MarketMakerStrategy strategy(
        config,
        limits
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult result =
        strategy.refreshQuotes(1000);

    CHECK(result.market_accepted);

    CHECK(!result.bid.accepted);
    CHECK(!result.ask.accepted);

    CHECK(
        strategy.rejectionReporter().count() ==
        2
    );

    checkBookInvariants();
}


void testStaleMarketPreventsQuoting() {
    resetOrderBook();
    addBasicExternalMarket();

    RiskLimits limits =
        makeTestRiskLimits();

    limits.maximum_market_age_ns = 100;

    MarketMakerStrategy strategy(
        makeTestStrategyConfig(),
        limits
    );

    strategy.onMarketUpdate(1000);

    /*
        Current time is 1200.

        Market age = 200 ns.
        Maximum permitted age = 100 ns.
    */
    QuoteRefreshResult result =
        strategy.refreshQuotes(1200);

    CHECK(!result.market_accepted);

    CHECK(!result.bid.accepted);
    CHECK(!result.ask.accepted);

    CHECK(
        strategy.rejectionReporter().count() ==
        1
    );

    CHECK(!strategy.activeBidOrderId().has_value());
    CHECK(!strategy.activeAskOrderId().has_value());

    checkBookInvariants();
}


void testKillSwitchCancelsQuotes() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerStrategy strategy(
        makeTestStrategyConfig(),
        makeTestRiskLimits()
    );

    strategy.onMarketUpdate(1000);

    QuoteRefreshResult initialQuotes =
        strategy.refreshQuotes(1000);

    CHECK(initialQuotes.bid.accepted);
    CHECK(initialQuotes.ask.accepted);

    OrderId bidId =
        initialQuotes.bid.order_id.value();

    OrderId askId =
        initialQuotes.ask.order_id.value();

    CHECK(orderIndex.contains(bidId));
    CHECK(orderIndex.contains(askId));

    strategy.activateKillSwitch(
        1050,
        "Manual emergency stop"
    );

    CHECK(
        strategy.riskManager()
            .killSwitchActive()
    );

    CHECK(!orderIndex.contains(bidId));
    CHECK(!orderIndex.contains(askId));

    CHECK(!strategy.activeBidOrderId().has_value());
    CHECK(!strategy.activeAskOrderId().has_value());

    strategy.onMarketUpdate(1100);

    QuoteRefreshResult blockedQuotes =
        strategy.refreshQuotes(1100);

    CHECK(!blockedQuotes.market_accepted);
    CHECK(!blockedQuotes.bid.accepted);
    CHECK(!blockedQuotes.ask.accepted);

    checkBookInvariants();
}

void testRejectedOrderReporting() {
    resetOrderBook();
    addBasicExternalMarket();

    MarketMakerConfig config =
        makeTestStrategyConfig();

    config.quote_quantity = 50;

    RiskLimits limits =
        makeTestRiskLimits();

    limits.maximum_order_size = 10;

    MarketMakerStrategy strategy(
        config,
        limits
    );

    strategy.onMarketUpdate(1000);
    strategy.refreshQuotes(1000);

    const auto& records =
        strategy.rejectionReporter().records();

    CHECK(records.size() == 2);

    CHECK(
        records[0].reason ==
        RejectionReason::
            MaximumOrderSizeExceeded
    );

    CHECK(records[0].order.has_value());

    CHECK(
        records[0].order->quantity ==
        50
    );

    CHECK(
        records[1].reason ==
        RejectionReason::
            MaximumOrderSizeExceeded
    );
}

void runStrategyRiskTests() {
    testMarketMakerGeneratesBidAndAsk();
    testInventoryChangesQuotePrices();

    testMaximumOrderSizeRejectsQuotes();
    testMaximumPositionRejectsQuotes();
    testMaximumNotionalRejectsQuotes();

    testStaleMarketPreventsQuoting();
    testKillSwitchCancelsQuotes();

    testRejectedOrderReporting();

    std::cout
        << "All strategy and risk tests passed!\n";
}