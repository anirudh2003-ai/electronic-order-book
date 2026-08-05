#include "order_book.hpp"
#include "pnl_engine.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

void testLongPositionPnl() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;
    constexpr SymbolId Symbol = 1;

    pnl.registerAccount(Trader);
    pnl.setSymbolName(Symbol, "TEST");

    /*
        Trader buys 10 at 100.

        Participant 0 represents an external/unknown
        counterparty.
    */
    Trade buyTrade{
        1,
        2,
        100,
        10,
        Side::Buy,
        UnknownParticipant,
        Trader,
        Symbol
    };

    CHECK(pnl.processTrade(buyTrade));

    CHECK(pnl.position(Trader, Symbol) == 10);

    CHECK_NEAR(
        pnl.averageEntryPrice(
            Trader,
            Symbol
        ),
        100.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Trader),
        -1000.0L,
        0.0001L
    );

    CHECK(
        pnl.setMarkPrice(
            Symbol,
            110.0L
        )
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            Symbol
        ),
        100.0L,
        0.0001L
    );

    /*
        Trader then sells 4 at 120.

        Profit on closed quantity:

        (120 - 100) × 4 = 80
    */
    Trade sellTrade{
        3,
        4,
        120,
        4,
        Side::Sell,
        UnknownParticipant,
        Trader,
        Symbol
    };

    CHECK(pnl.processTrade(sellTrade));

    CHECK(pnl.position(Trader, Symbol) == 6);

    CHECK_NEAR(
        pnl.realisedPnl(Trader),
        80.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            Symbol
        ),
        60.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Trader),
        140.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Trader),
        -520.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.accountEquity(Trader),
        140.0L,
        0.0001L
    );
}

void testWeightedAverageEntryPrice() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    pnl.processTrade(
        Trade{
            1,
            10,
            100,
            10,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    pnl.processTrade(
        Trade{
            2,
            11,
            120,
            10,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    CHECK(
        pnl.position(Trader, 1) ==
        20
    );

    CHECK_NEAR(
        pnl.averageEntryPrice(
            Trader,
            1
        ),
        110.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Trader),
        -2200.0L,
        0.0001L
    );
}


void testShortPositionPnl() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    /*
        Trader sells short 8 at 200.
    */
    pnl.processTrade(
        Trade{
            1,
            10,
            200,
            8,
            Side::Sell,
            UnknownParticipant,
            Trader,
            1
        }
    );

    CHECK(
        pnl.position(Trader, 1) ==
        -8
    );

    CHECK_NEAR(
        pnl.averageEntryPrice(
            Trader,
            1
        ),
        200.0L,
        0.0001L
    );

    /*
        Trader buys back 3 at 180.

        Profit:

        (200 - 180) × 3 = 60
    */
    pnl.processTrade(
        Trade{
            2,
            11,
            180,
            3,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    CHECK(
        pnl.position(Trader, 1) ==
        -5
    );

    CHECK_NEAR(
        pnl.realisedPnl(Trader),
        60.0L,
        0.0001L
    );

    pnl.setMarkPrice(1, 190.0L);

    /*
        Remaining short profit:

        (200 - 190) × 5 = 50
    */
    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            1
        ),
        50.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Trader),
        110.0L,
        0.0001L
    );
}




void testPositionFlip() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    pnl.processTrade(
        Trade{
            1,
            10,
            100,
            5,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    /*
        Sell 8:

        5 closes the existing long.
        3 opens a new short at 120.
    */
    pnl.processTrade(
        Trade{
            2,
            11,
            120,
            8,
            Side::Sell,
            UnknownParticipant,
            Trader,
            1
        }
    );

    CHECK(
        pnl.position(Trader, 1) ==
        -3
    );

    CHECK_NEAR(
        pnl.averageEntryPrice(
            Trader,
            1
        ),
        120.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.realisedPnl(Trader),
        100.0L,
        0.0001L
    );
}



void testMakerAndTakerAccounts() {
    PnlEngine pnl;

    constexpr ParticipantId Seller = 5001;
    constexpr ParticipantId Buyer = 5002;

    Trade trade{
        1,
        2,
        100,
        5,
        Side::Buy,
        Seller,
        Buyer,
        1
    };

    pnl.processTrade(trade);

    /*
        Taker aggressor was Buy.

        Therefore:
        maker sold;
        taker bought.
    */
    CHECK(
        pnl.position(Seller, 1) ==
        -5
    );

    CHECK(
        pnl.position(Buyer, 1) ==
        5
    );

    CHECK_NEAR(
        pnl.cashBalance(Seller),
        500.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Buyer),
        -500.0L,
        0.0001L
    );

    pnl.setMarkPrice(1, 100.0L);

    CHECK_NEAR(
        pnl.totalPnl(Seller),
        0.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Buyer),
        0.0L,
        0.0001L
    );
}

void testPerSymbolPnl() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    pnl.setSymbolName(1, "ALPHA");
    pnl.setSymbolName(2, "BETA");

    pnl.processTrade(
        Trade{
            1,
            10,
            100,
            10,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    pnl.processTrade(
        Trade{
            2,
            11,
            200,
            4,
            Side::Sell,
            UnknownParticipant,
            Trader,
            2
        }
    );

    pnl.setMarkPrice(1, 110.0L);
    pnl.setMarkPrice(2, 180.0L);

    CHECK(
        pnl.position(Trader, 1) ==
        10
    );

    CHECK(
        pnl.position(Trader, 2) ==
        -4
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            1
        ),
        100.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Trader,
            2
        ),
        80.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Trader),
        180.0L,
        0.0001L
    );
}

void testPnlFromOrderBookTrades() {
    resetOrderBook();

    constexpr ParticipantId Buyer = 5001;
    constexpr ParticipantId Seller = 5002;

    /*
        Seller places a resting ask.
    */
    CHECK(
        addOrder(
            Order{
                1,
                Side::Sell,
                10000,
                10,
                Seller,
                1
            }
        )
    );

    /*
        Buyer submits an aggressive buy.
    */
    ExecutionResult execution =
        executeOrder(
            Order{
                2,
                Side::Buy,
                10000,
                4,
                Buyer,
                1
            }
        );

    CHECK(execution.accepted);
    CHECK(execution.executed_quantity == 4);
    CHECK(tradeHistory.size() == 1);

    CHECK(
        tradeHistory[0]
            .maker_participant_id ==
        Seller
    );

    CHECK(
        tradeHistory[0]
            .taker_participant_id ==
        Buyer
    );

    PnlEngine pnl;

    CHECK(
        pnl.processNewTrades(
            tradeHistory
        )
    );

    CHECK(
        pnl.position(Buyer, 1) ==
        4
    );

    CHECK(
        pnl.position(Seller, 1) ==
        -4
    );

    CHECK_NEAR(
        pnl.cashBalance(Buyer),
        -40000.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.cashBalance(Seller),
        40000.0L,
        0.0001L
    );

    pnl.setMarkPrice(1, 10100.0L);

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Buyer,
            1
        ),
        400.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.unrealisedPnl(
            Seller,
            1
        ),
        -400.0L,
        0.0001L
    );

    CHECK_NEAR(
        pnl.totalPnl(Buyer) +
        pnl.totalPnl(Seller),
        0.0L,
        0.0001L
    );
}

void testPnlSummaryReport() {
    PnlEngine pnl;

    constexpr ParticipantId Trader = 5001;

    pnl.registerAccount(
        Trader,
        1'000'000.0L
    );

    pnl.setSymbolName(
        1,
        "TEST"
    );

    pnl.processTrade(
        Trade{
            1,
            10,
            10000,
            10,
            Side::Buy,
            UnknownParticipant,
            Trader,
            1
        }
    );

    pnl.setMarkPrice(
        1,
        10100.0L
    );

    PnlReportConfig config;

    /*
        Prices and cash are stored in pence.
        Divide by 100 to display pounds.
    */
    config.monetary_scale = 100.0L;
    config.currency_prefix = "£";
    config.decimal_places = 2;

    std::string report =
        pnl.createSummaryReport(config);

    CHECK(
        report.find(
            "Participant 5001"
        ) != std::string::npos
    );

    CHECK(
        report.find(
            "Symbol: TEST"
        ) != std::string::npos
    );

    CHECK(
        report.find(
            "Position: 10"
        ) != std::string::npos
    );

    CHECK(
        report.find(
            "Total P&L"
        ) != std::string::npos
    );
}

void runPnlTests() {
    testLongPositionPnl();
    testWeightedAverageEntryPrice();
    testShortPositionPnl();
    testPositionFlip();

    testMakerAndTakerAccounts();
    testPerSymbolPnl();

    testPnlFromOrderBookTrades();
    testPnlSummaryReport();

    std::cout
        << "All P&L tests passed!\n";
}