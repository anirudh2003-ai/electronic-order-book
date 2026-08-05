#include "binary_codec.hpp"
#include "market_maker_strategy.hpp"
#include "order_book.hpp"
#include "pnl_engine.hpp"
#include "recovery.hpp"
#include "scenario_defaults.hpp"
#include "test_support.hpp"

#include "analytics/telemetry_recorder.hpp"
#include "analytics/simulation_telemetry_bridge.hpp"

#include <filesystem>
#include <iostream>

void runStrategyPnlDemo() {
    // Start a completely new simulated trading session.
    resetOrderBook();

    analytics::TelemetryRecorder telemetry{
        "results/telemetry"
    };

    /*
        Enables the ScopedLatency timers only for this
        simulation, not for the unit tests.
    */


    analytics::SimulationTelemetryBridge
        analyticsBridge{
            telemetry
        };

    // --------------------------------------------------------
    // 1. Configure the market-making strategy
    // --------------------------------------------------------

    MarketMakerConfig strategyConfig;

    strategyConfig.quote_quantity = 10;
    strategyConfig.half_spread_ticks = 20;
    strategyConfig.inventory_skew_per_unit = 2;
    strategyConfig.target_position = 0;
    strategyConfig.tick_size = 1;

    strategyConfig.first_strategy_order_id =
        1'000'000;

    strategyConfig.participant_id = 5001;
    strategyConfig.symbol_id = DefaultSymbol;

    RiskLimits riskLimits;

    riskLimits.maximum_order_size = 20;
    riskLimits.maximum_absolute_position = 100;

    riskLimits.maximum_notional_exposure =
        10'000'000.0L;

    riskLimits.maximum_market_age_ns =
        1'000'000'000ULL;

    MarketMakerStrategy strategy{
        strategyConfig,
        riskLimits
    };

    // --------------------------------------------------------
    // 2. Create one persistent P&L engine
    // --------------------------------------------------------

    PnlEngine pnl;

    pnl.registerAccount(
        5001,
        1'000'000.0L
    );

    pnl.setSymbolName(
        DefaultSymbol,
        "SIMULATED_MARKET"
    );

    // --------------------------------------------------------
    // 3. Add an external market
    // --------------------------------------------------------

    constexpr ParticipantId ExternalBuyer = 6001;
    constexpr ParticipantId ExternalSeller = 6002;

    CHECK(
        addOrder(
            Order{
                1,
                Side::Buy,
                9900,
                100,
                ExternalBuyer,
                DefaultSymbol
            }
        )
    );

    CHECK(
        addOrder(
            Order{
                2,
                Side::Sell,
                10100,
                100,
                ExternalSeller,
                DefaultSymbol
            }
        )
    );

    // Inform the strategy that market data arrived.
    TimestampNs currentTimestamp = 1000;

    strategy.onMarketUpdate(
        currentTimestamp
    );

    // --------------------------------------------------------
    // 4. Generate the strategy's bid and ask
    // --------------------------------------------------------

    QuoteRefreshResult quotes =
        strategy.refreshQuotes(
            currentTimestamp
        );

    analyticsBridge.recordQuoteRefresh(
    currentTimestamp,
    quotes
);
    analyticsBridge.capture(
    currentTimestamp,
    strategyConfig.participant_id,
    strategyConfig.symbol_id,
    pnl,
    strategy,
    nullptr,
    nullptr,
    20
);

    std::cout
        << "\nStrategy bid: "
        << quotes.bid.price
        << '\n';

    std::cout
        << "Strategy ask: "
        << quotes.ask.price
        << '\n';

    // --------------------------------------------------------
    // 5. Simulate an external seller hitting our bid
    // --------------------------------------------------------
    currentTimestamp = 1100;
    if (
        quotes.bid.accepted &&
        quotes.bid.order_id.has_value()
    ) {
        ExecutionResult execution =
            executeOrder(
                Order{
                    100,
                    Side::Sell,
                    quotes.bid.price,
                    10,
                    ExternalSeller,
                    DefaultSymbol
                }
            );

        CHECK(execution.accepted);
    }

    // --------------------------------------------------------
    // 6. Send newly generated trades to inventory and P&L
    // --------------------------------------------------------

    strategy.processTrades();

    CHECK(
        pnl.processNewTrades(
            tradeHistory
        )
    );

    // --------------------------------------------------------
    // 7. Mark the position using the current order book
    // --------------------------------------------------------

    CHECK(
        pnl.markFromCurrentOrderBook(
            DefaultSymbol
        )
    );

    analyticsBridge.capture(
    currentTimestamp,
    strategyConfig.participant_id,
    strategyConfig.symbol_id,
    pnl,
    strategy,
    nullptr,
    nullptr,
    20
);

    // --------------------------------------------------------
    // 8. Produce the final report
    // --------------------------------------------------------

    PnlReportConfig reportConfig;

    /*
        Prices are stored in pence, so divide monetary
        values by 100 when displaying pounds.
    */
    reportConfig.monetary_scale = 100.0L;
    reportConfig.currency_prefix = "GBP ";
    reportConfig.decimal_places = 2;

    std::cout
    << '\n'
    << pnl.createSummaryReport(
        reportConfig
    );

    telemetry.flush();



    std::cout
        << "\nTelemetry written to: "
        << std::filesystem::absolute(
               "results/telemetry"
           )
        << '\n';
}