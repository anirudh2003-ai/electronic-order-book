#include "binary_codec.hpp"
#include "order_book.hpp"
#include "pnl_engine.hpp"
#include "recovery.hpp"
#include "scenario_defaults.hpp"
#include "test_support.hpp"
#include "udp_feed.hpp"
#include "validation.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

void testRecoveryRestoresMissingEvents() {
    std::vector<MarketEvent> completeFeed{
        makeAddEvent(
            1,
            1000,
            Order{
                1,
                Side::Buy,
                9900,
                10,
                6001,
                DefaultSymbol
            }
        ),

        makeAddEvent(
            2,
            1100,
            Order{
                2,
                Side::Sell,
                10100,
                10,
                6002,
                DefaultSymbol
            }
        ),

        makeModifyEvent(
            3,
            1200,
            1,
            9950,
            12,
            6001,
            DefaultSymbol
        )
    };

    // Produce the expected clean final state.
    resetOrderBook();

    for (const MarketEvent& event : completeFeed) {
        CHECK(
            applyMarketEvent(event).accepted
        );
    }

    std::string expectedSnapshot =
        createOrderBookSnapshot();

    // Simulate UDP loss: event 2 is missing.
    resetOrderBook();

    InMemoryRecoverySource recoverySource{
        completeFeed
    };

    RecoverySequencer sequencer{
        1,
        64,
        recoverySource
    };

    CHECK(sequencer.onEvent(completeFeed[0]));

    // Event 3 arrives while event 2 is missing.
    CHECK(sequencer.onEvent(completeFeed[2]));

    CHECK(
        sequencer.state() ==
        FeedState::Recovering
    );

    CHECK(sequencer.retryRecovery());

    CHECK(sequencer.healthy());
    CHECK(sequencer.expectedSequence() == 4);

    CHECK(
        sequencer.statistics()
            .events_recovered ==
        1
    );

    CHECK(
        createOrderBookSnapshot() ==
        expectedSnapshot
    );

    checkBookInvariants();
}

void testOutOfOrderArrivalWithoutRecovery() {
    std::vector<MarketEvent> events{
        makeAddEvent(
            1,
            1000,
            Order{1, Side::Buy, 9900, 10}
        ),

        makeAddEvent(
            2,
            1100,
            Order{2, Side::Sell, 10100, 10}
        ),

        makeModifyEvent(
            3,
            1200,
            1,
            9950,
            12
        )
    };

    resetOrderBook();

    InMemoryRecoverySource emptySource{
            {}
    };

    RecoverySequencer sequencer{
        1,
        64,
        emptySource
    };

    CHECK(sequencer.onEvent(events[0]));

    // Sequence 3 is buffered.
    CHECK(sequencer.onEvent(events[2]));

    CHECK(
        sequencer.state() ==
        FeedState::Recovering
    );

    CHECK(sequencer.bufferedEventCount() == 1);

    // Sequence 2 later arrives over UDP.
    CHECK(sequencer.onEvent(events[1]));

    CHECK(sequencer.healthy());
    CHECK(sequencer.expectedSequence() == 4);

    CHECK(bids.contains(9950));
    CHECK(asks.contains(10100));

    checkBookInvariants();
}
void testUdpProcessingLoopRecoversGap() {
    resetOrderBook();

    std::vector<MarketEvent> completeFeed{
        makeAddEvent(
            1,
            1000,
            Order{
                1,
                Side::Buy,
                9900,
                100,
                6001,
                DefaultSymbol
            }
        ),

        makeAddEvent(
            2,
            1100,
            Order{
                2,
                Side::Sell,
                10100,
                100,
                6002,
                DefaultSymbol
            }
        ),

        makeModifyEvent(
            3,
            1200,
            1,
            9950,
            100,
            6001,
            DefaultSymbol
        )
    };

    NetworkQueue queue;

    InMemoryRecoverySource recoverySource{
        completeFeed
    };

    RecoverySequencer sequencer{
        1,
        64,
        recoverySource
    };

    MarketMakerStrategy strategy{
        makeTestStrategyConfig(),
        makeTestRiskLimits()
    };

    PnlEngine pnl;

    pnl.registerAccount(
        5001,
        1'000'000.0L
    );

    pnl.setSymbolName(
        DefaultSymbol,
        "SIMULATED_MARKET"
    );

    UdpFeedProcessor processor{
        queue,
        sequencer,
        strategy,
        pnl
    };

    /*
        Sequence 1 arrives normally.
    */
    CHECK(
        queue.tryPush(
            makeUdpDatagram(
                completeFeed[0]
            )
        )
    );

    CHECK(processor.processAvailable() == 1);

    /*
        Sequence 2 is missing.

        Sequence 3 arrives, forcing the processor to:
        pause the strategy,
        recover sequence 2,
        apply sequence 2,
        apply sequence 3,
        resume the strategy.
    */
    CHECK(
        queue.tryPush(
            makeUdpDatagram(
                completeFeed[2]
            )
        )
    );

    CHECK(processor.processAvailable() == 1);

    CHECK(sequencer.healthy());
    CHECK(sequencer.expectedSequence() == 4);

    CHECK(
        sequencer.statistics()
            .events_recovered ==
        1
    );

    CHECK(
        processor.statistics()
            .recovery_pauses ==
        1
    );

    CHECK(
        processor.statistics()
            .recovery_resumes ==
        1
    );

    CHECK(bids.contains(9950));
    CHECK(asks.contains(10100));

    CHECK(
        !strategy.riskManager()
            .killSwitchActive()
    );

    checkBookInvariants();
}

void runUdpRecoveryTests() {
    testRecoveryRestoresMissingEvents();
    testOutOfOrderArrivalWithoutRecovery();
    testUdpProcessingLoopRecoversGap();

    std::cout
        << "All UDP recovery tests passed!\n";
}