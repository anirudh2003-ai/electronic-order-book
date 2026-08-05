#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include "binary_codec.hpp"
#include "market_maker_strategy.hpp"
#include "pnl_engine.hpp"
#include "recovery.hpp"

#include "analytics/simulation_telemetry_bridge.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

struct UdpReceiverStatistics {
    std::uint64_t packets_received{0};
    std::uint64_t packets_queued{0};
    std::uint64_t invalid_size_packets{0};
    std::uint64_t queue_full_drops{0};
    std::uint64_t socket_errors{0};
};

class UdpReceiver {
public:
    UdpReceiver(
        NetworkQueue& queue,
        std::uint16_t port
    )
        : queue_(queue),
          port_(port) {
        socket_ = socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP
        );

        if (socket_ == INVALID_SOCKET) {
            throw std::runtime_error(
                "Unable to create UDP socket: " +
                std::to_string(
                    WSAGetLastError()
                )
            );
        }

        sockaddr_in address{};

        address.sin_family = AF_INET;
        address.sin_addr.s_addr =
            htonl(INADDR_ANY);

        address.sin_port =
            htons(port_);

        int bindResult = bind(
            socket_,
            reinterpret_cast<
                const sockaddr*
            >(&address),
            sizeof(address)
        );

        if (bindResult == SOCKET_ERROR) {
            int error = WSAGetLastError();

            closesocket(socket_);
            socket_ = INVALID_SOCKET;

            throw std::runtime_error(
                "Unable to bind UDP socket: " +
                std::to_string(error)
            );
        }
    }

    ~UdpReceiver() {
        stop();

        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }

    UdpReceiver(
        const UdpReceiver&
    ) = delete;

    UdpReceiver& operator=(
        const UdpReceiver&
    ) = delete;

    void start() {
        bool expected = false;

        if (!running_.compare_exchange_strong(
            expected,
            true
        )) {
            return;
        }

        receiver_thread_ =
            std::thread(
                &UdpReceiver::receiveLoop,
                this
            );
    }

    void stop() {
        running_.store(false);

        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

    UdpReceiverStatistics statistics() const {
        return UdpReceiverStatistics{
            packets_received_.load(),
            packets_queued_.load(),
            invalid_size_packets_.load(),
            queue_full_drops_.load(),
            socket_errors_.load()
        };
    }

private:
    void receiveLoop() {
        /*
            Use a larger temporary buffer so oversized UDP
            packets can be identified and rejected cleanly.
        */
        std::array<char, 2048>
            receiveBuffer{};

        while (running_.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(socket_, &readSet);

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 100'000;

            int ready = select(
                0,
                &readSet,
                nullptr,
                nullptr,
                &timeout
            );

            if (ready == SOCKET_ERROR) {
                ++socket_errors_;
                continue;
            }

            if (ready == 0) {
                continue;
            }

            sockaddr_storage senderAddress{};
            int senderAddressSize =
                sizeof(senderAddress);

            int receivedBytes = recvfrom(
                socket_,
                receiveBuffer.data(),
                static_cast<int>(
                    receiveBuffer.size()
                ),
                0,
                reinterpret_cast<sockaddr*>(
                    &senderAddress
                ),
                &senderAddressSize
            );

            if (receivedBytes == SOCKET_ERROR) {
                ++socket_errors_;
                continue;
            }

            ++packets_received_;

            if (
                receivedBytes !=
                static_cast<int>(
                    BinaryFormat::RecordSize
                )
            ) {
                ++invalid_size_packets_;
                continue;
            }

            UdpDatagram datagram;

            std::memcpy(
                datagram.bytes.data(),
                receiveBuffer.data(),
                BinaryFormat::RecordSize
            );

            datagram.size =
                BinaryFormat::RecordSize;

            datagram.received_timestamp_ns =
                steadyTimestampNs();

            if (!queue_.tryPush(datagram)) {
                ++queue_full_drops_;
                continue;
            }

            ++packets_queued_;
        }
    }

    NetworkQueue& queue_;
    std::uint16_t port_;

    SOCKET socket_{INVALID_SOCKET};

    std::atomic<bool> running_{false};
    std::thread receiver_thread_;

    std::atomic<std::uint64_t>
        packets_received_{0};

    std::atomic<std::uint64_t>
        packets_queued_{0};

    std::atomic<std::uint64_t>
        invalid_size_packets_{0};

    std::atomic<std::uint64_t>
        queue_full_drops_{0};

    std::atomic<std::uint64_t>
        socket_errors_{0};
};


struct UdpProcessingStatistics {
    std::uint64_t datagrams_processed{0};
    std::uint64_t decode_failures{0};
    std::uint64_t sequencer_failures{0};

    std::uint64_t recovery_pauses{0};
    std::uint64_t recovery_resumes{0};

    std::uint64_t quote_refreshes{0};
};

class UdpFeedProcessor {
public:
    UdpFeedProcessor(
    NetworkQueue& queue,
    RecoverySequencer& sequencer,
    MarketMakerStrategy& strategy,
    PnlEngine& pnl,
    analytics::SimulationTelemetryBridge*
        telemetryBridge = nullptr,
    ParticipantId participantId = 5001,
    SymbolId symbolId = DefaultSymbol,
    bool updateStrategy = true,
    std::uint64_t telemetrySampleInterval = 1,
    std::size_t telemetryDepthLevels = 20
)
    : queue_(queue),
      sequencer_(sequencer),
      strategy_(strategy),
      pnl_(pnl),
      telemetry_bridge_(telemetryBridge),
      participant_id_(participantId),
      symbol_id_(symbolId),
      update_strategy_(updateStrategy),
      telemetry_sample_interval_(
          telemetrySampleInterval == 0
              ? 1
              : telemetrySampleInterval
      ),
      telemetry_depth_levels_(
          telemetryDepthLevels
      ) {
}

    std::size_t processAvailable() {
        std::size_t processedThisCall = 0;

        UdpDatagram datagram;

        while (queue_.tryPop(datagram)) {
            ++processedThisCall;
            ++statistics_
                .datagrams_processed;

            EventRecordDecodeResult decoded =
                decodeEventRecord(
                    datagram.bytes.data(),
                    datagram.size
                );

            if (!decoded.success) {
                ++statistics_.decode_failures;
                continue;
            }


            TimestampNs eventTimestamp = decoded.event.timestamp_ns;
            last_event_timestamp_ = eventTimestamp;

            ++telemetry_events_seen_;

            bool accepted =
                sequencer_.onEvent(
                    decoded.event
                );

            if (
                    !accepted ||
                    sequencer_.state() == FeedState::Failed
                ) {
                ++statistics_
                    .sequencer_failures;

                pauseStrategy(
                    eventTimestamp,
                    "Market-data feed failed"
                );

                failRecoveryTelemetry(
                    eventTimestamp
                );

                captureTelemetry(
    eventTimestamp,
    true
);

                continue;
}

            if (
                    sequencer_.state() == FeedState::Recovering
                ) {
                beginRecoveryTelemetry(
                    eventTimestamp,
                    decoded.event.sequence_number
                );

                pauseStrategy(
                    eventTimestamp,
                    "Market-data recovery in progress"
                );

                /*
                    Capture the Recovering state before attempting
                    to repair the gap.
                */
                captureTelemetry(
                    eventTimestamp,
                    true
                );

                bool recoverySucceeded =
                    sequencer_.retryRecovery();

                if (
                        !recoverySucceeded ||
                        sequencer_.state() ==
                            FeedState::Failed
                    ) {
                    ++statistics_
                        .sequencer_failures;

                    failRecoveryTelemetry(
                        eventTimestamp
                    );

                    captureTelemetry(
    eventTimestamp,
    true
);

                    continue;
                }
            }

            if (sequencer_.healthy()) {
                const bool recoveryJustCompleted =
                    telemetry_recovery_active_;

                finishRecoveryTelemetry(
                    eventTimestamp
                );

                resumeStrategyIfNecessary(
                    eventTimestamp
                );

                if (update_strategy_) {
                    updateStrategyAndAccounting(
                        eventTimestamp
                    );
                } else {
                    /*
                        In a market-data-only replay, do not submit
                        strategy quotes or alter the reconstructed book.
                    */
                    captureTelemetry(
                        eventTimestamp,
                        recoveryJustCompleted
                    );
                }
            }
        }

        /*
    Retry an incomplete recovery when the processing
    loop is called without receiving another datagram.
*/
        if (
    processedThisCall == 0 &&
    sequencer_.state() ==
        FeedState::Recovering &&
    last_event_timestamp_ != 0
) {
            pauseStrategy(
                last_event_timestamp_,
                "Market-data recovery in progress"
            );

            bool recoverySucceeded =
                sequencer_.retryRecovery();

            if (
                !recoverySucceeded ||
                sequencer_.state() ==
                    FeedState::Failed
            ) {
                ++statistics_.sequencer_failures;

                failRecoveryTelemetry(
                    last_event_timestamp_
                );

                captureTelemetry(
                    last_event_timestamp_,
                    true
                );
            } else if (sequencer_.healthy()) {
                const bool recoveryJustCompleted =
                    telemetry_recovery_active_;

                finishRecoveryTelemetry(
                    last_event_timestamp_
                );

                resumeStrategyIfNecessary(
                    last_event_timestamp_
                );

                if (update_strategy_) {
                    updateStrategyAndAccounting(
                        last_event_timestamp_
                    );
                } else {
                    captureTelemetry(
                        last_event_timestamp_,
                        recoveryJustCompleted
                    );
                }
            }
}

        return processedThisCall;
    }

    const UdpProcessingStatistics&
    statistics() const {
        return statistics_;
    }


private:
    void beginRecoveryTelemetry(
        TimestampNs timestamp,
        std::uint64_t receivedSequence
    ) {
        if (
            telemetry_bridge_ == nullptr ||
            telemetry_recovery_active_
        ) {
            return;
        }

        telemetry_bridge_->recordRecoveryStart(
            timestamp,
            sequencer_.expectedSequence(),
            receivedSequence,
            sequencer_.bufferedEventCount()
        );

        telemetry_recovery_active_ = true;
    }

    void finishRecoveryTelemetry(
        TimestampNs timestamp
    ) {
        if (
            telemetry_bridge_ == nullptr ||
            !telemetry_recovery_active_
        ) {
            return;
        }

        telemetry_bridge_->recordRecoveryEnd(
            timestamp,
            sequencer_.expectedSequence(),
            sequencer_.bufferedEventCount()
        );

        telemetry_recovery_active_ = false;
    }

    void failRecoveryTelemetry(
    TimestampNs timestamp
) {
        if (
            telemetry_bridge_ == nullptr ||
            !telemetry_recovery_active_
        ) {
            return;
        }

        telemetry_bridge_->recordRecoveryFailure(
            timestamp,
            sequencer_.expectedSequence(),
            sequencer_.bufferedEventCount()
        );

        telemetry_recovery_active_ = false;

    }

    void captureTelemetry(
    TimestampNs timestamp,
    bool force = false
) {
        if (telemetry_bridge_ == nullptr) {
            return;
        }

        /*
            Record ordinary state only at the configured sample
            interval. Recovery transitions are always forced.
        */
        if (
            !force &&
            telemetry_events_seen_ %
                telemetry_sample_interval_ !=
                0
        ) {
            return;
        }

        telemetry_bridge_->capture(
            timestamp,
            participant_id_,
            symbol_id_,
            pnl_,
            strategy_,
            &queue_,
            &sequencer_,
            telemetry_depth_levels_
        );
    }
    void pauseStrategy(
        TimestampNs timestamp,
        const std::string& reason
    ) {
        if (recovery_pause_active_) {
            return;
        }

        /*
            Do not claim ownership of a kill switch that was
            already active for another reason.
        */
        if (
            strategy_.riskManager()
                .killSwitchActive()
        ) {
            return;
        }

        strategy_.activateKillSwitch(
            timestamp,
            reason
        );

        recovery_pause_active_ = true;

        ++statistics_.recovery_pauses;
    }

    void resumeStrategyIfNecessary(
        TimestampNs timestamp
    ) {
        if (!recovery_pause_active_) {
            return;
        }

        strategy_.deactivateKillSwitch();

        recovery_pause_active_ = false;

        ++statistics_.recovery_resumes;

        /*
            The recovered book now represents the latest
            complete market-data state.
        */
        strategy_.onMarketUpdate(timestamp);
    }

    void updateStrategyAndAccounting(
        TimestampNs timestamp
    ) {
        /*
            First process trades generated by incoming
            market-data events.
        */
        strategy_.processTrades();

        pnl_.processNewTrades(
            tradeHistory
        );

        pnl_.markFromCurrentOrderBook(
            DefaultSymbol
        );

        strategy_.onMarketUpdate(timestamp);

        QuoteRefreshResult quotes =
            strategy_.refreshQuotes(
                timestamp
            );

        if (telemetry_bridge_ != nullptr) {
            telemetry_bridge_->recordQuoteRefresh(
                timestamp,
                quotes
            );
        }

        if (quotes.market_accepted) {
            ++statistics_.quote_refreshes;
        }

        /*
            Process any trades potentially generated while
            submitting the refreshed quotes.
        */
        strategy_.processTrades();

        pnl_.processNewTrades(
            tradeHistory
        );

        pnl_.markFromCurrentOrderBook(
            DefaultSymbol
        );
        captureTelemetry(timestamp);
    }

    NetworkQueue& queue_;
    RecoverySequencer& sequencer_;

    MarketMakerStrategy& strategy_;
    PnlEngine& pnl_;

    bool recovery_pause_active_{false};

    TimestampNs last_event_timestamp_{0};

    analytics::SimulationTelemetryBridge*
        telemetry_bridge_{nullptr};

    ParticipantId participant_id_{5001};
    SymbolId symbol_id_{DefaultSymbol};

    bool telemetry_recovery_active_{false};

    bool update_strategy_{true};

    std::uint64_t telemetry_sample_interval_{1};

    std::size_t telemetry_depth_levels_{20};

    std::uint64_t telemetry_events_seen_{0};

    UdpProcessingStatistics statistics_;
};