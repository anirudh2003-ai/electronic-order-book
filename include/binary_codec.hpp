#pragma once

#include "market_event.hpp"
#include "spsc_ring_buffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

namespace BinaryFormat {

    constexpr std::array<std::uint8_t, 8> FileMagic{
        'O', 'B', 'R', 'E', 'P', 'L', 'A', 'Y'
    };

    constexpr std::uint16_t FileVersion = 2;
    constexpr std::uint16_t FileHeaderSize = 20;

    constexpr std::uint32_t RecordMagic =
        0x544E5645;

    constexpr std::uint8_t RecordVersion = 2;

    constexpr std::uint16_t RecordSize = 64;
    constexpr std::size_t RecordPayloadSize = 60;

} // namespace BinaryFormat
struct UdpDatagram {
    std::array<
        std::uint8_t,
        BinaryFormat::RecordSize
    > bytes{};

    std::size_t size{0};

    std::uint64_t received_timestamp_ns{0};
};

using NetworkQueue =
    SpscRingBuffer<
        UdpDatagram,
        4096
    >;

struct EventRecordDecodeResult {
    bool success{false};
    MarketEvent event{};
    std::string error;
};

std::uint64_t steadyTimestampNs();

std::uint32_t calculateChecksum(
    const std::vector<std::uint8_t>& bytes,
    std::size_t count
);

std::vector<std::uint8_t> serialiseEvent(
    const MarketEvent& event
);

UdpDatagram makeUdpDatagram(
    const MarketEvent& event
);

EventRecordDecodeResult decodeEventRecord(
    const std::uint8_t* data,
    std::size_t size
);

template<typename T>
void appendLittleEndian(
    std::vector<std::uint8_t>& buffer,
    T value
) {
    static_assert(std::is_integral_v<T>);

    using UnsignedType = std::make_unsigned_t<T>;

    UnsignedType bits = 0;
    std::memcpy(&bits, &value, sizeof(T));

    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
        buffer.push_back(
            static_cast<std::uint8_t>(
                (bits >> (byte * 8)) & 0xFF
            )
        );
    }
}

template<typename T>
bool readLittleEndian(
    const std::vector<std::uint8_t>& buffer,
    std::size_t& offset,
    T& output
) {
    static_assert(std::is_integral_v<T>);

    if (offset + sizeof(T) > buffer.size()) {
        return false;
    }

    using UnsignedType = std::make_unsigned_t<T>;

    UnsignedType bits = 0;

    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
        bits |= (
            static_cast<UnsignedType>(buffer[offset + byte])
            << (byte * 8)
        );
    }

    std::memcpy(&output, &bits, sizeof(T));
    offset += sizeof(T);

    return true;
}

class BinaryEventWriter {
public:
    static bool write(
        const std::filesystem::path& filePath,
        const std::vector<MarketEvent>& events,
        std::string& error
    ) {
        for (
            std::size_t index = 0;
            index < events.size();
            ++index
        ) {
            std::string validationReason;

            if (!validateMarketEvent(
                events[index],
                validationReason
            )) {
                error =
                    "Invalid event at index " +
                    std::to_string(index) +
                    ": " +
                    validationReason;

                return false;
            }
        }

        std::ofstream output(
            filePath,
            std::ios::binary | std::ios::trunc
        );

        if (!output.is_open()) {
            error =
                "Unable to open binary output file: " +
                filePath.string();

            return false;
        }

        output.write(
            reinterpret_cast<const char*>(
                BinaryFormat::FileMagic.data()
            ),
            static_cast<std::streamsize>(
                BinaryFormat::FileMagic.size()
            )
        );

        std::vector<std::uint8_t> header;

        appendLittleEndian(
            header,
            BinaryFormat::FileVersion
        );

        appendLittleEndian(
            header,
            BinaryFormat::FileHeaderSize
        );

        appendLittleEndian(
            header,
            static_cast<std::uint64_t>(events.size())
        );

        output.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size())
        );

        for (const MarketEvent& event : events) {
            std::vector<std::uint8_t> record =
                serialiseEvent(event);

            if (record.size() != BinaryFormat::RecordSize) {
                error = "Internal binary record-size error";
                return false;
            }

            output.write(
                reinterpret_cast<const char*>(record.data()),
                static_cast<std::streamsize>(record.size())
            );

            if (!output.good()) {
                error = "Failed while writing binary event";
                return false;
            }
        }

        output.flush();

        if (!output.good()) {
            error = "Failed to flush binary output file";
            return false;
        }

        return true;
    }
};

// ============================================================
// BINARY PARSER
// ============================================================

struct ParseStatistics {
    std::uint64_t records_declared{0};
    std::uint64_t records_read{0};
    std::uint64_t valid_records{0};
    std::uint64_t corrupt_records{0};
    std::uint64_t truncated_records{0};
};

struct ParseResult {
    bool success{false};
    std::vector<MarketEvent> events;
    std::vector<std::string> errors;
    ParseStatistics statistics;
};

class BinaryEventParser {
public:
    static ParseResult parse(
        const std::filesystem::path& filePath
    ) {
        ParseResult result;

        std::ifstream input(
            filePath,
            std::ios::binary
        );

        if (!input.is_open()) {
            result.errors.push_back(
                "Unable to open binary input file: " +
                filePath.string()
            );

            return result;
        }

        std::array<std::uint8_t, 8> magic{};

        input.read(
            reinterpret_cast<char*>(magic.data()),
            static_cast<std::streamsize>(magic.size())
        );

        if (
            input.gcount() !=
            static_cast<std::streamsize>(magic.size())
        ) {
            result.errors.push_back(
                "File is too small to contain a valid header"
            );

            return result;
        }

        if (magic != BinaryFormat::FileMagic) {
            result.errors.push_back(
                "Invalid binary file magic"
            );

            return result;
        }

        constexpr std::size_t RemainingHeaderSize =
            BinaryFormat::FileHeaderSize -
            BinaryFormat::FileMagic.size();

        std::vector<std::uint8_t> header(
            RemainingHeaderSize
        );

        input.read(
            reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size())
        );

        if (
            input.gcount() !=
            static_cast<std::streamsize>(header.size())
        ) {
            result.errors.push_back(
                "Truncated binary file header"
            );

            return result;
        }

        std::size_t headerOffset = 0;

        std::uint16_t fileVersion = 0;
        std::uint16_t headerSize = 0;
        std::uint64_t recordCount = 0;

        if (
            !readLittleEndian(
                header,
                headerOffset,
                fileVersion
            ) ||
            !readLittleEndian(
                header,
                headerOffset,
                headerSize
            ) ||
            !readLittleEndian(
                header,
                headerOffset,
                recordCount
            )
        ) {
            result.errors.push_back(
                "Unable to decode binary file header"
            );

            return result;
        }

        if (fileVersion != BinaryFormat::FileVersion) {
            result.errors.push_back(
                "Unsupported binary file version: " +
                std::to_string(fileVersion)
            );

            return result;
        }

        if (headerSize != BinaryFormat::FileHeaderSize) {
            result.errors.push_back(
                "Unexpected binary header size"
            );

            return result;
        }

        result.statistics.records_declared = recordCount;
        result.events.reserve(
            static_cast<std::size_t>(recordCount)
        );

        for (
            std::uint64_t recordIndex = 0;
            recordIndex < recordCount;
            ++recordIndex
        ) {
            std::vector<std::uint8_t> record(
                BinaryFormat::RecordSize
            );

            input.read(
                reinterpret_cast<char*>(record.data()),
                static_cast<std::streamsize>(record.size())
            );

            if (
                input.gcount() !=
                static_cast<std::streamsize>(record.size())
            ) {
                ++result.statistics.truncated_records;

                result.errors.push_back(
                    "Truncated record at index " +
                    std::to_string(recordIndex)
                );

                break;
            }

            ++result.statistics.records_read;

            std::uint32_t storedChecksum = 0;
            std::size_t checksumOffset =
                BinaryFormat::RecordPayloadSize;

            if (!readLittleEndian(
                record,
                checksumOffset,
                storedChecksum
            )) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Unable to decode checksum at record " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            std::uint32_t calculatedChecksum =
                calculateChecksum(
                    record,
                    BinaryFormat::RecordPayloadSize
                );

            if (storedChecksum != calculatedChecksum) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Checksum mismatch at record " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            std::size_t offset = 0;

            std::uint32_t recordMagic = 0;
            std::uint16_t recordSize = 0;
            std::uint8_t recordVersion = 0;
            std::uint8_t eventTypeValue = 0;
            std::uint64_t sequenceNumber = 0;
            std::uint64_t timestampNs = 0;
            OrderId orderId = 0;
            Price price = 0;
            Quantity quantity = 0;
            std::uint8_t sideValue = 0;

            ParticipantId participantId =
    UnknownParticipant;

            SymbolId symbolId =
                DefaultSymbol;

            std::uint8_t reserved0 = 0;
            std::uint8_t reserved1 = 0;
            std::uint8_t reserved2 = 0;

            bool decoded =
                readLittleEndian(
                    record,
                    offset,
                    recordMagic
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    recordSize
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    recordVersion
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    eventTypeValue
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    sequenceNumber
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    timestampNs
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    orderId
                ) &&
                readLittleEndian(
                    record,
                    offset,
                    price
                ) &&
                    readLittleEndian(
        record,
        offset,
        quantity
    ) &&
    readLittleEndian(
        record,
        offset,
        sideValue
    ) &&
    readLittleEndian(
        record,
        offset,
        participantId
    ) &&
    readLittleEndian(
        record,
        offset,
        symbolId
    ) &&
    readLittleEndian(
        record,
        offset,
        reserved0
    ) &&
    readLittleEndian(
        record,
        offset,
        reserved1
    ) &&
    readLittleEndian(
        record,
        offset,
        reserved2
    );

            if (!decoded) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Unable to decode record " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (recordMagic != BinaryFormat::RecordMagic) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid record magic at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (recordSize != BinaryFormat::RecordSize) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid record size at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (
                recordVersion !=
                BinaryFormat::RecordVersion
            ) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Unsupported record version at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (
                eventTypeValue <
                    static_cast<std::uint8_t>(
                        EventType::Add
                    ) ||
                eventTypeValue >
                    static_cast<std::uint8_t>(
                        EventType::Execute
                    )
            ) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid event type at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            if (sideValue > 1) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid side value at index " +
                    std::to_string(recordIndex)
                );

                continue;
            }

            MarketEvent event{
                sequenceNumber,
                timestampNs,
                static_cast<EventType>(eventTypeValue),
                orderId,
                static_cast<Side>(sideValue),
                price,
                quantity,
                participantId,
                symbolId
            };

            std::string validationReason;

            if (!validateMarketEvent(
                event,
                validationReason
            )) {
                ++result.statistics.corrupt_records;

                result.errors.push_back(
                    "Invalid event at record " +
                    std::to_string(recordIndex) +
                    ": " +
                    validationReason
                );

                continue;
            }

            result.events.push_back(event);
            ++result.statistics.valid_records;
        }

        result.success = result.errors.empty();
        return result;
    }
};