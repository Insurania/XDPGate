#pragma once

#include "xdpg/physics/input_command.h"
#include "xdpg/physics/ode_world.h"
#include "xdpg/net/udp_socket.h"
#include "protocol/xdg_protocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace xdpg::server {

// DS server 第一阶段的运行参数。
// 当前配置故意保持很小：一个 UDP 端口、一个固定 tick rate、一个 snapshot rate。
// 后续做 benchmark 时，再在这里扩展 recvfrom/recvmmsg、SO_REUSEPORT worker 等模式。
struct DsServerConfig {
    std::uint16_t port = 40000;
    std::uint32_t tick_rate = 60;
    std::uint32_t snapshot_rate = 60;

    // server 默认和本地 ODE viewer 保持一致：1 个 player + 180 个 small cube。
    // UDP snapshot 通过 chunk 分包发送，避免单包超过 1200 字节。
    std::uint32_t small_cube_count = 180;
};

// 最小权威 DS：
// - 从 UDP 收 INPUT/PING；
// - 用 fixed timestep 推进 ODE world；
// - 按 snapshot_rate 把权威状态发回最近一个 client endpoint。
//
// 这里暂时只支持一个活跃 client。多 client、房间、广播、可靠层都留到后续阶段，
// 这样第一阶段可以专注验证“协议 -> 输入 -> ODE -> snapshot”的主链路。
class DsServer {
public:
    explicit DsServer(DsServerConfig config);

    // 主循环会一直运行，直到 stop_requested 被信号处理函数置为 true。
    // 返回 0 表示正常退出，非 0 表示启动 socket 等基础设施失败。
    int Run(const std::atomic_bool& stop_requested);

private:
    // 这些 counter 只做低成本运行观测。第一阶段先每秒打印一次；
    // 后续 benchmark 可以把它们写入文件或暴露给 stats 工具。
    struct Counters {
        std::uint64_t received_packets = 0;
        std::uint64_t valid_inputs = 0;
        std::uint64_t invalid_packets = 0;
        std::uint64_t sent_snapshots = 0;
        std::uint64_t sent_pongs = 0;
    };

    struct ClientRecord {
        net::Endpoint endpoint;
        std::string endpoint_key;
        std::chrono::steady_clock::time_point last_seen;
        std::uint64_t client_id = 0;
        std::uint32_t player_entity_id = 0;
        physics::InputCommand latest_input;
    };

    void DrainSocket();
    void HandlePacket(const std::uint8_t* data, std::size_t size, const net::Endpoint& from);
    void HandleInput(const std::uint8_t* data, std::size_t size, const net::Endpoint& from);
    void HandlePing(const std::uint8_t* data, std::size_t size, const net::Endpoint& from);
    void StepSimulation();
    void SendSnapshot();
    void LogStatsIfDue();
    ClientRecord* FindClient(const net::Endpoint& endpoint);
    ClientRecord* RegisterInputClient(const net::Endpoint& endpoint, std::uint64_t client_id);
    void RemoveStaleClients();
    std::vector<xdpg::EntitySnapshot> BuildCurrentSnapshotEntities() const;
    std::vector<xdpg::EntitySnapshot> BuildSnapshotForClient(
        const std::vector<xdpg::EntitySnapshot>& entities,
        const ClientRecord& client) const;
    std::vector<xdpg::EntitySnapshot> CollectChangedEntities(
        const std::vector<xdpg::EntitySnapshot>& current) const;
    xdpg::SnapshotEncodingMode ChooseSnapshotEncoding(
        const std::vector<xdpg::EntitySnapshot>& current,
        const std::vector<xdpg::EntitySnapshot>& changed) const;

    DsServerConfig config_;
    net::UdpSocket socket_;

    // 服务端持有唯一权威物理世界。客户端 input 不能直接改 entity transform，
    // 只能通过 latest_input_ 影响下一次 fixed tick 中施加的力/速度变化。
    physics::OdeWorld world_;

    // session 表按 endpoint 区分客户端。每个新 endpoint 第一次发送 INPUT 时，
    // server 分配一个新的 PlayerCube；同一 endpoint 后续只更新自己的 latest_input。
    std::vector<ClientRecord> clients_;
    std::uint32_t next_player_entity_id_ = xdpg::kFirstPlayerEntityId;

    // 发送方向的 packet sequence。它是 server-local 序号，不等同于 input_sequence。
    std::uint32_t outbound_sequence_ = 1;
    std::vector<xdpg::EntitySnapshot> previous_snapshot_entities_;
    bool has_previous_snapshot_ = false;
    std::uint32_t snapshots_since_full_ = 0;

    Counters counters_;
    Counters last_logged_counters_;
    std::chrono::steady_clock::time_point next_stats_log_;
};

}  // namespace xdpg::server
