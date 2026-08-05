#pragma once

#include "market_event.hpp"

#include <filesystem>
#include <vector>

std::vector<MarketEvent>
loadLobsterMessageFile(
    const std::filesystem::path& filePath
);