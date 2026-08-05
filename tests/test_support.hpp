#pragma once

#include "order_book.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#define CHECK(condition)                              \
    do {                                              \
        if (!(condition)) {                           \
            std::cerr                                 \
                << "CHECK failed: " #condition        \
                << " at line "                        \
                << __LINE__                           \
                << '\n';                              \
            std::exit(EXIT_FAILURE);                  \
        }                                             \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)        \
    do {                                              \
        const long double actualValue =               \
            static_cast<long double>(actual);         \
                                                        \
        const long double expectedValue =             \
            static_cast<long double>(expected);       \
                                                        \
        const long double toleranceValue =            \
            static_cast<long double>(tolerance);      \
                                                        \
        if (                                          \
            std::fabs(                                \
                actualValue - expectedValue           \
            ) > toleranceValue                        \
        ) {                                           \
            std::cerr                                 \
                << "CHECK_NEAR failed at line "        \
                << __LINE__                           \
                << ": actual="                        \
                << static_cast<double>(actualValue)   \
                << ", expected="                      \
                << static_cast<double>(expectedValue) \
                << ", tolerance="                     \
                << static_cast<double>(toleranceValue)\
                << '\n';                              \
                                                        \
            std::exit(EXIT_FAILURE);                  \
        }                                             \
    } while (false)

std::vector<OrderId> getQueueIds(
    const OrderQueue& queue
);

void checkTrade(
    const Trade& trade,
    OrderId expectedMaker,
    OrderId expectedTaker,
    Price expectedPrice,
    Quantity expectedQuantity,
    Side expectedAggressor
);

void checkExecutionArithmetic(
    const ExecutionResult& result,
    Quantity originalQuantity
);