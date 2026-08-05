#pragma once

#include <cstdint>
#include <vector>

double currentWorkingSetMb();

std::uint64_t percentileFromSorted(
    const std::vector<std::uint64_t>& sortedSamples,
    long double percentile
);