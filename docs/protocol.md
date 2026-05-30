# XDPGate 协议 v1

XDPGate 使用小型二进制 UDP 协议。协议格式刻意保持简单，方便后续 XDP 程序只
读取 UDP payload 开头几个字节，就能完成早期过滤。

所有多字节整数字段均使用 little-endian（小端序）。浮点数字段使用 IEEE-754
`float32`。

## UDP Payload 布局

每个 UDP payload 都以固定协议头开始：

```text
XdgPacketHeader
payload bytes
```

服务端和工具不能假设收到的 buffer 一定合法。每个 packet 在解码前都必须先
通过长度、magic、version、packet type 和 payload size 校验。

## 公共 Header

```c
struct XdgPacketHeader {
    char     magic[4];      // "XDPG"
    uint8_t  version;       // 1
    uint8_t  packet_type;   // 见 packet type
    uint16_t header_size;   // v1 固定为 16
    uint32_t sequence;      // 发送方本地递增序号
    uint16_t payload_size;  // header 之后的 payload 字节数
    uint16_t flags;         // v1 保留字段，必须为 0
};
```

Header 大小：16 字节。

Magic bytes：

```text
X D P G
0x58 0x44 0x50 0x47
```

## Packet Type

```text
1 = INPUT
2 = SNAPSHOT
3 = PING
4 = PONG
5 = BENCHMARK
```

用户态 server 遇到未知 packet type 时应忽略。XDP 第一版不需要深度校验 packet
type，只要 header 合法即可。

## INPUT Packet

方向：

```text
client -> server
```

Payload：

```c
struct XdgInputPayload {
    uint64_t client_id;
    uint32_t input_sequence;
    float    move_x;
    float    move_z;
    uint32_t buttons;
    uint64_t client_timestamp_usec;
};
```

大小：32 字节。

字段含义：

- `client_id`：简单实验用的稳定 client 标识。
- `input_sequence`：单调递增的 input command 序号。
- `move_x`：世界 X 轴移动输入，通常限制在 `[-1, 1]`。
- `move_z`：世界 Z 轴移动输入，通常限制在 `[-1, 1]`。
- `buttons`：动作 bitset，例如 impulse、jump、boost。
- `client_timestamp_usec`：客户端时间戳，后续用于延迟观测。

第一阶段按钮定义：

```text
bit 0 = impulse / boost
```

服务端不能根据 input 直接设置 player cube 的位置。input 只能影响权威 ODE body
上的 force 或 impulse。

## SNAPSHOT Packet

方向：

```text
server -> client/viewer
```

Payload 前缀：

```c
struct XdgSnapshotPayloadHeader {
    uint64_t server_tick;
    uint32_t last_processed_input_sequence;
    uint16_t entity_count;
    uint16_t reserved;
};
```

大小：16 字节。

随后跟随 `entity_count` 个 entity record：

```c
struct XdgEntitySnapshot {
    uint32_t entity_id;
    uint16_t entity_type;
    uint16_t flags;
    float    position[3];
    float    rotation[4];          // quaternion: x, y, z, w
    float    linear_velocity[3];
    float    angular_velocity[3];
};
```

Entity record 大小：60 字节。

Entity type：

```text
1 = player_cube
2 = small_cube
3 = static_ground
```

第一阶段 snapshot 可以不发送无限 ground plane，因为 viewer 可以本地绘制地面。
动态 cube 必须发送。

Snapshot payload 大小：

```text
16 + entity_count * 60
```

## PING Packet

方向：

```text
client/tool -> server
```

Payload：

```c
struct XdgPingPayload {
    uint64_t timestamp_usec;
};
```

服务端收到后应返回 PONG，并原样带回 `timestamp_usec`。

## PONG Packet

方向：

```text
server -> client/tool
```

Payload：

```c
struct XdgPongPayload {
    uint64_t timestamp_usec;
    uint64_t server_timestamp_usec;
};
```

## BENCHMARK Packet

方向：

```text
tool -> server
```

第一阶段不需要实现 benchmark packet 行为。提前保留该 packet type，是为了后续
packet generator 可以生成看起来合法的 benchmark 流量。

未来 payload 可参考：

```c
struct XdgBenchmarkPayload {
    uint64_t client_id;
    uint32_t stream_id;
    uint32_t packet_index;
    uint64_t send_timestamp_usec;
};
```

## 大小限制

第一阶段建议限制：

```text
最小 UDP payload: 16 字节
最大 UDP payload: 1200 字节
单个 snapshot 最大 entity 数: 16
```

1200 字节可以比较稳妥地避开常见 MTU 下的 IP 分片。

16 个 entity 时：

```text
header 16 + snapshot prefix 16 + 16 * 60 = 992 字节
```

## 校验规则

用户态 decoder 应检查：

1. 收到长度至少为 16 字节。
2. `magic == "XDPG"`。
3. `version == 1`。
4. `header_size == 16`。
5. `payload_size == received_length - 16`。
6. v1 中 `flags == 0`。
7. packet-specific payload size 合法。

XDP 第一版只检查：

1. IPv4 UDP packet。
2. UDP dst port 是配置的 DS 端口。
3. UDP payload length 在允许范围内。
4. payload 前四个字节为 `"XDPG"`。
5. version byte 为 `1`。

XDP 不能解析完整 snapshot 或 input payload。
