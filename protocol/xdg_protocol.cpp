#include "protocol/xdg_protocol.h"

#include <cstring>
#include <iterator>
#include <stdexcept>

namespace xdpg {
namespace {

constexpr std::size_t kInputPayloadSize = 32;
constexpr std::size_t kSnapshotPrefixSize = 16;
constexpr std::size_t kEntitySnapshotSize = 60;
constexpr std::size_t kPingPayloadSize = 8;
constexpr std::size_t kPongPayloadSize = 16;

// 这里不用 reinterpret_cast 直接读写整数，是为了避免 CPU 对齐、结构体 padding、
// 主机字节序等因素影响网络协议。当前协议固定为 little-endian，小端 helper 是
// 所有 encode/decode 的唯一入口。
void WriteU8(std::vector<std::uint8_t>& out, std::uint8_t value) {
    out.push_back(value);
}

void WriteU16Le(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void WriteU32Le(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift <= 24; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

void WriteU64Le(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift <= 56; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

void WriteF32Le(std::vector<std::uint8_t>& out, float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t), "float must be 32-bit");
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32Le(out, bits);
}

std::uint8_t ReadU8(const std::uint8_t* data, std::size_t& offset) {
    return data[offset++];
}

std::uint16_t ReadU16Le(const std::uint8_t* data, std::size_t& offset) {
    const std::uint16_t value =
        static_cast<std::uint16_t>(data[offset]) |
        static_cast<std::uint16_t>(data[offset + 1] << 8u);
    offset += 2;
    return value;
}

std::uint32_t ReadU32Le(const std::uint8_t* data, std::size_t& offset) {
    std::uint32_t value = 0;
    for (int shift = 0; shift <= 24; shift += 8) {
        value |= static_cast<std::uint32_t>(data[offset++]) << shift;
    }
    return value;
}

std::uint64_t ReadU64Le(const std::uint8_t* data, std::size_t& offset) {
    std::uint64_t value = 0;
    for (int shift = 0; shift <= 56; shift += 8) {
        value |= static_cast<std::uint64_t>(data[offset++]) << shift;
    }
    return value;
}

float ReadF32Le(const std::uint8_t* data, std::size_t& offset) {
    const std::uint32_t bits = ReadU32Le(data, offset);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool IsKnownPacketType(std::uint8_t value) {
    switch (static_cast<PacketType>(value)) {
        case PacketType::Input:
        case PacketType::Snapshot:
        case PacketType::Ping:
        case PacketType::Pong:
        case PacketType::Benchmark:
            return true;
    }
    return false;
}

void WriteHeader(
    std::vector<std::uint8_t>& out,
    PacketType packet_type,
    std::uint32_t sequence,
    std::uint16_t payload_size) {
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    WriteU8(out, kVersion);
    WriteU8(out, static_cast<std::uint8_t>(packet_type));
    WriteU16Le(out, kHeaderSize);
    WriteU32Le(out, sequence);
    WriteU16Le(out, payload_size);
    WriteU16Le(out, 0);
}

DecodedHeader DecodeHeaderInternal(const std::uint8_t* data, std::size_t size) {
    DecodedHeader decoded;

    if (size < kHeaderSize) {
        decoded.error = DecodeError::TooShort;
        return decoded;
    }
    if (size > kMaxUdpPayloadSize) {
        decoded.error = DecodeError::TooLarge;
        return decoded;
    }
    if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
        decoded.error = DecodeError::InvalidMagic;
        return decoded;
    }

    std::size_t offset = 4;
    const auto version = ReadU8(data, offset);
    if (version != kVersion) {
        decoded.error = DecodeError::InvalidVersion;
        return decoded;
    }

    const auto packet_type_raw = ReadU8(data, offset);
    if (!IsKnownPacketType(packet_type_raw)) {
        decoded.error = DecodeError::UnknownPacketType;
        return decoded;
    }

    const auto header_size = ReadU16Le(data, offset);
    if (header_size != kHeaderSize) {
        decoded.error = DecodeError::InvalidHeaderSize;
        return decoded;
    }

    decoded.header.packet_type = static_cast<PacketType>(packet_type_raw);
    decoded.header.sequence = ReadU32Le(data, offset);
    decoded.header.payload_size = ReadU16Le(data, offset);
    decoded.header.flags = ReadU16Le(data, offset);

    if (decoded.header.payload_size != size - kHeaderSize) {
        decoded.error = DecodeError::InvalidPayloadSize;
        return decoded;
    }
    if (decoded.header.flags != 0) {
        decoded.error = DecodeError::InvalidFlags;
        return decoded;
    }

    decoded.error = DecodeError::None;
    return decoded;
}

void WriteInputPayload(std::vector<std::uint8_t>& out, const InputPayload& payload) {
    WriteU64Le(out, payload.client_id);
    WriteU32Le(out, payload.input_sequence);
    WriteF32Le(out, payload.move_x);
    WriteF32Le(out, payload.move_z);
    WriteU32Le(out, payload.buttons);
    WriteU64Le(out, payload.client_timestamp_usec);
}

InputPayload ReadInputPayload(const std::uint8_t* data, std::size_t offset) {
    InputPayload payload;
    payload.client_id = ReadU64Le(data, offset);
    payload.input_sequence = ReadU32Le(data, offset);
    payload.move_x = ReadF32Le(data, offset);
    payload.move_z = ReadF32Le(data, offset);
    payload.buttons = ReadU32Le(data, offset);
    payload.client_timestamp_usec = ReadU64Le(data, offset);
    return payload;
}

void WriteEntitySnapshot(std::vector<std::uint8_t>& out, const EntitySnapshot& entity) {
    WriteU32Le(out, entity.entity_id);
    WriteU16Le(out, static_cast<std::uint16_t>(entity.entity_type));
    WriteU16Le(out, entity.flags);
    for (float value : entity.position) {
        WriteF32Le(out, value);
    }
    for (float value : entity.rotation) {
        WriteF32Le(out, value);
    }
    for (float value : entity.linear_velocity) {
        WriteF32Le(out, value);
    }
    for (float value : entity.angular_velocity) {
        WriteF32Le(out, value);
    }
}

EntitySnapshot ReadEntitySnapshot(const std::uint8_t* data, std::size_t& offset) {
    EntitySnapshot entity;
    entity.entity_id = ReadU32Le(data, offset);
    entity.entity_type = static_cast<EntityType>(ReadU16Le(data, offset));
    entity.flags = ReadU16Le(data, offset);
    for (float& value : entity.position) {
        value = ReadF32Le(data, offset);
    }
    for (float& value : entity.rotation) {
        value = ReadF32Le(data, offset);
    }
    for (float& value : entity.linear_velocity) {
        value = ReadF32Le(data, offset);
    }
    for (float& value : entity.angular_velocity) {
        value = ReadF32Le(data, offset);
    }
    return entity;
}

std::uint16_t CheckedPayloadSize(std::size_t size) {
    if (size > UINT16_MAX) {
        throw std::invalid_argument("payload too large for protocol header");
    }
    return static_cast<std::uint16_t>(size);
}

}  // namespace

const char* DecodeErrorName(DecodeError error) {
    switch (error) {
        case DecodeError::None:
            return "None";
        case DecodeError::TooShort:
            return "TooShort";
        case DecodeError::TooLarge:
            return "TooLarge";
        case DecodeError::InvalidMagic:
            return "InvalidMagic";
        case DecodeError::InvalidVersion:
            return "InvalidVersion";
        case DecodeError::InvalidHeaderSize:
            return "InvalidHeaderSize";
        case DecodeError::InvalidPayloadSize:
            return "InvalidPayloadSize";
        case DecodeError::InvalidFlags:
            return "InvalidFlags";
        case DecodeError::UnknownPacketType:
            return "UnknownPacketType";
        case DecodeError::InvalidPacketPayload:
            return "InvalidPacketPayload";
    }
    return "UnknownDecodeError";
}

const char* PacketTypeName(PacketType packet_type) {
    switch (packet_type) {
        case PacketType::Input:
            return "INPUT";
        case PacketType::Snapshot:
            return "SNAPSHOT";
        case PacketType::Ping:
            return "PING";
        case PacketType::Pong:
            return "PONG";
        case PacketType::Benchmark:
            return "BENCHMARK";
    }
    return "UNKNOWN";
}

std::vector<std::uint8_t> EncodeInput(std::uint32_t sequence, const InputPayload& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + kInputPayloadSize);
    WriteHeader(out, PacketType::Input, sequence, kInputPayloadSize);
    WriteInputPayload(out, payload);
    return out;
}

DecodedInput DecodeInput(const std::uint8_t* data, std::size_t size) {
    DecodedInput decoded;
    auto header = DecodeHeaderInternal(data, size);
    decoded.header = header.header;
    decoded.error = header.error;
    if (decoded.error != DecodeError::None) {
        return decoded;
    }
    if (decoded.header.packet_type != PacketType::Input ||
        decoded.header.payload_size != kInputPayloadSize) {
        decoded.error = DecodeError::InvalidPacketPayload;
        return decoded;
    }

    decoded.payload = ReadInputPayload(data, kHeaderSize);
    return decoded;
}

std::vector<std::uint8_t> EncodeSnapshot(std::uint32_t sequence, const SnapshotPayload& payload) {
    if (payload.entities.size() > kMaxSnapshotEntities) {
        throw std::invalid_argument("too many snapshot entities");
    }

    const std::size_t payload_size =
        kSnapshotPrefixSize + payload.entities.size() * kEntitySnapshotSize;
    if (kHeaderSize + payload_size > kMaxUdpPayloadSize) {
        throw std::invalid_argument("snapshot exceeds maximum UDP payload size");
    }

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload_size);
    WriteHeader(out, PacketType::Snapshot, sequence, CheckedPayloadSize(payload_size));
    WriteU64Le(out, payload.server_tick);
    WriteU32Le(out, payload.last_processed_input_sequence);
    WriteU16Le(out, static_cast<std::uint16_t>(payload.entities.size()));
    WriteU16Le(out, 0);
    for (const auto& entity : payload.entities) {
        WriteEntitySnapshot(out, entity);
    }
    return out;
}

DecodedSnapshot DecodeSnapshot(const std::uint8_t* data, std::size_t size) {
    DecodedSnapshot decoded;
    auto header = DecodeHeaderInternal(data, size);
    decoded.header = header.header;
    decoded.error = header.error;
    if (decoded.error != DecodeError::None) {
        return decoded;
    }
    if (decoded.header.packet_type != PacketType::Snapshot ||
        decoded.header.payload_size < kSnapshotPrefixSize) {
        decoded.error = DecodeError::InvalidPacketPayload;
        return decoded;
    }

    std::size_t offset = kHeaderSize;
    decoded.payload.server_tick = ReadU64Le(data, offset);
    decoded.payload.last_processed_input_sequence = ReadU32Le(data, offset);
    const auto entity_count = ReadU16Le(data, offset);
    const auto reserved = ReadU16Le(data, offset);
    if (reserved != 0 ||
        entity_count > kMaxSnapshotEntities ||
        decoded.header.payload_size != kSnapshotPrefixSize + entity_count * kEntitySnapshotSize) {
        decoded.error = DecodeError::InvalidPacketPayload;
        return decoded;
    }

    decoded.payload.entities.reserve(entity_count);
    for (std::uint16_t i = 0; i < entity_count; ++i) {
        decoded.payload.entities.push_back(ReadEntitySnapshot(data, offset));
    }
    return decoded;
}

std::vector<std::uint8_t> EncodePing(std::uint32_t sequence, const PingPayload& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + kPingPayloadSize);
    WriteHeader(out, PacketType::Ping, sequence, kPingPayloadSize);
    WriteU64Le(out, payload.timestamp_usec);
    return out;
}

std::vector<std::uint8_t> EncodePong(std::uint32_t sequence, const PongPayload& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + kPongPayloadSize);
    WriteHeader(out, PacketType::Pong, sequence, kPongPayloadSize);
    WriteU64Le(out, payload.timestamp_usec);
    WriteU64Le(out, payload.server_timestamp_usec);
    return out;
}

DecodedHeader DecodeHeaderOnly(const std::uint8_t* data, std::size_t size) {
    return DecodeHeaderInternal(data, size);
}

}  // namespace xdpg
