#include "scenario_defaults.hpp"

MarketMakerConfig makeTestStrategyConfig() {
    MarketMakerConfig config;

    config.quote_quantity = 10;
    config.half_spread_ticks = 20;
    config.inventory_skew_per_unit = 2;
    config.target_position = 0;
    config.tick_size = 1;
    config.first_strategy_order_id =
        1'000'000;

    return config;
}

RiskLimits makeTestRiskLimits() {
    RiskLimits limits;

    limits.maximum_order_size = 20;
    limits.maximum_absolute_position = 100;
    limits.maximum_notional_exposure =
        10'000'000.0L;

    limits.maximum_market_age_ns = 1000;

    return limits;
}