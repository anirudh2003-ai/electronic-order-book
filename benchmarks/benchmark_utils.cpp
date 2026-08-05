#include "benchmark_utils.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <psapi.h>

#include <cmath>
#include <cstddef>

double currentWorkingSetMb() {
    PROCESS_MEMORY_COUNTERS counters{};

    if (
        GetProcessMemoryInfo(
            GetCurrentProcess(),
            &counters,
            static_cast<DWORD>(
                sizeof(counters)
            )
        ) == 0
    ) {
        return -1.0;
    }

    constexpr double BytesPerMegabyte =
        1024.0 * 1024.0;

    return static_cast<double>(
        counters.WorkingSetSize
    ) / BytesPerMegabyte;
}


std::uint64_t percentileFromSorted(
    const std::vector<std::uint64_t>& sortedSamples,
    long double percentile
) {
    if (sortedSamples.empty()) {
        return 0;
    }

    if (sortedSamples.size() == 1) {
        return sortedSamples.front();
    }

    const long double position =
        percentile *
        static_cast<long double>(
            sortedSamples.size() - 1
        );

    const std::size_t lowerIndex =
        static_cast<std::size_t>(
            std::floor(position)
        );

    const std::size_t upperIndex =
        static_cast<std::size_t>(
            std::ceil(position)
        );

    if (lowerIndex == upperIndex) {
        return sortedSamples[lowerIndex];
    }

    const long double fraction =
        position -
        static_cast<long double>(
            lowerIndex
        );

    const long double interpolated =
        static_cast<long double>(
            sortedSamples[lowerIndex]
        ) *
            (1.0L - fraction) +
        static_cast<long double>(
            sortedSamples[upperIndex]
        ) *
            fraction;

    return static_cast<std::uint64_t>(
        std::llround(interpolated)
    );
}