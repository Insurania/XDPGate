#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xdpg {

// 协议常量保持集中定义，后续 server、client、packet generator 和 XDP loader
// 都应引用这里的值，避免多个模块各自手写 magic/version 导致不一致。
constexpr char kMagic[4] = {'X', 'D', 'P', 'G'};
constexpr std::uint8_t kVersion = 1;
constexpr std::uint16_t kHeaderSize = 16;
constexpr std::size_t kMaxUdpPayloadSize = 1200;
constexpr std::size_t kMaxSnapshotEntities = 16;

enum class PacketType : std::uint8_t {
    Input = 1,
    Snapshot = 2,
    Ping = 3,
    Pong = 4,
    Benchmark = 5,
};

enum class EntityType : std::uint16_t {
    PlayerCube = 1,
    SmallCube = 2,
    StaticGround = 3,
};

enum class DecodeError {
    None,
    TooShort,
    TooLarge,
    InvalidMagic,
    InvalidVersion,
    InvalidHeaderSize,
    InvalidPayloadSize,
    InvalidFlags,
    UnknownPacketType,
    InvalidPacketPayload,
};

struct PacketHeader {
    PacketType packet_type = PacketType::Input;
    std::uint32_t sequence = 0;
    std::uint16_t payload_size = 0;
    std::uint16_t flags = 0;
};

struct InputPayload {
    std::uint64_t client_id = 0;
    std::uint32_t input_sequence = 0;
    float move_x = 0.0f;
    float move_z = 0.0f;
    std::uint32_t buttons = 0;
    std::uint64_t client_timestamp_usec = 0;
};

struct EntitySnapshot {
    std::uint32_t entity_id = 0;
    EntityType entity_type = EntityType::SmallCube;
    std::uint16_t flags = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float linear_velocity[3] = {0.0f, 0.0f, 0.0f};
    float angular_velocity[3] = {0.0f, 0.0f, 0.0f};
};

struct SnapshotPayload {
    std::uint64_t server_tick = 0;
    std::uint32_t last_processed_input_sequence = 0;
    std::vector<EntitySnapshot> entities;
};

struct PingPayload {
    std::uint64_t timestamp_usec = 0;
};

struct PongPayload {
    std::uint64_t timestamp_usec = 0;
    std::uint64_t server_timestamp_usec = 0;
};

struct DecodedHeader {
    PacketHeader header;
    DecodeError error = DecodeError::None;
};

struct DecodedInput {
    PacketHeader header;
    InputPayload payload;
    DecodeError error = DecodeError::None;
};

struct DecodedSnapshot {
    PacketHeader header;
    SnapshotPayload payload;
    DecodeError error = DecodeError::None;
};

const char* DecodeErrorName(DecodeError error);
const char* PacketTypeName(PacketType packet_type);

std::vector<std::uint8_t> EncodeInput(std::uint32_t sequence, const InputPayload& payload);
DecodedInput DecodeInput(const std::uint8_t* data, std::size_t size);

std::vector<std::uint8_t> EncodeSnapshot(std::uint32_t sequence, const SnapshotPayload& payload);
DecodedSnapshot DecodeSnapshot(const std::uint8_t* data, std::size_t size);

std::vector<std::uint8_t> EncodePing(std::uint32_t sequence, const PingPayload& payload);
std::vector<std::uint8_t> EncodePong(std::uint32_t sequence, const PongPayload& payload);

DecodedHeader DecodeHeaderOnly(const std::uint8_t* data, std::size_t size);

}  // namespace xdpg

