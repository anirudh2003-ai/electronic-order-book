#include "order_book.hpp"
#include "test_support.hpp"
#include "validation.hpp"

#include <iostream>
#include <vector>

void testExecuteRejectsZeroQuantity() {
    resetOrderBook();

    ExecutionResult result = executeOrder(
        Order{1, Side::Buy, 10000, 0}
    );

    CHECK(!result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.empty());

    CHECK(bids.empty());
    CHECK(asks.empty());
    CHECK(orderIndex.empty());
    CHECK(tradeHistory.empty());

    checkBookInvariants();
}

void testExecuteRejectsInvalidPrice() {
    resetOrderBook();

    ExecutionResult zeroPrice = executeOrder(
        Order{1, Side::Buy, 0, 10}
    );

    CHECK(!zeroPrice.accepted);

    ExecutionResult negativePrice = executeOrder(
        Order{2, Side::Sell, -100, 10}
    );

    CHECK(!negativePrice.accepted);

    CHECK(bids.empty());
    CHECK(asks.empty());
    CHECK(orderIndex.empty());
    CHECK(tradeHistory.empty());

    checkBookInvariants();
}

void testExecuteRejectsDuplicateId() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10000, 10}));

    ExecutionResult result = executeOrder(
        Order{1, Side::Sell, 9000, 5}
    );

    CHECK(!result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 5);
    CHECK(result.trades.empty());

    // Original order must remain unchanged.
    CHECK(orderIndex.size() == 1);
    CHECK(orderIndex.contains(1));
    CHECK(bids.at(10000).total_quantity == 10);
    CHECK(bids.at(10000).orders.front().quantity == 10);

    CHECK(tradeHistory.empty());

    checkBookInvariants();
}




void testBuyWithEmptyAskBookRests() {
    resetOrderBook();

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 8);
    CHECK(result.trades.empty());

    CHECK(bids.contains(10000));
    CHECK(bids.at(10000).total_quantity == 8);
    CHECK(bids.at(10000).orders.front().id == 10);
    CHECK(orderIndex.contains(10));

    CHECK(tradeHistory.empty());

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}

void testSellWithEmptyBidBookRests() {
    resetOrderBook();

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10100, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 8);
    CHECK(result.trades.empty());

    CHECK(asks.contains(10100));
    CHECK(asks.at(10100).total_quantity == 8);
    CHECK(asks.at(10100).orders.front().id == 10);
    CHECK(orderIndex.contains(10));

    CHECK(tradeHistory.empty());

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}

void testNonCrossingBuyRests() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10100, 5}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 8);
    CHECK(result.trades.empty());

    // Existing ask remains unchanged.
    CHECK(asks.at(10100).total_quantity == 5);

    // Incoming buy rests.
    CHECK(bids.at(10000).total_quantity == 8);
    CHECK(orderIndex.contains(10));

    CHECK(bestBid().value() == 10000);
    CHECK(bestAsk().value() == 10100);

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}

void testNonCrossingSellRests() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10000, 5}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10100, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 0);
    CHECK(result.remaining_quantity == 8);
    CHECK(result.trades.empty());

    CHECK(bids.at(10000).total_quantity == 5);
    CHECK(asks.at(10100).total_quantity == 8);

    CHECK(bestBid().value() == 10000);
    CHECK(bestAsk().value() == 10100);

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}



void testExactFullFillIncomingBuy() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 5}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 5}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 5);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 1);

    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        5,
        Side::Buy
    );

    CHECK(asks.empty());
    CHECK(bids.empty());

    // Maker was removed.
    CHECK(!orderIndex.contains(1));

    // Fully executed taker was never added.
    CHECK(!orderIndex.contains(10));

    CHECK(tradeHistory.size() == 1);

    checkExecutionArithmetic(result, 5);
    checkBookInvariants();
}

void testExactFullFillIncomingSell() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10100, 7}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10100, 7}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 7);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 1);

    checkTrade(
        result.trades[0],
        1,
        10,
        10100,
        7,
        Side::Sell
    );

    CHECK(bids.empty());
    CHECK(asks.empty());
    CHECK(orderIndex.empty());

    CHECK(tradeHistory.size() == 1);

    checkExecutionArithmetic(result, 7);
    checkBookInvariants();
}





void testIncomingBuyPartiallyFillsRestingSell() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 10}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10100, 4}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 4);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 1);

    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        4,
        Side::Buy
    );

    // Resting sell remains with quantity 6.
    CHECK(asks.contains(10000));
    CHECK(asks.at(10000).total_quantity == 6);
    CHECK(asks.at(10000).orders.size() == 1);
    CHECK(asks.at(10000).orders.front().id == 1);
    CHECK(asks.at(10000).orders.front().quantity == 6);

    CHECK(orderIndex.contains(1));
    CHECK(orderIndex.at(1).iterator->quantity == 6);

    CHECK(!orderIndex.contains(10));

    checkExecutionArithmetic(result, 4);
    checkBookInvariants();
}

void testIncomingSellPartiallyFillsRestingBuy() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10100, 10}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10000, 4}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 4);
    CHECK(result.remaining_quantity == 0);

    CHECK(bids.at(10100).total_quantity == 6);
    CHECK(bids.at(10100).orders.front().quantity == 6);
    CHECK(orderIndex.at(1).iterator->quantity == 6);

    checkTrade(
        result.trades[0],
        1,
        10,
        10100,
        4,
        Side::Sell
    );

    checkExecutionArithmetic(result, 4);
    checkBookInvariants();
}

void testIncomingBuyLeavesRestingRemainder() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10100, 10}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 4);
    CHECK(result.remaining_quantity == 6);

    // Sell was completely removed.
    CHECK(asks.empty());
    CHECK(!orderIndex.contains(1));

    // Remaining incoming buy rests at its own limit price.
    CHECK(bids.contains(10100));
    CHECK(bids.at(10100).total_quantity == 6);
    CHECK(bids.at(10100).orders.front().id == 10);
    CHECK(bids.at(10100).orders.front().quantity == 6);
    CHECK(orderIndex.contains(10));

    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        4,
        Side::Buy
    );

    checkExecutionArithmetic(result, 10);
    checkBookInvariants();
}

void testIncomingSellLeavesRestingRemainder() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10100, 3}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10000, 8}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 3);
    CHECK(result.remaining_quantity == 5);

    CHECK(bids.empty());

    CHECK(asks.contains(10000));
    CHECK(asks.at(10000).total_quantity == 5);
    CHECK(asks.at(10000).orders.front().id == 10);
    CHECK(asks.at(10000).orders.front().quantity == 5);

    checkTrade(
        result.trades[0],
        1,
        10,
        10100,
        3,
        Side::Sell
    );

    checkExecutionArithmetic(result, 8);
    checkBookInvariants();
}




void testBuyUsesFifoAtSameAskPrice() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 3}));
    CHECK(addOrder(Order{2, Side::Sell, 10000, 4}));
    CHECK(addOrder(Order{3, Side::Sell, 10000, 5}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 5}
    );

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 5);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 2);

    // Oldest order executes first.
    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        3,
        Side::Buy
    );

    // Then the second-oldest order.
    checkTrade(
        result.trades[1],
        2,
        10,
        10000,
        2,
        Side::Buy
    );

    std::vector<OrderId> expectedQueue{2, 3};

    CHECK(
        getQueueIds(asks.at(10000).orders) ==
        expectedQueue
    );

    CHECK(asks.at(10000).orders.front().quantity == 2);
    CHECK(asks.at(10000).total_quantity == 7);

    CHECK(!orderIndex.contains(1));
    CHECK(orderIndex.contains(2));
    CHECK(orderIndex.contains(3));

    checkExecutionArithmetic(result, 5);
    checkBookInvariants();
}

void testSellUsesFifoAtSameBidPrice() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10000, 2}));
    CHECK(addOrder(Order{2, Side::Buy, 10000, 4}));
    CHECK(addOrder(Order{3, Side::Buy, 10000, 6}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10000, 5}
    );

    CHECK(result.trades.size() == 2);

    checkTrade(
        result.trades[0],
        1,
        10,
        10000,
        2,
        Side::Sell
    );

    checkTrade(
        result.trades[1],
        2,
        10,
        10000,
        3,
        Side::Sell
    );

    std::vector<OrderId> expectedQueue{2, 3};

    CHECK(
        getQueueIds(bids.at(10000).orders) ==
        expectedQueue
    );

    CHECK(bids.at(10000).orders.front().quantity == 1);
    CHECK(bids.at(10000).total_quantity == 7);

    checkExecutionArithmetic(result, 5);
    checkBookInvariants();
}



void testBuyConsumesMultipleAskLevels() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 9900, 2}));
    CHECK(addOrder(Order{2, Side::Sell, 10000, 3}));
    CHECK(addOrder(Order{3, Side::Sell, 10100, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10000, 6}
    );

    /*
        Matches:
        2 at 9900
        3 at 10000

        Stops before 10100 because buy limit is 10000.

        Remaining quantity 1 rests as a bid at 10000.
    */

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 5);
    CHECK(result.remaining_quantity == 1);
    CHECK(result.trades.size() == 2);

    checkTrade(
        result.trades[0],
        1,
        10,
        9900,
        2,
        Side::Buy
    );

    checkTrade(
        result.trades[1],
        2,
        10,
        10000,
        3,
        Side::Buy
    );

    CHECK(!asks.contains(9900));
    CHECK(!asks.contains(10000));

    CHECK(asks.contains(10100));
    CHECK(asks.at(10100).total_quantity == 4);

    CHECK(bids.contains(10000));
    CHECK(bids.at(10000).total_quantity == 1);
    CHECK(bids.at(10000).orders.front().id == 10);

    CHECK(bestBid().value() == 10000);
    CHECK(bestAsk().value() == 10100);

    checkExecutionArithmetic(result, 6);
    checkBookInvariants();
}

void testSellConsumesMultipleBidLevels() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10300, 2}));
    CHECK(addOrder(Order{2, Side::Buy, 10200, 3}));
    CHECK(addOrder(Order{3, Side::Buy, 10100, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10200, 6}
    );

    /*
        Matches:
        2 at 10300
        3 at 10200

        Stops before 10100 because sell limit is 10200.

        Remaining quantity 1 rests as an ask at 10200.
    */

    CHECK(result.accepted);
    CHECK(result.executed_quantity == 5);
    CHECK(result.remaining_quantity == 1);
    CHECK(result.trades.size() == 2);

    checkTrade(
        result.trades[0],
        1,
        10,
        10300,
        2,
        Side::Sell
    );

    checkTrade(
        result.trades[1],
        2,
        10,
        10200,
        3,
        Side::Sell
    );

    CHECK(!bids.contains(10300));
    CHECK(!bids.contains(10200));

    CHECK(bids.contains(10100));
    CHECK(bids.at(10100).total_quantity == 4);

    CHECK(asks.contains(10200));
    CHECK(asks.at(10200).total_quantity == 1);

    CHECK(bestBid().value() == 10100);
    CHECK(bestAsk().value() == 10200);

    checkExecutionArithmetic(result, 6);
    checkBookInvariants();
}




void testBuyCompletelySweepsSeveralAskLevels() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 2}));
    CHECK(addOrder(Order{2, Side::Sell, 10100, 3}));
    CHECK(addOrder(Order{3, Side::Sell, 10200, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Buy, 10200, 9}
    );

    CHECK(result.executed_quantity == 9);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 3);

    CHECK(asks.empty());
    CHECK(bids.empty());
    CHECK(orderIndex.empty());

    CHECK(tradeHistory.size() == 3);

    checkExecutionArithmetic(result, 9);
    checkBookInvariants();
}

void testSellCompletelySweepsSeveralBidLevels() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Buy, 10200, 2}));
    CHECK(addOrder(Order{2, Side::Buy, 10100, 3}));
    CHECK(addOrder(Order{3, Side::Buy, 10000, 4}));

    ExecutionResult result = executeOrder(
        Order{10, Side::Sell, 10000, 9}
    );

    CHECK(result.executed_quantity == 9);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.trades.size() == 3);

    CHECK(bids.empty());
    CHECK(asks.empty());
    CHECK(orderIndex.empty());

    CHECK(tradeHistory.size() == 3);

    checkExecutionArithmetic(result, 9);
    checkBookInvariants();
}




void testExecutionTradesAreSeparateFromGlobalHistory() {
    resetOrderBook();

    CHECK(addOrder(Order{1, Side::Sell, 10000, 2}));

    ExecutionResult first = executeOrder(
        Order{10, Side::Buy, 10000, 1}
    );

    CHECK(first.trades.size() == 1);
    CHECK(tradeHistory.size() == 1);

    ExecutionResult second = executeOrder(
        Order{11, Side::Buy, 10000, 1}
    );

    // This call created only one trade.
    CHECK(second.trades.size() == 1);

    // Global history contains both calls.
    CHECK(tradeHistory.size() == 2);

    checkTrade(
        first.trades[0],
        1,
        10,
        10000,
        1,
        Side::Buy
    );

    checkTrade(
        second.trades[0],
        1,
        11,
        10000,
        1,
        Side::Buy
    );

    CHECK(asks.empty());
    CHECK(orderIndex.empty());

    checkBookInvariants();
}



void runExecuteOrderTests() {
    testExecuteRejectsZeroQuantity();
    testExecuteRejectsInvalidPrice();
    testExecuteRejectsDuplicateId();

    testBuyWithEmptyAskBookRests();
    testSellWithEmptyBidBookRests();

    testNonCrossingBuyRests();
    testNonCrossingSellRests();

    testExactFullFillIncomingBuy();
    testExactFullFillIncomingSell();

    testIncomingBuyPartiallyFillsRestingSell();
    testIncomingSellPartiallyFillsRestingBuy();

    testIncomingBuyLeavesRestingRemainder();
    testIncomingSellLeavesRestingRemainder();

    testBuyUsesFifoAtSameAskPrice();
    testSellUsesFifoAtSameBidPrice();

    testBuyConsumesMultipleAskLevels();
    testSellConsumesMultipleBidLevels();

    testBuyCompletelySweepsSeveralAskLevels();
    testSellCompletelySweepsSeveralBidLevels();

    testExecutionTradesAreSeparateFromGlobalHistory();

    std::cout
        << "All executeOrder edge-case tests passed!\n";
}