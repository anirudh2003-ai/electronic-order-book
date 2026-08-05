#pragma once

#include <array>
#include <atomic>
#include <cstddef>


template<
    typename T,
    std::size_t Capacity
>
class SpscRingBuffer {
public:
    static_assert(
        Capacity >= 2
    );

    static_assert(
        (Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of two"
    );

    bool tryPush(const T& value) {
        std::size_t write =
            write_index_.load(
                std::memory_order_relaxed
            );

        std::size_t next =
            (write + 1) &
            (Capacity - 1);

        if (
            next ==
            read_index_.load(
                std::memory_order_acquire
            )
        ) {
            return false;
        }

        values_[write] = value;

        write_index_.store(
            next,
            std::memory_order_release
        );

        return true;
    }

    bool tryPop(T& value) {
        std::size_t read =
            read_index_.load(
                std::memory_order_relaxed
            );

        if (
            read ==
            write_index_.load(
                std::memory_order_acquire
            )
        ) {
            return false;
        }

        value = values_[read];

        read_index_.store(
            (read + 1) &
                (Capacity - 1),
            std::memory_order_release
        );

        return true;
    }

    std::size_t approximateSize() const {
        std::size_t write =
            write_index_.load(
                std::memory_order_acquire
            );

        std::size_t read =
            read_index_.load(
                std::memory_order_acquire
            );

        if (write >= read) {
            return write - read;
        }

        return Capacity - read + write;
    }

private:
    std::array<T, Capacity> values_{};

    alignas(64)
    std::atomic<std::size_t>
        write_index_{0};

    alignas(64)
    std::atomic<std::size_t>
        read_index_{0};
};