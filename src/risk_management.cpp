#include "risk_management.hpp"


Side oppositeSide(Side side) {
    return side == Side::Buy
        ? Side::Sell
        : Side::Buy;
}


std::string sideToString(Side side) {
    return side == Side::Buy
        ? "BUY"
        : "SELL";
}


std::string rejectionReasonToString(
    RejectionReason reason
) {
    switch (reason) {
        case RejectionReason::None:
            return "none";

        case RejectionReason::
            KillSwitchActive:
            return "kill switch active";

        case RejectionReason::
            MarketUnavailable:
            return "market unavailable";

        case RejectionReason::StaleMarket:
            return "stale market";

        case RejectionReason::
            InvalidTimestamp:
            return "invalid timestamp";

        case RejectionReason::
            CrossedMarket:
            return "crossed or locked market";

        case RejectionReason::InvalidPrice:
            return "invalid price";

        case RejectionReason::
            InvalidQuantity:
            return "invalid quantity";

        case RejectionReason::
            MaximumOrderSizeExceeded:
            return "maximum order size exceeded";

        case RejectionReason::
            MaximumPositionExceeded:
            return "maximum position exceeded";

        case RejectionReason::
            MaximumNotionalExceeded:
            return
                "maximum notional exposure "
                "exceeded";

        case RejectionReason::
            QuoteWouldCross:
            return
                "generated quotes would cross";

        case RejectionReason::
            DuplicateOrderId:
            return "duplicate order ID";

        case RejectionReason::
            OrderBookRejected:
            return
                "order book rejected order";
    }

    return "unknown";
}


RiskCheckResult acceptRiskCheck() {
    return RiskCheckResult{
        true,
        RejectionReason::None,
        ""
    };
}


RiskCheckResult rejectRiskCheck(
    RejectionReason reason,
    const std::string& message
) {
    return RiskCheckResult{
        false,
        reason,
        message
    };
}