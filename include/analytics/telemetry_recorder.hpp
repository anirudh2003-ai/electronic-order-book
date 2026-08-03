#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <utility>

namespace analytics {

inline std::uint64_t wallClockTimestampNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

struct StateSample {
    std::uint64_t timestamp_ns{0};
    std::uint64_t wall_time_ns{0};
    std::uint64_t event_index{0};

    std::uint64_t participant_id{0};
    std::uint32_t symbol_id{0};

    std::int64_t best_bid{0};
    std::int64_t best_ask{0};
    long double mid_price{0.0L};

    std::int64_t strategy_bid{0};
    std::int64_t strategy_ask{0};

    long double realised_pnl{0.0L};
    long double unrealised_pnl{0.0L};
    long double total_pnl{0.0L};

    long double cash_balance{0.0L};
    long double account_equity{0.0L};

    std::int64_t inventory{0};
    std::int64_t maximum_absolute_position{0};

    std::int64_t market_spread{0};
    std::int64_t strategy_spread{0};

    std::uint64_t queue_depth{0};
    std::uint64_t events_processed{0};

    std::uint64_t gaps_detected{0};
    std::uint64_t events_recovered{0};
    std::uint64_t recovery_misses{0};

    std::string feed_state{"Healthy"};
};

struct DepthSample {
    std::uint64_t timestamp_ns{0};
    std::uint64_t event_index{0};
    std::string side;
    std::int64_t price{0};
    std::uint64_t quantity{0};
    std::uint64_t level{0};
};

struct QuoteSample {
    std::uint64_t timestamp_ns{0};
    std::uint64_t event_index{0};
    std::uint64_t order_id{0};
    std::string side;
    std::int64_t price{0};
    std::uint64_t quantity{0};
    bool accepted{false};
};

struct FillSample {
    std::uint64_t timestamp_ns{0};
    std::uint64_t event_index{0};
    std::uint64_t order_id{0};
    std::string side;
    std::int64_t price{0};
    std::uint64_t quantity{0};
    bool was_maker{false};
    long double mid_at_fill{0.0L};
};

struct LatencySample {
    std::uint64_t timestamp_ns{0};
    std::string operation;
    std::uint64_t latency_ns{0};
};

struct RecoverySample {
    std::uint64_t timestamp_ns{0};
    std::uint64_t wall_time_ns{0};
    std::uint64_t event_index{0};

    std::string event;
    std::string feed_state;

    std::uint64_t expected_sequence{0};
    std::uint64_t received_sequence{0};
    std::uint64_t gap_size{0};
    std::uint64_t buffer_depth{0};
    std::uint64_t duration_ns{0};
};

class TelemetryRecorder {
public:
    explicit TelemetryRecorder(
        const std::filesystem::path& outputDirectory
    ) {
        std::filesystem::create_directories(outputDirectory);

        openFile(
            state_file_,
            outputDirectory / "state.csv",
            "timestamp_ns,wall_time_ns,event_index,participant_id,symbol_id,"
            "best_bid,best_ask,mid_price,strategy_bid,strategy_ask,"
            "realised_pnl,unrealised_pnl,total_pnl,cash_balance,account_equity,"
            "inventory,maximum_absolute_position,market_spread,strategy_spread,"
            "queue_depth,events_processed,gaps_detected,events_recovered,"
            "recovery_misses,feed_state\n"
        );

        openFile(
            depth_file_,
            outputDirectory / "depth.csv",
            "timestamp_ns,event_index,side,price,quantity,level\n"
        );

        openFile(
            quotes_file_,
            outputDirectory / "quotes.csv",
            "timestamp_ns,event_index,order_id,side,price,quantity,accepted\n"
        );

        openFile(
            fills_file_,
            outputDirectory / "fills.csv",
            "timestamp_ns,event_index,order_id,side,price,quantity,"
            "was_maker,mid_at_fill\n"
        );

        openFile(
            latency_file_,
            outputDirectory / "latency.csv",
            "timestamp_ns,operation,latency_ns\n"
        );

        openFile(
            recovery_file_,
            outputDirectory / "recovery.csv",
            "timestamp_ns,wall_time_ns,event_index,event,feed_state,"
            "expected_sequence,received_sequence,gap_size,buffer_depth,"
            "duration_ns\n"
        );

        setPrecision(state_file_);
        setPrecision(depth_file_);
        setPrecision(quotes_file_);
        setPrecision(fills_file_);
        setPrecision(latency_file_);
        setPrecision(recovery_file_);
    }

    void recordState(const StateSample& sample) {
        state_file_
            << sample.timestamp_ns << ','
            << sample.wall_time_ns << ','
            << sample.event_index << ','
            << sample.participant_id << ','
            << sample.symbol_id << ','
            << sample.best_bid << ','
            << sample.best_ask << ','
            << sample.mid_price << ','
            << sample.strategy_bid << ','
            << sample.strategy_ask << ','
            << sample.realised_pnl << ','
            << sample.unrealised_pnl << ','
            << sample.total_pnl << ','
            << sample.cash_balance << ','
            << sample.account_equity << ','
            << sample.inventory << ','
            << sample.maximum_absolute_position << ','
            << sample.market_spread << ','
            << sample.strategy_spread << ','
            << sample.queue_depth << ','
            << sample.events_processed << ','
            << sample.gaps_detected << ','
            << sample.events_recovered << ','
            << sample.recovery_misses << ','
            << sample.feed_state << '\n';
    }

    void recordDepth(const DepthSample& sample) {
        depth_file_
            << sample.timestamp_ns << ','
            << sample.event_index << ','
            << sample.side << ','
            << sample.price << ','
            << sample.quantity << ','
            << sample.level << '\n';
    }

    void recordQuote(const QuoteSample& sample) {
        quotes_file_
            << sample.timestamp_ns << ','
            << sample.event_index << ','
            << sample.order_id << ','
            << sample.side << ','
            << sample.price << ','
            << sample.quantity << ','
            << static_cast<int>(sample.accepted) << '\n';
    }

    void recordFill(const FillSample& sample) {
        fills_file_
            << sample.timestamp_ns << ','
            << sample.event_index << ','
            << sample.order_id << ','
            << sample.side << ','
            << sample.price << ','
            << sample.quantity << ','
            << static_cast<int>(sample.was_maker) << ','
            << sample.mid_at_fill << '\n';
    }

    void recordLatency(const LatencySample& sample) {
        latency_file_
            << sample.timestamp_ns << ','
            << sample.operation << ','
            << sample.latency_ns << '\n';
    }

    void recordRecovery(const RecoverySample& sample) {
        recovery_file_
            << sample.timestamp_ns << ','
            << sample.wall_time_ns << ','
            << sample.event_index << ','
            << sample.event << ','
            << sample.feed_state << ','
            << sample.expected_sequence << ','
            << sample.received_sequence << ','
            << sample.gap_size << ','
            << sample.buffer_depth << ','
            << sample.duration_ns << '\n';
    }

    void flush() {
        state_file_.flush();
        depth_file_.flush();
        quotes_file_.flush();
        fills_file_.flush();
        latency_file_.flush();
        recovery_file_.flush();
    }

private:
    static void openFile(
        std::ofstream& stream,
        const std::filesystem::path& path,
        const std::string& header
    ) {
        stream.open(
            path,
            std::ios::out | std::ios::trunc
        );

        if (!stream.is_open()) {
            throw std::runtime_error(
                "Unable to open telemetry output: " +
                path.string()
            );
        }

        stream << header;
    }

    static void setPrecision(std::ofstream& stream) {
        stream
            << std::fixed
            << std::setprecision(6);
    }

    std::ofstream state_file_;
    std::ofstream depth_file_;
    std::ofstream quotes_file_;
    std::ofstream fills_file_;
    std::ofstream latency_file_;
    std::ofstream recovery_file_;
};

/*
    Leave this as nullptr while unit tests are running.
    Assign it only around the simulation/benchmark you want to measure.
*/
inline TelemetryRecorder* active_recorder = nullptr;

class ScopedLatency {
public:
    explicit ScopedLatency(std::string operation)
        : operation_(std::move(operation)),
          start_ns_(wallClockTimestampNs()) {
    }

    ~ScopedLatency() {
        if (active_recorder == nullptr) {
            return;
        }

        const std::uint64_t endNs =
            wallClockTimestampNs();

        active_recorder->recordLatency(
            LatencySample{
                endNs,
                operation_,
                endNs - start_ns_
            }
        );
    }

    ScopedLatency(const ScopedLatency&) = delete;
    ScopedLatency& operator=(const ScopedLatency&) = delete;

private:
    std::string operation_;
    std::uint64_t start_ns_;
};

} // namespace analytics