#pragma once

#include "xdpg/physics/input_command.h"
#include "xdpg/physics/ode_world.h"
#include "xdpg/server/net/udp_socket.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace xdpg::server {

// DS server 第一阶段的运行参数。
// 当前配置故意保持很小：一个 UDP 端口、一个固定 tick rate、一个 snapshot rate。
// 后续做 benchmark 时，再在这里扩展 recvfrom/recvmmsg、SO_REUSEPORT worker 等模式。
struct DsServerConfig {
    std::uint16_t port = 40000;
    std::uint32_t tick_rate = 60;
    std::uint32_t snapshot_rate = 20;

    // snapshot v1 单包最多 16 个 entity。server 默认 1 个 player + 15 个 small cube，
    // 避免 UDP payload 超过 1200 字节并触发 IP 分片。
    std::uint32_t small_cube_count = 15;
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

    void DrainSocket();
    void HandlePacket(const std::uint8_t* data, std::size_t size, const Endpoint& from);
    void HandleInput(const std::uint8_t* data, std::size_t size, const Endpoint& from);
    void HandlePing(const std::uint8_t* data, std::size_t size, const Endpoint& from);
    void StepSimulation();
    void SendSnapshot();
    void LogStatsIfDue();

    DsServerConfig config_;
    UdpSocket socket_;

    // 服务端持有唯一权威物理世界。客户端 input 不能直接改 entity transform，
    // 只能通过 latest_input_ 影响下一次 fixed tick 中施加的力/速度变化。
    physics::OdeWorld world_;

    // 第一版只保留“最近一次输入”。这足够验证控制链路，但不是最终网络模型。
    // 后续做 prediction/reconciliation 时，应改成按 input_sequence 排队消费。
    physics::InputCommand latest_input_;

    // 最近发送合法 INPUT/PING 的客户端地址。snapshot 只发给这个 endpoint。
    // 多客户端支持会把这里替换成 client table。
    Endpoint latest_client_;
    bool has_client_ = false;

    // 发送方向的 packet sequence。它是 server-local 序号，不等同于 input_sequence。
    std::uint32_t outbound_sequence_ = 1;

    Counters counters_;
    Counters last_logged_counters_;
    std::chrono::steady_clock::time_point next_stats_log_;
};

}  // namespace xdpg::server
