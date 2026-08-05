#include "binary_codec.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>


std::uint64_t steadyTimestampNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<
            std::chrono::nanoseconds
        >(
            std::chrono::steady_clock::
                now().time_since_epoch()
        ).count()
    );
}


std::uint32_t calculateChecksum(
    const std::vector<std::uint8_t>& bytes,
    std::size_t count
) {
    constexpr std::uint32_t FnvOffsetBasis = 2166136261u;
    constexpr std::uint32_t FnvPrime = 16777619u;

    std::uint32_t hash = FnvOffsetBasis;

    for (
        std::size_t index = 0;
        index < count;
        ++index
    ) {
        hash ^= bytes[index];
        hash *= FnvPrime;
    }

    return hash;
}

std::vector<std::uint8_t> serialiseEvent(
    const MarketEvent& event
) {
    std::vector<std::uint8_t> record;
    record.reserve(BinaryFormat::RecordSize);

    appendLittleEndian(
        record,
        BinaryFormat::RecordMagic
    );

    appendLittleEndian(
        record,
        BinaryFormat::RecordSize
    );

    appendLittleEndian(
        record,
        BinaryFormat::RecordVersion
    );

    appendLittleEndian(
        record,
        static_cast<std::uint8_t>(event.type)
    );

    appendLittleEndian(
        record,
        event.sequence_number
    );

    appendLittleEndian(
        record,
        event.timestamp_ns
    );

    appendLittleEndian(
        record,
        event.order_id
    );

    appendLittleEndian(
        record,
        event.price
    );

    appendLittleEndian(
        record,
        event.quantity
    );

    appendLittleEndian(
        record,
        static_cast<std::uint8_t>(event.side)
    );
    appendLittleEndian(
    record,
    event.participant_id
);

    appendLittleEndian(
        record,
        event.symbol_id
    );

    // Reserved bytes for future format extensions.
    appendLittleEndian(record, std::uint8_t{0});
    appendLittleEndian(record, std::uint8_t{0});
    appendLittleEndian(record, std::uint8_t{0});

    std::uint32_t checksum = calculateChecksum(
        record,
        record.size()
    );

    appendLittleEndian(record, checksum);

    return record;
}

UdpDatagram makeUdpDatagram(
    const MarketEvent& event
) {
    std::vector<std::uint8_t> record =
        serialiseEvent(event);

    if (
        record.size() !=
        BinaryFormat::RecordSize
    ) {
        throw std::runtime_error(
            "Unable to create UDP datagram"
        );
    }

    UdpDatagram datagram;

    std::copy(
        record.begin(),
        record.end(),
        datagram.bytes.begin()
    );

    datagram.size = record.size();

    datagram.received_timestamp_ns =
        steadyTimestampNs();

    return datagram;
}


EventRecordDecodeResult decodeEventRecord(
    const std::uint8_t* data,
    std::size_t size
) {
    EventRecordDecodeResult result;

    if (data == nullptr) {
        result.error = "null record data";
        return result;
    }

    if (size != BinaryFormat::RecordSize) {
        result.error =
            "unexpected UDP record size";

        return result;
    }

    std::vector<std::uint8_t> record(
        data,
        data + size
    );

    std::uint32_t storedChecksum = 0;

    std::size_t checksumOffset =
        BinaryFormat::RecordPayloadSize;

    if (!readLittleEndian(
        record,
        checksumOffset,
        storedChecksum
    )) {
        result.error =
            "unable to decode checksum";

        return result;
    }

    std::uint32_t calculatedChecksum =
        calculateChecksum(
            record,
            BinaryFormat::RecordPayloadSize
        );

    if (storedChecksum != calculatedChecksum) {
        result.error = "checksum mismatch";
        return result;
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
        result.error =
            "unable to decode UDP record";

        return result;
    }

    if (
        recordMagic !=
        BinaryFormat::RecordMagic
    ) {
        result.error = "invalid record magic";
        return result;
    }

    if (
        recordSize !=
        BinaryFormat::RecordSize
    ) {
        result.error = "invalid record size";
        return result;
    }

    if (
        recordVersion !=
        BinaryFormat::RecordVersion
    ) {
        result.error =
            "unsupported record version";

        return result;
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
        result.error = "invalid event type";
        return result;
    }

    if (sideValue > 1) {
        result.error = "invalid side";
        return result;
    }

    MarketEvent event{
        sequenceNumber,
        timestampNs,
        static_cast<EventType>(
            eventTypeValue
        ),
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
        result.error = validationReason;
        return result;
    }

    result.success = true;
    result.event = event;

    return result;
}