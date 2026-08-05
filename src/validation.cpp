#include "validation.hpp"

#include "order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

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

void checkBookInvariants() {
    std::size_t numberOfOrdersInBook = 0;

    auto checkSide = [&](auto& book, Side expectedSide) {
        for (auto& [price, level] : book) {
            CHECK(!level.orders.empty());

            std::uint64_t calculatedTotal = 0;

            for (
                auto orderIterator = level.orders.begin();
                orderIterator != level.orders.end();
                ++orderIterator
            ) {
                ++numberOfOrdersInBook;
                calculatedTotal += orderIterator->quantity;

                CHECK(orderIterator->side == expectedSide);
                CHECK(orderIterator->price == price);
                CHECK(orderIterator->quantity > 0);

                auto indexIterator =
                    orderIndex.find(orderIterator->id);

                CHECK(indexIterator != orderIndex.end());
                CHECK(indexIterator->second.side == expectedSide);
                CHECK(indexIterator->second.price == price);

                // orderIndex must point to this exact list element.
                CHECK(
                    indexIterator->second.iterator ==
                    orderIterator
                );
            }

            CHECK(calculatedTotal == level.total_quantity);
        }
    };

    checkSide(bids, Side::Buy);
    checkSide(asks, Side::Sell);

    CHECK(numberOfOrdersInBook == orderIndex.size());

    for (const auto& [id, location] : orderIndex) {
        CHECK(location.iterator->id == id);
        CHECK(location.iterator->side == location.side);
        CHECK(location.iterator->price == location.price);
        CHECK(location.iterator->quantity > 0);

        if (location.side == Side::Buy) {
            CHECK(bids.contains(location.price));
        } else {
            CHECK(asks.contains(location.price));
        }
    }
}