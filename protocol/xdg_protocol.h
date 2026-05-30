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

// snapshot v1 把完整状态控制在一个普通 MTU 以内，避免 IP 分片干扰后续网络实验。
// 如果需要更多 entity，应通过 chunked snapshot 或区域裁剪扩展协议，而不是直接加大包。
constexpr std::size_t kMaxUdpPayloadSize = 1200;
constexpr std::size_t kMaxSnapshotEntities = 16;

// EntitySnapshot::flags 的 bit 定义。先保留一个 interacting 状态，
// 用于表达小 cube 正在被玩家碰撞/吸引影响，viewer 可以据此变色。
constexpr std::uint16_t kEntityFlagInteracting = 1u << 0u;

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

    // sequence 是发送方本地 packet 序号，用于观察丢包和乱序；
    // 它和 input_sequence 不是同一个概念。
    std::uint32_t sequence = 0;
    std::uint16_t payload_size = 0;
    std::uint16_t flags = 0;
};

struct InputPayload {
    std::uint64_t client_id = 0;

    // 客户端输入命令序号。server snapshot 会回传最后处理到的 input_sequence，
    // 后续做 prediction/reconciliation 时会依赖这个字段。
    std::uint32_t input_sequence = 0;

    // 移动输入不是位置。server 会把它转换成作用在 player cube 上的 force/impulse。
    float move_x = 0.0f;
    float move_z = 0.0f;
    std::uint32_t buttons = 0;
    std::uint64_t client_timestamp_usec = 0;
};

struct EntitySnapshot {
    std::uint32_t entity_id = 0;
    EntityType entity_type = EntityType::SmallCube;

    // bitset，当前只定义 kEntityFlagInteracting。不要把它当成颜色本身；
    // 颜色是 viewer 策略，flags 表示服务端状态。
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
    // client/tool 发出的本地时间戳。server PONG 会原样带回。
    std::uint64_t timestamp_usec = 0;
};

struct PongPayload {
    std::uint64_t timestamp_usec = 0;

    // server 收到 PING 并编码 PONG 时的 steady timestamp，用于粗略观测 server 活性。
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

struct DecodedPing {
    PacketHeader header;
    PingPayload payload;
    DecodeError error = DecodeError::None;
};

struct DecodedPong {
    PacketHeader header;
    PongPayload payload;
    DecodeError error = DecodeError::None;
};

const char* DecodeErrorName(DecodeError error);
const char* PacketTypeName(PacketType packet_type);

std::vector<std::uint8_t> EncodeInput(std::uint32_t sequence, const InputPayload& payload);
DecodedInput DecodeInput(const std::uint8_t* data, std::size_t size);

std::vector<std::uint8_t> EncodeSnapshot(std::uint32_t sequence, const SnapshotPayload& payload);
DecodedSnapshot DecodeSnapshot(const std::uint8_t* data, std::size_t size);

std::vector<std::uint8_t> EncodePing(std::uint32_t sequence, const PingPayload& payload);
DecodedPing DecodePing(const std::uint8_t* data, std::size_t size);

std::vector<std::uint8_t> EncodePong(std::uint32_t sequence, const PongPayload& payload);
DecodedPong DecodePong(const std::uint8_t* data, std::size_t size);

DecodedHeader DecodeHeaderOnly(const std::uint8_t* data, std::size_t size);

}  // namespace xdpg
