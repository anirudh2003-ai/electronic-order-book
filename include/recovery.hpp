#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include "binary_codec.hpp"
#include "replay_engine.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class RecoverySource {
public:
    virtual ~RecoverySource() = default;

    virtual std::optional<MarketEvent>
    fetch(
        std::uint64_t sequenceNumber
    ) = 0;
};

class InMemoryRecoverySource final
    : public RecoverySource {
public:
    explicit InMemoryRecoverySource(
        const std::vector<MarketEvent>& events
    ) {
        for (const MarketEvent& event : events) {
            events_[event.sequence_number] =
                event;
        }
    }

    std::optional<MarketEvent> fetch(
        std::uint64_t sequenceNumber
    ) override {
        auto iterator =
            events_.find(sequenceNumber);

        if (iterator == events_.end()) {
            return std::nullopt;
        }

        return iterator->second;
    }

private:
    std::map<
        std::uint64_t,
        MarketEvent
    > events_;
};

class TcpRecoverySource final
    : public RecoverySource {
public:
    TcpRecoverySource(
        std::string serverAddress,
        std::uint16_t serverPort
    )
        : server_address_(
              std::move(serverAddress)
          ),
          server_port_(serverPort) {
    }

    std::optional<MarketEvent> fetch(
        std::uint64_t sequenceNumber
    ) override {
        last_error_.clear();

        SOCKET recoverySocket =
            socket(
                AF_INET,
                SOCK_STREAM,
                IPPROTO_TCP
            );

        if (
            recoverySocket ==
            INVALID_SOCKET
        ) {
            last_error_ =
                "Unable to create TCP socket";

            return std::nullopt;
        }

        sockaddr_in server{};

        server.sin_family = AF_INET;
        server.sin_port =
            htons(server_port_);

        int addressResult = inet_pton(
            AF_INET,
            server_address_.c_str(),
            &server.sin_addr
        );

        if (addressResult != 1) {
            closesocket(recoverySocket);

            last_error_ =
                "Invalid recovery-server address";

            return std::nullopt;
        }

        int connectionResult = connect(
            recoverySocket,
            reinterpret_cast<
                const sockaddr*
            >(&server),
            sizeof(server)
        );

        if (
            connectionResult ==
            SOCKET_ERROR
        ) {
            closesocket(recoverySocket);

            last_error_ =
                "Unable to connect to recovery server";

            return std::nullopt;
        }

        /*
            Request protocol:

            8 bytes: missing sequence number,
            little-endian.
        */
        std::vector<std::uint8_t> request;

        appendLittleEndian(
            request,
            sequenceNumber
        );

        if (!sendAll(
            recoverySocket,
            request.data(),
            request.size()
        )) {
            closesocket(recoverySocket);

            last_error_ =
                "Unable to send recovery request";

            return std::nullopt;
        }

        /*
            Response protocol:

            status = 0: sequence was not found.
            status = 1: a 64-byte event follows.
        */
        std::uint8_t status = 0;

        if (!receiveAll(
            recoverySocket,
            &status,
            sizeof(status)
        )) {
            closesocket(recoverySocket);

            last_error_ =
                "Unable to receive recovery status";

            return std::nullopt;
        }

        if (status != 1) {
            closesocket(recoverySocket);

            last_error_ =
                "Recovery sequence was not found";

            return std::nullopt;
        }

        std::array<
            std::uint8_t,
            BinaryFormat::RecordSize
        > record{};

        if (!receiveAll(
            recoverySocket,
            record.data(),
            record.size()
        )) {
            closesocket(recoverySocket);

            last_error_ =
                "Unable to receive recovery event";

            return std::nullopt;
        }

        closesocket(recoverySocket);

        EventRecordDecodeResult decoded =
            decodeEventRecord(
                record.data(),
                record.size()
            );

        if (!decoded.success) {
            last_error_ =
                decoded.error;

            return std::nullopt;
        }

        if (
            decoded.event.sequence_number !=
            sequenceNumber
        ) {
            last_error_ =
                "Recovery server returned "
                "the wrong sequence";

            return std::nullopt;
        }

        return decoded.event;
    }

    const std::string& lastError() const {
        return last_error_;
    }

private:
    static bool sendAll(
        SOCKET socketHandle,
        const std::uint8_t* data,
        std::size_t size
    ) {
        std::size_t sentTotal = 0;

        while (sentTotal < size) {
            int sent = send(
                socketHandle,
                reinterpret_cast<
                    const char*
                >(data + sentTotal),
                static_cast<int>(
                    size - sentTotal
                ),
                0
            );

            if (
                sent == SOCKET_ERROR ||
                sent == 0
            ) {
                return false;
            }

            sentTotal +=
                static_cast<std::size_t>(
                    sent
                );
        }

        return true;
    }

    static bool receiveAll(
        SOCKET socketHandle,
        std::uint8_t* data,
        std::size_t size
    ) {
        std::size_t receivedTotal = 0;

        while (receivedTotal < size) {
            int received = recv(
                socketHandle,
                reinterpret_cast<char*>(
                    data + receivedTotal
                ),
                static_cast<int>(
                    size - receivedTotal
                ),
                0
            );

            if (
                received == SOCKET_ERROR ||
                received == 0
            ) {
                return false;
            }

            receivedTotal +=
                static_cast<std::size_t>(
                    received
                );
        }

        return true;
    }

    std::string server_address_;
    std::uint16_t server_port_;

    std::string last_error_;
};




enum class FeedState {
    Healthy,
    Recovering,
    Failed
};

struct RecoveryStatistics {
    std::uint64_t events_received{0};
    std::uint64_t events_applied{0};

    std::uint64_t gaps_detected{0};
    std::uint64_t out_of_order_events{0};
    std::uint64_t duplicates_ignored{0};

    std::uint64_t events_recovered{0};
    std::uint64_t recovery_misses{0};

    std::uint64_t application_rejections{0};
    std::uint64_t reorder_buffer_overflows{0};

    std::size_t maximum_buffer_depth{0};
};


class RecoverySequencer {
public:
    RecoverySequencer(
        std::uint64_t expectedFirstSequence,
        std::size_t maximumReorderDepth,
        RecoverySource& recoverySource
    )
        : expected_sequence_(
              expectedFirstSequence
          ),
          maximum_reorder_depth_(
              maximumReorderDepth
          ),
          recovery_source_(
              recoverySource
          ) {
        if (expectedFirstSequence == 0) {
            throw std::invalid_argument(
                "Expected sequence cannot be zero"
            );
        }

        if (maximumReorderDepth == 0) {
            throw std::invalid_argument(
                "Maximum reorder depth cannot be zero"
            );
        }
    }

    bool onEvent(const MarketEvent& event) {
        if (state_ == FeedState::Failed) {
            return false;
        }

        ++statistics_.events_received;

        if (
            event.sequence_number <
            expected_sequence_
        ) {
            ++statistics_.duplicates_ignored;
            return true;
        }

        if (
            event.sequence_number ==
            expected_sequence_
        ) {
            if (!applyExpectedEvent(event)) {
                return false;
            }

            return drainContiguousEvents();
        }

        // Event is ahead of the expected sequence.
        ++statistics_.out_of_order_events;
        ++statistics_.gaps_detected;

        if (
            event.sequence_number -
                expected_sequence_ >
            maximum_reorder_depth_
        ) {
            ++statistics_
                .reorder_buffer_overflows;

            state_ = FeedState::Failed;
            return false;
        }

        auto [iterator, inserted] =
            reorder_buffer_.try_emplace(
                event.sequence_number,
                event
            );

        static_cast<void>(iterator);

        if (!inserted) {
            ++statistics_.duplicates_ignored;
            return true;
        }

        updateMaximumBufferDepth();

        state_ = FeedState::Recovering;

        /*
            Do not recover inside onEvent().

            Return control to the processing loop first so the
            strategy can cancel its quotes before recovery begins.
        */
        return true;
    }

    bool retryRecovery() {
        if (
            state_ == FeedState::Failed ||
            reorder_buffer_.empty()
        ) {
            return state_ != FeedState::Failed;
        }

        std::uint64_t highestBufferedSequence =
            reorder_buffer_.rbegin()->first;

        attemptRecoveryUntil(
            highestBufferedSequence
        );

        return drainContiguousEvents();
    }

    FeedState state() const {
        return state_;
    }

    bool healthy() const {
        return state_ == FeedState::Healthy;
    }

    std::uint64_t expectedSequence() const {
        return expected_sequence_;
    }

    std::size_t bufferedEventCount() const {
        return reorder_buffer_.size();
    }

    const RecoveryStatistics&
    statistics() const {
        return statistics_;
    }

private:
    void attemptRecoveryUntil(
        std::uint64_t sequenceExclusive
    ) {
        for (
            std::uint64_t sequence =
                expected_sequence_;
            sequence < sequenceExclusive;
            ++sequence
        ) {
            if (
                reorder_buffer_.contains(
                    sequence
                )
            ) {
                continue;
            }

            std::optional<MarketEvent>
                recovered =
                    recovery_source_.fetch(
                        sequence
                    );

            if (!recovered.has_value()) {
                ++statistics_.recovery_misses;
                return;
            }

            reorder_buffer_.emplace(
                sequence,
                recovered.value()
            );

            ++statistics_.events_recovered;

            updateMaximumBufferDepth();
        }
    }

    bool drainContiguousEvents() {
        while (true) {
            auto iterator =
                reorder_buffer_.find(
                    expected_sequence_
                );

            if (
                iterator ==
                reorder_buffer_.end()
            ) {
                break;
            }

            MarketEvent event =
                iterator->second;

            reorder_buffer_.erase(iterator);

            if (!applyExpectedEvent(event)) {
                return false;
            }
        }

        if (reorder_buffer_.empty()) {
            state_ = FeedState::Healthy;
        } else {
            state_ = FeedState::Recovering;
        }

        return true;
    }

    bool applyExpectedEvent(
        const MarketEvent& event
    ) {
        if (
            event.sequence_number !=
            expected_sequence_
        ) {
            state_ = FeedState::Failed;
            return false;
        }

        ApplyEventResult application =
            applyMarketEvent(event);

        if (!application.accepted) {
            ++statistics_
                .application_rejections;

            state_ = FeedState::Failed;
            return false;
        }

        ++statistics_.events_applied;

        if (
            expected_sequence_ ==
            std::numeric_limits<
                std::uint64_t
            >::max()
        ) {
            state_ = FeedState::Failed;
            return false;
        }

        ++expected_sequence_;
        return true;
    }

    void updateMaximumBufferDepth() {
        statistics_.maximum_buffer_depth =
            std::max(
                statistics_
                    .maximum_buffer_depth,
                reorder_buffer_.size()
            );
    }

    std::uint64_t expected_sequence_;
    std::size_t maximum_reorder_depth_;

    RecoverySource& recovery_source_;

    std::map<
        std::uint64_t,
        MarketEvent
    > reorder_buffer_;

    FeedState state_{FeedState::Healthy};

    RecoveryStatistics statistics_;
};