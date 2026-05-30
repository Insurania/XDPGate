#include "protocol/xdg_protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace xdpg {
namespace {

constexpr std::size_t kInputPayloadSize = 32;
constexpr std::size_t kSnapshotPrefixSize = kSnapshotPayloadHeaderSize;
constexpr std::size_t kEntitySnapshotSize = kEntitySnapshotWireSize;
constexpr std::size_t kPingPayloadSize = 8;
constexpr std::size_t kPongPayloadSize = 16;
constexpr float kQuaternionSmallestThreeLimit = 0.7071067811865476f;
constexpr std::uint32_t kSmallCubeDenseIndexBase = kMaxPlayerEntities;

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

float ClampFloat(float value, float min_value, float max_value) {
    return std::max(min_value, std::min(value, max_value));
}

std::uint16_t QuantizeU16(float value, float min_value, float max_value) {
    // 使用 round 而不是截断，让解码后的平均误差接近 0。范围外的位置会被钳制；
    // 这比让 uint16 溢出安全，也方便后续在 stats 中统计 out-of-bounds。
    const float clamped = ClampFloat(value, min_value, max_value);
    const float normalized = (clamped - min_value) / (max_value - min_value);
    return static_cast<std::uint16_t>(std::lround(normalized * 65535.0f));
}

float DequantizeU16(std::uint16_t value, float min_value, float max_value) {
    const float normalized = static_cast<float>(value) / 65535.0f;
    return min_value + normalized * (max_value - min_value);
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

bool IsKnownSnapshotEncodingMode(std::uint16_t value) {
    switch (static_cast<SnapshotEncodingMode>(value)) {
        case SnapshotEncodingMode::Full:
        case SnapshotEncodingMode::DeltaOffsetU8:
        case SnapshotEncodingMode::DeltaOffsetU16:
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

std::array<float, 4> NormalizedQuaternion(const float rotation[4]) {
    const float length_sq =
        rotation[0] * rotation[0] + rotation[1] * rotation[1] +
        rotation[2] * rotation[2] + rotation[3] * rotation[3];
    if (length_sq <= 0.000001f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }

    const float inv_length = 1.0f / std::sqrt(length_sq);
    return {
        rotation[0] * inv_length,
        rotation[1] * inv_length,
        rotation[2] * inv_length,
        rotation[3] * inv_length,
    };
}

void WriteCompressedPosition(std::vector<std::uint8_t>& out, const float position[3]) {
    WriteU16Le(out, QuantizeU16(position[0], kSnapshotHorizontalMinMeters,
                                kSnapshotHorizontalMaxMeters));
    WriteU16Le(out, QuantizeU16(position[1], kSnapshotVerticalMinMeters,
                                kSnapshotVerticalMaxMeters));
    WriteU16Le(out, QuantizeU16(position[2], kSnapshotHorizontalMinMeters,
                                kSnapshotHorizontalMaxMeters));
}

void ReadCompressedPosition(const std::uint8_t* data, std::size_t& offset, float position[3]) {
    position[0] = DequantizeU16(ReadU16Le(data, offset), kSnapshotHorizontalMinMeters,
                                kSnapshotHorizontalMaxMeters);
    position[1] = DequantizeU16(ReadU16Le(data, offset), kSnapshotVerticalMinMeters,
                                kSnapshotVerticalMaxMeters);
    position[2] = DequantizeU16(ReadU16Le(data, offset), kSnapshotHorizontalMinMeters,
                                kSnapshotHorizontalMaxMeters);
}

void WriteCompressedRotation(std::vector<std::uint8_t>& out, const float rotation[4]) {
    auto q = NormalizedQuaternion(rotation);

    std::uint8_t largest_index = 0;
    float largest_abs = std::fabs(q[0]);
    for (std::uint8_t i = 1; i < 4; ++i) {
        const float value_abs = std::fabs(q[i]);
        if (value_abs > largest_abs) {
            largest_abs = value_abs;
            largest_index = i;
        }
    }

    // 四元数 q 和 -q 表示同一个旋转。编码前把被省略的最大项翻成非负，
    // 解码端就不需要额外发送符号位，只要用单位长度约束恢复正值即可。
    if (q[largest_index] < 0.0f) {
        for (float& value : q) {
            value = -value;
        }
    }

    WriteU8(out, largest_index);
    for (std::uint8_t i = 0; i < 4; ++i) {
        if (i == largest_index) {
            continue;
        }
        WriteU16Le(out, QuantizeU16(q[i], -kQuaternionSmallestThreeLimit,
                                    kQuaternionSmallestThreeLimit));
    }
}

bool ReadCompressedRotation(const std::uint8_t* data, std::size_t& offset, float rotation[4]) {
    const std::uint8_t largest_index = ReadU8(data, offset);
    if (largest_index >= 4) {
        return false;
    }

    float sum_sq = 0.0f;
    for (std::uint8_t i = 0; i < 4; ++i) {
        if (i == largest_index) {
            continue;
        }
        const float value =
            DequantizeU16(ReadU16Le(data, offset), -kQuaternionSmallestThreeLimit,
                          kQuaternionSmallestThreeLimit);
        rotation[i] = value;
        sum_sq += value * value;
    }

    rotation[largest_index] = std::sqrt(std::max(0.0f, 1.0f - sum_sq));
    return true;
}

std::uint32_t DenseIndexFromEntityId(std::uint32_t entity_id) {
    if (entity_id >= kFirstPlayerEntityId &&
        entity_id < kFirstPlayerEntityId + kMaxPlayerEntities) {
        return entity_id - kFirstPlayerEntityId;
    }
    if (entity_id >= kSmallCubeEntityIdBase) {
        return kSmallCubeDenseIndexBase + (entity_id - kSmallCubeEntityIdBase);
    }
    throw std::invalid_argument("entity_id cannot be represented as dense snapshot index");
}

EntitySnapshot EntityFromDenseIndex(std::uint32_t dense_index) {
    EntitySnapshot entity;
    if (dense_index < kMaxPlayerEntities) {
        entity.entity_id = kFirstPlayerEntityId + dense_index;
        entity.entity_type = EntityType::PlayerCube;
    } else {
        entity.entity_id = kSmallCubeEntityIdBase + (dense_index - kSmallCubeDenseIndexBase);
        entity.entity_type = EntityType::SmallCube;
    }
    return entity;
}

void WriteEntityState(std::vector<std::uint8_t>& out, const EntitySnapshot& entity) {
    WriteU16Le(out, entity.flags);
    WriteCompressedPosition(out, entity.position);
    WriteCompressedRotation(out, entity.rotation);
}

bool ReadEntityState(const std::uint8_t* data, std::size_t& offset, EntitySnapshot* entity) {
    entity->flags = ReadU16Le(data, offset);
    ReadCompressedPosition(data, offset, entity->position);
    return ReadCompressedRotation(data, offset, entity->rotation);
}

void WriteFullEntitySnapshot(std::vector<std::uint8_t>& out, const EntitySnapshot& entity) {
    WriteU32Le(out, entity.entity_id);
    WriteU16Le(out, static_cast<std::uint16_t>(entity.entity_type));
    WriteEntityState(out, entity);
}

bool ReadFullEntitySnapshot(const std::uint8_t* data, std::size_t& offset, EntitySnapshot* out) {
    if (out == nullptr) {
        return false;
    }

    EntitySnapshot entity;
    entity.entity_id = ReadU32Le(data, offset);
    entity.entity_type = static_cast<EntityType>(ReadU16Le(data, offset));
    if (!ReadEntityState(data, offset, &entity)) {
        return false;
    }
    *out = entity;
    return true;
}

void WriteDeltaEntitySnapshot(
    std::vector<std::uint8_t>& out,
    SnapshotEncodingMode mode,
    const EntitySnapshot& entity,
    std::int64_t* previous_dense_index) {
    const std::uint32_t dense_index = DenseIndexFromEntityId(entity.entity_id);
    if (static_cast<std::int64_t>(dense_index) <= *previous_dense_index) {
        throw std::invalid_argument("delta snapshot entities must be sorted by dense index");
    }
    const auto relative_offset =
        static_cast<std::uint32_t>(static_cast<std::int64_t>(dense_index) - *previous_dense_index - 1);
    if (mode == SnapshotEncodingMode::DeltaOffsetU8) {
        if (relative_offset > UINT8_MAX) {
            throw std::invalid_argument("delta offset does not fit in u8");
        }
        WriteU8(out, static_cast<std::uint8_t>(relative_offset));
    } else if (mode == SnapshotEncodingMode::DeltaOffsetU16) {
        if (relative_offset > UINT16_MAX) {
            throw std::invalid_argument("delta offset does not fit in u16");
        }
        WriteU16Le(out, static_cast<std::uint16_t>(relative_offset));
    } else {
        throw std::invalid_argument("invalid delta snapshot encoding mode");
    }

    WriteEntityState(out, entity);
    *previous_dense_index = dense_index;
}

bool ReadDeltaEntitySnapshot(
    const std::uint8_t* data,
    std::size_t& offset,
    SnapshotEncodingMode mode,
    std::int64_t* previous_dense_index,
    EntitySnapshot* out) {
    std::uint32_t relative_offset = 0;
    if (mode == SnapshotEncodingMode::DeltaOffsetU8) {
        relative_offset = ReadU8(data, offset);
    } else if (mode == SnapshotEncodingMode::DeltaOffsetU16) {
        relative_offset = ReadU16Le(data, offset);
    } else {
        return false;
    }

    const auto dense_index =
        static_cast<std::uint32_t>(*previous_dense_index + relative_offset + 1);
    EntitySnapshot entity = EntityFromDenseIndex(dense_index);
    if (!ReadEntityState(data, offset, &entity)) {
        return false;
    }

    *previous_dense_index = dense_index;
    *out = entity;
    return true;
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

const char* SnapshotEncodingModeName(SnapshotEncodingMode mode) {
    switch (mode) {
        case SnapshotEncodingMode::Full:
            return "FULL";
        case SnapshotEncodingMode::DeltaOffsetU8:
            return "DELTA_OFFSET_U8";
        case SnapshotEncodingMode::DeltaOffsetU16:
            return "DELTA_OFFSET_U16";
    }
    return "UNKNOWN";
}

std::size_t SnapshotEntityWireSize(SnapshotEncodingMode mode) {
    switch (mode) {
        case SnapshotEncodingMode::Full:
            return kEntitySnapshotWireSize;
        case SnapshotEncodingMode::DeltaOffsetU8:
            return kDeltaSnapshotOffsetU8WireSize;
        case SnapshotEncodingMode::DeltaOffsetU16:
            return kDeltaSnapshotOffsetU16WireSize;
    }
    throw std::invalid_argument("unknown snapshot encoding mode");
}

std::size_t MaxSnapshotEntitiesPerPacket(SnapshotEncodingMode mode) {
    return (kMaxUdpPayloadSize - kHeaderSize - kSnapshotPayloadHeaderSize) /
           SnapshotEntityWireSize(mode);
}

std::uint32_t SnapshotDenseIndexFromEntityId(std::uint32_t entity_id) {
    return DenseIndexFromEntityId(entity_id);
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
    if (payload.entities.size() > MaxSnapshotEntitiesPerPacket(payload.encoding_mode)) {
        throw std::invalid_argument("too many snapshot entities in one packet");
    }
    if (payload.chunk_count == 0 || payload.chunk_index >= payload.chunk_count) {
        throw std::invalid_argument("invalid snapshot chunk index/count");
    }
    if (payload.total_entity_count < payload.entities.size()) {
        throw std::invalid_argument("invalid snapshot total entity count");
    }

    const std::size_t payload_size =
        kSnapshotPrefixSize + payload.entities.size() * SnapshotEntityWireSize(payload.encoding_mode);
    if (kHeaderSize + payload_size > kMaxUdpPayloadSize) {
        throw std::invalid_argument("snapshot exceeds maximum UDP payload size");
    }

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload_size);
    WriteHeader(out, PacketType::Snapshot, sequence, CheckedPayloadSize(payload_size));
    WriteU64Le(out, payload.server_tick);
    WriteU32Le(out, payload.last_processed_input_sequence);
    WriteU16Le(out, payload.chunk_index);
    WriteU16Le(out, payload.chunk_count);
    WriteU16Le(out, payload.total_entity_count);
    WriteU16Le(out, static_cast<std::uint16_t>(payload.entities.size()));
    WriteU16Le(out, static_cast<std::uint16_t>(payload.encoding_mode));
    WriteU16Le(out, 0);

    if (payload.encoding_mode == SnapshotEncodingMode::Full) {
        for (const auto& entity : payload.entities) {
            WriteFullEntitySnapshot(out, entity);
        }
    } else {
        std::int64_t previous_dense_index = -1;
        for (const auto& entity : payload.entities) {
            WriteDeltaEntitySnapshot(out, payload.encoding_mode, entity, &previous_dense_index);
        }
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
    decoded.payload.chunk_index = ReadU16Le(data, offset);
    decoded.payload.chunk_count = ReadU16Le(data, offset);
    decoded.payload.total_entity_count = ReadU16Le(data, offset);
    const auto entity_count = ReadU16Le(data, offset);
    const auto encoding_mode_raw = ReadU16Le(data, offset);
    const auto reserved1 = ReadU16Le(data, offset);
    if (!IsKnownSnapshotEncodingMode(encoding_mode_raw) ||
        reserved1 != 0 ||
        decoded.payload.chunk_count == 0 ||
        decoded.payload.chunk_index >= decoded.payload.chunk_count ||
        decoded.payload.total_entity_count < entity_count) {
        decoded.error = DecodeError::InvalidPacketPayload;
        return decoded;
    }

    decoded.payload.encoding_mode = static_cast<SnapshotEncodingMode>(encoding_mode_raw);
    const std::size_t entity_wire_size = SnapshotEntityWireSize(decoded.payload.encoding_mode);
    if (entity_count > MaxSnapshotEntitiesPerPacket(decoded.payload.encoding_mode) ||
        decoded.header.payload_size != kSnapshotPrefixSize + entity_count * entity_wire_size) {
        decoded.error = DecodeError::InvalidPacketPayload;
        return decoded;
    }

    decoded.payload.entities.reserve(entity_count);
    if (decoded.payload.encoding_mode == SnapshotEncodingMode::Full) {
        for (std::uint16_t i = 0; i < entity_count; ++i) {
            EntitySnapshot entity;
            if (!ReadFullEntitySnapshot(data, offset, &entity)) {
                decoded.error = DecodeError::InvalidPacketPayload;
                decoded.payload.entities.clear();
                return decoded;
            }
            decoded.payload.entities.push_back(entity);
        }
    } else {
        std::int64_t previous_dense_index = -1;
        for (std::uint16_t i = 0; i < entity_count; ++i) {
            EntitySnapshot entity;
            if (!ReadDeltaEntitySnapshot(data, offset, decoded.payload.encoding_mode,
                                         &previous_dense_index, &entity)) {
                decoded.error = DecodeError::InvalidPacketPayload;
                decoded.payload.entities.clear();
                return decoded;
            }
            decoded.payload.entities.push_back(entity);
        }
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

DecodedPing DecodePing(const std::uint8_t* data, std::size_t size) {
    DecodedPing decoded;

    // PING/PONG 走同一套 header 校验，保证 XDP/用户态看到的 magic/version/size 规则一致。
    auto header = DecodeHeaderInternal(data, size);
    decoded.header = header.header;
    decoded.error = header.error;
    if (decoded.error != DecodeError::None) {
        return decoded;
    }
    if (decoded.header.packet_type != PacketType::Ping ||
        decoded.header.payload_size != kPingPayloadSize) {
        decoded.error = DecodeError::InvalidPacketPayload;
        return decoded;
    }

    std::size_t offset = kHeaderSize;
    decoded.payload.timestamp_usec = ReadU64Le(data, offset);
    return decoded;
}

std::vector<std::uint8_t> EncodePong(std::uint32_t sequence, const PongPayload& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + kPongPayloadSize);
    WriteHeader(out, PacketType::Pong, sequence, kPongPayloadSize);
    WriteU64Le(out, payload.timestamp_usec);
    WriteU64Le(out, payload.server_timestamp_usec);
    return out;
}

DecodedPong DecodePong(const std::uint8_t* data, std::size_t size) {
    DecodedPong decoded;

    // PONG 当前主要给 tools/client 使用；server 暂时不会主动消费 PONG。
    auto header = DecodeHeaderInternal(data, size);
    decoded.header = header.header;
    decoded.error = header.error;
    if (decoded.error != DecodeError::None) {
        return decoded;
    }
    if (decoded.header.packet_type != PacketType::Pong ||
        decoded.header.payload_size != kPongPayloadSize) {
        decoded.error = DecodeError::InvalidPacketPayload;
        return decoded;
    }

    std::size_t offset = kHeaderSize;
    decoded.payload.timestamp_usec = ReadU64Le(data, offset);
    decoded.payload.server_timestamp_usec = ReadU64Le(data, offset);
    return decoded;
}

DecodedHeader DecodeHeaderOnly(const std::uint8_t* data, std::size_t size) {
    return DecodeHeaderInternal(data, size);
}

}  // namespace xdpg
